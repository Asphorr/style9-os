/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddb.h"
#include "intr.h"
#include "kprintf.h"
#include "ksym.h"
#include "panic.h"
#include "pic.h"
#include "port.h"
#include "port_internal.h"
#include "sched.h"
#include "smap.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"
#include "tty.h"

static irq_handler_t	irq_handlers[16];

static const char *const exception_names[32] = {
	"#DE divide-by-zero",
	"#DB debug",
	"NMI",
	"#BP breakpoint",
	"#OF overflow",
	"#BR BOUND range exceeded",
	"#UD invalid opcode",
	"#NM device not available",
	"#DF double fault",
	"coprocessor segment overrun",
	"#TS invalid TSS",
	"#NP segment not present",
	"#SS stack-segment fault",
	"#GP general protection",
	"#PF page fault",
	"reserved (15)",
	"#MF x87 FP error",
	"#AC alignment check",
	"#MC machine check",
	"#XM SIMD FP error",
	"#VE virtualisation",
	"#CP control protection",
	"reserved (22)",
	"reserved (23)",
	"reserved (24)",
	"reserved (25)",
	"reserved (26)",
	"reserved (27)",
	"reserved (28)",
	"reserved (29)",
	"reserved (30)",
	"reserved (31)",
};

static void	intr_panic(const struct trapframe *) __attribute__((noreturn));
static void	user_fault_die(struct trapframe *);
static uint32_t	deliver_exception_and_wait(struct port *exc,
		    const struct trapframe *tf,
		    uint32_t *rip_advance_out);
static void	pf_print_err(uint64_t err);
static void	rip_byte_dump(uint64_t rip);

/*
 * Map an x86 trap vector down to one of the EXC_TYPE_* indices used
 * by t->t_exc_ports[].  Any vector not explicitly listed falls into
 * EXC_TYPE_BAD_ACCESS -- it is the catch-all bucket, matching real
 * Mach's tendency to put unknown faults under bad-access semantics.
 */
static unsigned
exc_type_from_trapno(uint32_t trapno)
{

	switch (trapno) {
	case 3:		/* #BP -- INT3 breakpoint            */
		return (EXC_TYPE_BREAKPOINT);
	case 6:		/* #UD -- invalid opcode             */
	case 7:		/* #NM -- device-not-available       */
		return (EXC_TYPE_BAD_INSTRUCTION);
	case 0:		/* #DE -- divide error               */
	case 16:	/* #MF -- x87 floating-point error   */
	case 19:	/* #XM -- SIMD floating-point error  */
		return (EXC_TYPE_ARITHMETIC);
	default:
		return (EXC_TYPE_BAD_ACCESS);
	}
}

static inline uint64_t
read_cr2(void)
{
	uint64_t	v;

	__asm__ __volatile__ ("mov %%cr2, %0" : "=r"(v));
	return (v);
}

void
irq_install(unsigned int irq, irq_handler_t handler)
{

	if (irq < 16)
		irq_handlers[irq] = handler;
}

/*
 * Async-kill detection point #4: IRQ-return-to-user.  Catches a kill
 * issued against a task whose thread is doing a pure ring-3 compute
 * loop -- one that never voluntarily enters the kernel via syscall or
 * blocking IPC.  The timer IRQ (or any other hardware IRQ) brings the
 * thread into the kernel; the trampoline runs intr_dispatch; this
 * helper fires before the function returns and the asm iretq's back
 * to ring 3, retiring the thread instead of resuming user code.
 *
 * Gated on (tf_cs & 3) == 3 so kernel-mode IRQ interrupts (e.g. PIT
 * landing while the scheduler holds sched_lock) never call thread_exit.
 * kernel_task is skipped explicitly even though t_killed cannot be set
 * on it -- belt-and-braces, and makes the early-out cheaper than the
 * atomic load.
 */
static inline void
intr_check_async_kill_on_user_return(const struct trapframe *tf)
{

	if ((tf->tf_cs & 3) != 3)
		return;
	if (current_thread == NULL)
		return;
	if (current_thread->th_task == kernel_task)
		return;
	if (!task_kill_pending(current_thread->th_task))
		return;
	thread_exit();
	/* NOTREACHED */
}

/*
 * C-side trap dispatcher, called from isr_common with %rsp pointing at
 * the populated trapframe.  Vectors below 32 are CPU exceptions and
 * abort the kernel; vectors 32-47 are hardware IRQs and route through
 * the registered handler (or are simply EOI'd if none is installed).
 */
void
intr_dispatch(struct trapframe *tf)
{
	unsigned int	irq;

	if (tf->tf_trapno < 32) {
		/*
		 * Exceptions originating from ring 3 do not bring the
		 * kernel down: they retire the offending user thread
		 * instead, the way FreeBSD turns a user #PF into
		 * SIGSEGV-then-die.  The kernel keeps running, the
		 * shell stays interactive.
		 */
		if ((tf->tf_cs & 3) == 3) {
			user_fault_die(tf);
			/*
			 * RESUME path: user_fault_die mutated tf->tf_rip
			 * and returned.  Fall through to the IRQ-entry
			 * tail, which iretq's back to user mode with the
			 * new trapframe.  KILL path doesn't reach here --
			 * user_fault_die calls thread_exit() which is
			 * noreturn.
			 */
			intr_check_async_kill_on_user_return(tf);
			return;
		}
		intr_panic(tf);
		/* NOTREACHED */
	}

	if (tf->tf_trapno < 48) {
		irq = (unsigned int)(tf->tf_trapno - 32);
		if (irq_handlers[irq] != NULL)
			irq_handlers[irq](tf);
		pic_eoi(irq);

		/*
		 * Wake any thread whose mach_msg_recv_timed deadline has
		 * elapsed.  Runs here (not in pit_isr) because the
		 * spin_unlock inside sched_check_timeouts can drop
		 * preempt_count to zero with need_resched still set,
		 * which would yield -- doing that BEFORE pic_eoi leaves
		 * the 8259 holding the IRQ and blocks subsequent PIT
		 * ticks (the busy-sleep path observed this as an outright
		 * boot hang at stress_preempt).
		 */
		sched_check_timeouts();

		/*
		 * Preempt point.  Two pieces of deferred work may have
		 * been queued by the IRQ handler that just ran: any
		 * wake requests posted via sched_post_irq_wake, and a
		 * possible need_resched from PIT.  Service both only
		 * when preempt is enabled (no caller held a spinlock
		 * across the IRQ -- otherwise the deferred work fires
		 * at the next spin_unlock that drops the count to
		 * zero).
		 */
		if (preempt_is_enabled()) {
			sched_drain_irq_wakes();
			if (__atomic_load_n(&preempt_need_resched,
			    __ATOMIC_RELAXED)) {
				__atomic_store_n(&preempt_need_resched,
				    0, __ATOMIC_RELAXED);
				sched_count_preempt();
				thread_yield();
			}
		}
		intr_check_async_kill_on_user_return(tf);
		return;
	}

	/* Vector >= 48: spurious / not installed -- ignore quietly. */
	intr_check_async_kill_on_user_return(tf);
}

static void
intr_panic(const struct trapframe *tf)
{
	const char	*name;
	const char	*ring;
	uint64_t	 cr2;
	bool		 from_user;

	name = (tf->tf_trapno < 32)
	    ? exception_names[tf->tf_trapno]
	    : "(out-of-range)";

	from_user = (tf->tf_cs & 3) == 3;
	ring      = from_user ? "ring 3 (user)" : "ring 0 (kernel)";

	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	kprintf("\n*** %s [%s] (vec %u, err=0x%016lx)\n",
	    name, ring, (unsigned int)tf->tf_trapno,
	    (unsigned long)tf->tf_err);
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));

	if (tf->tf_trapno == 14) {	/* #PF */
		cr2 = read_cr2();
		kprintf("cr2=0x%016lx  (faulting VA)\n",
		    (unsigned long)cr2);
		pf_print_err((uint64_t)tf->tf_err);
	}

	kprintf("rip=");
	ksym_print((uint64_t)tf->tf_rip);
	kprintf("\n     cs=0x%04lx  rflags=0x%016lx\n",
	    (unsigned long)tf->tf_cs, (unsigned long)tf->tf_rflags);
	kprintf("rsp=0x%016lx  ss=0x%04lx\n",
	    (unsigned long)tf->tf_rsp, (unsigned long)tf->tf_ss);
	kprintf("rax=0x%016lx  rbx=0x%016lx\n",
	    (unsigned long)tf->tf_rax, (unsigned long)tf->tf_rbx);
	kprintf("rcx=0x%016lx  rdx=0x%016lx\n",
	    (unsigned long)tf->tf_rcx, (unsigned long)tf->tf_rdx);
	kprintf("rsi=0x%016lx  rdi=0x%016lx\n",
	    (unsigned long)tf->tf_rsi, (unsigned long)tf->tf_rdi);
	kprintf("rbp=0x%016lx  r8 =0x%016lx\n",
	    (unsigned long)tf->tf_rbp, (unsigned long)tf->tf_r8);
	kprintf("r9 =0x%016lx  r10=0x%016lx\n",
	    (unsigned long)tf->tf_r9,  (unsigned long)tf->tf_r10);
	kprintf("r11=0x%016lx  r12=0x%016lx\n",
	    (unsigned long)tf->tf_r11, (unsigned long)tf->tf_r12);
	kprintf("r13=0x%016lx  r14=0x%016lx  r15=0x%016lx\n",
	    (unsigned long)tf->tf_r13, (unsigned long)tf->tf_r14,
	    (unsigned long)tf->tf_r15);

	rip_byte_dump((uint64_t)tf->tf_rip);

	if (!from_user)
		backtrace_print((uintptr_t)tf->tf_rbp, 16);

	ddb_enter((uint64_t)tf->tf_rbp, tf);
	/* NOTREACHED -- ddb_enter is __dead. */
}

/*
 * Reply-protocol helper.  Allocates a kernel-owned reply port,
 * attaches it as an implicit msgh_local SEND descriptor on the
 * exception message, posts to the watcher, and parks the calling
 * thread on the kernel-side recv queue for up to EXC_REPLY_TIMEOUT_MS.
 *
 * Returns the verdict the watcher sent back (EXC_VERDICT_*).  Any
 * shortfall -- failed allocation, failed enqueue, timeout, malformed
 * reply -- collapses to EXC_VERDICT_KILL so the caller retires the
 * thread the same way the A v1 path would.  On EXC_VERDICT_RESUME
 * the byte count to advance past the faulting instruction lands in
 * *rip_advance_out (0 by default).
 *
 * The reply port lives only for the duration of this call: created
 * with RECV+SEND in kernel_space, transferred to the watcher as a
 * fresh SEND name in their space (so they can reply), then released
 * via port_deallocate(kernel_space, K) after we read the verdict.
 * The watcher's SEND keeps the port object alive a moment longer
 * (until they deallocate their name), but the kernel side never
 * touches it again.
 */
static uint32_t
deliver_exception_and_wait(struct port *exc, const struct trapframe *tf,
    uint32_t *rip_advance_out)
{
	struct mach_exception_reply	*reply;
	struct port			*reply_port;
	struct task			*t;
	uint8_t				 reply_buf[sizeof(*reply)];
	mach_port_name_t		 reply_name;
	uint64_t			 cr2;
	uint32_t			 verdict;
	int				 rv;

	*rip_advance_out = 0;
	t = (current_thread != NULL) ? current_thread->th_task : NULL;
	cr2 = (tf->tf_trapno == 14) ? read_cr2() : 0;

	reply_port = port_create();
	if (reply_port == NULL)
		return (EXC_VERDICT_KILL);
	rv = space_install(kernel_space, reply_port,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND, &reply_name);
	if (rv != MACH_MSG_OK) {
		port_free(reply_port);
		return (EXC_VERDICT_KILL);
	}

	rv = port_exception_post(exc,
	    (uint32_t)tf->tf_trapno, (uint32_t)tf->tf_err,
	    (uint64_t)tf->tf_rip, (uint64_t)tf->tf_rsp,
	    (uint64_t)tf->tf_rflags, cr2,
	    (uint64_t)(t != NULL ? t->t_id : 0),
	    reply_port);
	if (rv != MACH_MSG_OK) {
		(void)port_deallocate(kernel_space, reply_name);
		return (EXC_VERDICT_KILL);
	}

	rv = mach_msg_recv_timed(kernel_space, reply_name,
	    (struct mach_msg_header *)reply_buf, sizeof(reply_buf),
	    EXC_REPLY_TIMEOUT_MS);

	verdict = EXC_VERDICT_KILL;
	if (rv == MACH_MSG_OK) {
		reply = (struct mach_exception_reply *)reply_buf;
		if (reply->hdr.msgh_id == (uint32_t)MACH_EXC_REPLY) {
			verdict          = reply->er_verdict;
			*rip_advance_out = reply->er_rip_advance;
		}
	}

	(void)port_deallocate(kernel_space, reply_name);
	return (verdict);
}

/*
 * BSD-style fault retirement: a #PF / #GP / #UD / etc. that originates
 * in ring 3 prints an autopsy and then kills just the offending user
 * thread.  Sibling kernel threads (shell, kbd-drv, uart-drv) keep
 * running, mirroring the SIGSEGV-then-die model.
 *
 * If the faulting task has an exception port set
 * (Xr task_set_exception_port 9 via SYS_TASK_SET_EXC_PORT), the kernel
 * posts a MACH_EXC_FAULT message onto that port carrying enough state
 * for a debugger or crash reporter in another task to do an autopsy
 * of its own.  Delivery is best-effort; a full queue or a dead port
 * silently drops the message but never blocks fault retirement.
 *
 * When the watcher opted into the reply protocol (EXC_FLAG_RESUMABLE
 * on the owning task), the kernel parks the faulting thread on a
 * kernel-allocated reply port until the watcher's verdict comes back
 * -- EXC_VERDICT_RESUME advances tf->tf_rip and returns to user mode,
 * EXC_VERDICT_KILL retires the thread.  Without the flag, the post-
 * and-retire path of A v1 runs unchanged.
 */
static void
user_fault_die(struct trapframe *tf)
{
	struct task	*t;
	struct port	*exc;
	const char	*name;
	uint64_t	 cr2;
	uint32_t	 task_flags;
	uint32_t	 verdict;
	uint32_t	 rip_advance;

	name = (tf->tf_trapno < 32)
	    ? exception_names[tf->tf_trapno]
	    : "(out-of-range)";
	cr2 = (tf->tf_trapno == 14) ? read_cr2() : 0;

	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	kprintf("\n*** user fault: %s (vec %u, err=0x%lx)\n",
	    name, (unsigned int)tf->tf_trapno,
	    (unsigned long)tf->tf_err);
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));

	if (tf->tf_trapno == 14) {
		kprintf("  cr2 = 0x%lx (faulting VA)\n",
		    (unsigned long)cr2);
		pf_print_err((uint64_t)tf->tf_err);
	}

	kprintf("  rip=");
	ksym_print((uint64_t)tf->tf_rip);
	kprintf("\n  rsp=0x%lx  rbp=0x%lx\n",
	    (unsigned long)tf->tf_rsp,
	    (unsigned long)tf->tf_rbp);
	kprintf("  rax=0x%lx  rdi=0x%lx  rsi=0x%lx  rdx=0x%lx\n",
	    (unsigned long)tf->tf_rax,
	    (unsigned long)tf->tf_rdi,
	    (unsigned long)tf->tf_rsi,
	    (unsigned long)tf->tf_rdx);
	rip_byte_dump((uint64_t)tf->tf_rip);

	/*
	 * Resolve the destination port + take a local SEND ref so the
	 * port survives concurrent SYS_*_SET_EXC_PORTS replacement or
	 * task/thread teardown between here and the post.  Drop the
	 * local ref after delivery; the slot keeps its own ref
	 * independently.
	 *
	 * Dispatch order: thread-level slot first, task-level slot
	 * second.  The thread-level slot is the canonical "debugger
	 * attached to one thread" hook -- if a thread has nominated a
	 * watcher for the fault's type, the task-level slot is NOT
	 * consulted (mirrors real Mach's thread/task precedence).
	 *
	 * Locking note: the thread-level read is intentionally
	 * lock-free.  th->th_exc_ports[] is only ever mutated by
	 * SYS_THREAD_SET_EXC_PORTS, which operates on the calling
	 * thread itself (no API today sets another thread's slots).
	 * user_fault_die runs from a trap on that same thread, so the
	 * writer can never run concurrently with this reader.  Taking
	 * th_lock here would close a lock-order cycle with
	 * thread_block_release's `external=p_lock` path (which takes
	 * th_lock while holding p_lock) -- WITNESS would (and did)
	 * panic.
	 */
	exc = NULL;
	t = (current_thread != NULL) ? current_thread->th_task : NULL;
	if (current_thread != NULL) {
		unsigned	exi;

		exi = exc_type_from_trapno((uint32_t)tf->tf_trapno);

		exc = current_thread->th_exc_ports[exi];
		if (exc != NULL)
			port_ref(exc, MACH_PORT_RIGHT_SEND);

		if (exc == NULL && t != NULL) {
			spin_lock(&t->t_lock);
			exc = t->t_exc_ports[exi];
			if (exc != NULL)
				port_ref(exc, MACH_PORT_RIGHT_SEND);
			spin_unlock(&t->t_lock);
		}
	}

	task_flags = 0;
	if (t != NULL) {
		spin_lock(&t->t_lock);
		task_flags = t->t_exc_flags;
		spin_unlock(&t->t_lock);
	}

	if (exc != NULL && (task_flags & EXC_FLAG_RESUMABLE) != 0) {
		verdict     = EXC_VERDICT_KILL;
		rip_advance = 0;
		verdict = deliver_exception_and_wait(exc, tf, &rip_advance);
		port_deref(exc, MACH_PORT_RIGHT_SEND);
		kprintf("user fault verdict=%u advance=%u\n",
		    (unsigned)verdict, (unsigned)rip_advance);
		if (verdict == EXC_VERDICT_RESUME) {
			tf->tf_rip += rip_advance;
			kprintf("user fault resumed past faulting insn\n");
			return;
		}
		/* Fall through to retire on KILL / unknown verdict. */
	} else if (exc != NULL) {
		(void)port_exception_post(exc,
		    (uint32_t)tf->tf_trapno, (uint32_t)tf->tf_err,
		    (uint64_t)tf->tf_rip, (uint64_t)tf->tf_rsp,
		    (uint64_t)tf->tf_rflags, cr2,
		    (uint64_t)(t != NULL ? t->t_id : 0),
		    NULL);
		port_deref(exc, MACH_PORT_RIGHT_SEND);
		kprintf("user fault posted to exception port\n");
	}

	kprintf("user thread retired by kernel\n");

	thread_exit();
	/* NOTREACHED */
}

/*
 * Decode an x86_64 #PF error code into a single human-readable line.
 * The bits are:
 *	0  P     1 = protection violation, 0 = page not present
 *	1  W/R   1 = write,        0 = read
 *	2  U/S   1 = user mode,    0 = supervisor
 *	3  RSVD  1 = reserved-bit set in a paging-structure entry
 *	4  I/D   1 = instruction fetch (NX violation)
 */
static void
pf_print_err(uint64_t err)
{

	kprintf("  err: %s | %s | %s%s%s\n",
	    (err & 0x1) ? "protection-violation" : "page-not-present",
	    (err & 0x2) ? "write" : "read",
	    (err & 0x4) ? "user-mode" : "supervisor",
	    (err & 0x8) ? " | reserved-bit" : "",
	    (err & 0x10) ? " | instruction-fetch" : "");
}

/*
 * Dump 16 bytes starting at the faulting RIP.  Reading user-VA RIPs is
 * fine -- the kernel can see them via the shared PML4 -- but if RIP
 * sits in an unmapped page we'd take a second fault.  Bound the read
 * to the same simple ranges the rest of the kernel considers
 * dereferenceable: [0, PHYSMAP) for kernel direct-mapped code, and
 * [USER_VA_LO, USER_VA_HI) for ring-3 code.  Anything else gets a "?
 * unmapped" line so a single bogus RIP doesn't double-fault us.
 */
static void
rip_byte_dump(uint64_t rip)
{
	const uint8_t	*p;
	uint8_t		 scratch[16];
	unsigned int	 i;
	bool		 is_user;

	/*
	 * Restrict to the two ranges we know are mapped:
	 *	[0, 1 GiB)		kernel identity map (boot.S 2 MiB pages)
	 *	[USER_VA_LO, USER_VA_HI)	user code/data slot (pmap_kenter U=1)
	 * Anything else, decline rather than risk a double fault.
	 */
	if (rip >= 0x80000000ULL) {
		kprintf("code @rip: (unmapped range, skip)\n");
		return;
	}

	/*
	 * User-VA reads need SMAP-bracketing once CR4.SMAP is on; the
	 * kernel-VA read range (< 1 GiB identity map) keeps the brackets
	 * for uniformity -- STAC/CLAC on a kernel address are no-ops.
	 * Copy into a kernel scratch first so the printf path stays
	 * outside AC=1 (the tty lock + console output must not run with
	 * the SMAP override held).
	 */
	p = (const uint8_t *)(uintptr_t)rip;
	is_user = (rip >= 0x40000000ULL && rip < 0x80000000ULL);
	if (is_user)
		smap_user_access_begin();
	for (i = 0; i < 16; i++)
		scratch[i] = p[i];
	if (is_user)
		smap_user_access_end();

	kprintf("code @rip:");
	for (i = 0; i < 16; i++)
		kprintf(" %02x", (unsigned int)scratch[i]);
	kprintf("\n");
}
