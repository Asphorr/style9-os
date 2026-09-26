/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "darwin.h"
#include "ddb.h"
#include "intr.h"
#include "idt.h"
#include "kprintf.h"
#include "ksym.h"
#include "lapic.h"
#include "panic.h"
#include "pic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "port_internal.h"
#include "sched.h"
#include "smap.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"
#include "tty.h"
#include "vm.h"

static irq_handler_t	irq_handlers[16];

/*
 * Handlers for local APIC vectors, indexed by vector - INTR_LOCAL_BASE.
 * Kept apart from irq_handlers because the two are acknowledged to
 * different chips; the table tells the dispatcher which.
 */
static irq_handler_t	local_handlers[IDT_NENTRIES - INTR_LOCAL_BASE];

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
static bool	fault_fill(const struct trapframe *tf);
static uint32_t	deliver_exception_and_wait(struct port *exc,
		    const struct trapframe *tf,
		    uint32_t *rip_advance_out);
static void	pf_print_err(uint64_t err);
static unsigned int rip_bytes(uint64_t rip, bool from_user, uint8_t *buf);
static void	rip_bytes_print(const uint8_t *buf, unsigned int n);

/* Code bytes an autopsy shows at the faulting RIP. */
#define	RIP_DUMP_BYTES	16

/*
 * Map an x86 trap vector to an EXC_TYPE_* index into t_exc_ports[].
 * Anything unlisted is EXC_TYPE_BAD_ACCESS, Mach's catch-all.
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

void
intr_install_local(unsigned int vec, irq_handler_t handler)
{

	if (vec < INTR_LOCAL_BASE || vec >= IDT_NENTRIES)
		return;
	local_handlers[vec - INTR_LOCAL_BASE] = handler;
}

/*
 * Async-kill detection point #4: return from an interrupt to ring 3.
 * Catches a kill against a thread in a pure ring-3 compute loop that
 * never enters the kernel on its own; any interrupt brings it in, and
 * this retires it instead of resuming user code.
 *
 * Only for frames from ring 3, so an interrupt taken in the kernel (a
 * timer tick while sched_lock is held) never calls thread_exit.
 * kernel_task cannot be killed; testing it first is cheaper than the
 * atomic load.
 */
static inline void
intr_check_async_kill_on_user_return(struct trapframe *tf)
{

	if ((tf->tf_cs & 3) != 3)
		return;
	if (current_thread == NULL)
		return;
	if (current_thread->th_task == kernel_task)
		return;
	if (task_kill_pending(current_thread->th_task))
		thread_exit();
	/*
	 * Signal delivery for a compute loop that never syscalls.  A
	 * default-terminate signal retires the thread here; a caught one is
	 * delivered to its ring-3 handler, with tf carrying the interrupted
	 * state into the signal frame and back -- hence tf is mutable and,
	 * unlike the syscall-exit path, the whole register file is saved.
	 * Darwin-personality tasks only; native tasks have no signal state.
	 */
	if (current_thread->th_task->t_personality == TASK_PERSONALITY_DARWIN)
		darwin_signal_deliver_trap(tf);
	/* NOTREACHED if terminated */
}

/*
 * C-side trap dispatcher, called from isr_common with the trapframe
 * built.  Vectors below 32 are CPU exceptions: demand-filled, fatal to
 * the user thread, or a kernel panic.  32-47 are 8259 IRQs, EOI'd to the
 * 8259 whether or not a handler is installed.  48 and up are local APIC
 * vectors, EOI'd to the APIC only if a handler is installed.
 */
void
intr_dispatch(struct trapframe *tf)
{
	unsigned int	irq;

	if (tf->tf_trapno < 32) {
		/* A lazily mapped page asking to be filled is not an error. */
		if (fault_fill(tf)) {
			intr_check_async_kill_on_user_return(tf);
			return;
		}
		/*
		 * An exception from ring 3 retires the user thread, like
		 * FreeBSD's SIGSEGV-then-die; the kernel runs on.
		 */
		if ((tf->tf_cs & 3) == 3) {
			user_fault_die(tf);
			/*
			 * Only the RESUME verdict returns here, with tf_rip
			 * advanced; the iretq goes back to user mode with it.
			 * KILL ends in thread_exit.
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
		 * Wake threads whose deadline has passed.  Here, after
		 * pic_eoi, not in pit_isr: the spin_unlock inside can drop
		 * the preempt count to zero and yield, and a yield before
		 * the EOI leaves the 8259 holding the IRQ and stops the
		 * ticks.
		 */
		sched_check_timeouts();

		/*
		 * Preempt point, for work the handler left behind: wakes
		 * posted via sched_post_irq_wake, and a need_resched owed
		 * by a timer tick (the PIT's, or one the APIC timer left
		 * pending across a critical section).  Only with preemption
		 * enabled; otherwise the spin_unlock that drops the count
		 * to zero does it.
		 */
		if (preempt_is_enabled()) {
			sched_drain_irq_wakes();
			if (preempt_resched_wanted()) {
				preempt_resched_clear();
				sched_count_preempt();
				thread_yield();
			}
		}
		intr_check_async_kill_on_user_return(tf);
		return;
	}

	/*
	 * Vector >= 48: delivered by this CPU's local APIC.  EOI to the APIC,
	 * and only when a handler is installed: an unclaimed vector is
	 * acknowledged to nothing, as the spurious vector requires -- it has
	 * no in-service bit, so an EOI would retire some other interrupt.
	 */
	if (tf->tf_trapno < IDT_NENTRIES) {
		unsigned int	vec;

		vec = (unsigned int)(tf->tf_trapno - INTR_LOCAL_BASE);
		if (local_handlers[vec] != NULL) {
			local_handlers[vec](tf);
			lapic_eoi();

			/*
			 * Preempt point, after the EOI.  An in-service
			 * interrupt blocks everything of its priority and
			 * below until acknowledged, and the timer is near the
			 * top: a yield before the EOI would carry it off with
			 * the outgoing thread and stop this CPU's ticks.
			 *
			 * No sched_check_timeouts here: the PIT still ticks
			 * and the 8259 tail above runs it.
			 *
			 * Only on a CPU that has a thread.  A parked AP
			 * answers IPIs with no current thread, idle thread or
			 * runqueue, and thread_yield would assert.
			 */
			if (current_thread != NULL && preempt_is_enabled()) {
				sched_drain_irq_wakes();
				if (preempt_resched_wanted()) {
					preempt_resched_clear();
					sched_count_preempt();
					thread_yield();
				}
			}
		}
	}
	intr_check_async_kill_on_user_return(tf);
}

static void
intr_panic(const struct trapframe *tf)
{
	const char	*name;
	const char	*ring;
	uint64_t	 cr2;
	uint8_t		 code[RIP_DUMP_BYTES];
	unsigned int	 ncode;
	bool		 from_user;

	name = (tf->tf_trapno < 32)
	    ? exception_names[tf->tf_trapno]
	    : "(out-of-range)";

	from_user = (tf->tf_cs & 3) == 3;
	ring      = from_user ? "ring 3 (user)" : "ring 0 (kernel)";
	ncode     = rip_bytes((uint64_t)tf->tf_rip, from_user, code);

	/* One console write, so other CPUs' lines land around it. */
	tty_batch_begin();
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
	rip_bytes_print(code, ncode);
	tty_batch_end();

	if (!from_user)
		backtrace_print((uintptr_t)tf->tf_rbp, 16);

	ddb_enter((uint64_t)tf->tf_rbp, tf);
	/* NOTREACHED -- ddb_enter is __dead. */
}

/*
 * Reply protocol.  Create a kernel-owned reply port, send it with the
 * exception message as its msgh_local SEND right, and park for up to
 * EXC_REPLY_TIMEOUT_MS waiting for the verdict.
 *
 * Returns EXC_VERDICT_*.  Any failure -- allocation, enqueue, timeout,
 * malformed reply -- is EXC_VERDICT_KILL, so the caller retires the
 * thread as on the post-and-retire path.  On RESUME, *rip_advance_out is
 * the byte count to skip past the faulting instruction (0 by default).
 *
 * The reply port (RECV+SEND in kernel_space) lives for this call only;
 * the watcher's SEND may keep the object alive a little longer, but the
 * kernel never touches it again.
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
 * The demand-paging half of the #PF handler: if this fault is a mapping
 * asking to be populated, populate it.  Returns true when the faulting
 * instruction should simply be re-run.
 *
 * Kernel-mode faults come here too: a syscall copying into a user buffer
 * not yet touched (read(2) into fresh malloc'd memory) faults from ring 0
 * on a user address.  So the rule is about the address, not the ring: a
 * user VA in the current task's map is serviceable, a kernel VA is a bug
 * and panics.
 *
 * Filling a page can sleep, so the interrupted code must have been able
 * to: no spinlock held.  Every spinlock raises this CPU's preempt count,
 * so that is the whole test -- a thread that blocks holding a lock is
 * never woken again (see fs/bio.c).  The user-copy paths bounce through a
 * stack buffer and drop their locks before copying; a fault that fails
 * the test is a caller touching user memory where it must not, and panics.
 *
 * Interrupts are not re-enabled.  Every IDT gate is an interrupt gate and
 * SYSCALL's FMASK clears IF, so the kernel runs with interrupts off and
 * gets its wakeups by switching away: the thread blocked on the disk hands
 * the CPU to one whose restored RFLAGS turn interrupts on, and the drive's
 * interrupt lands there (ata_wait_intr works this way).
 *
 * CR2 is read once, before anything that can fault or sleep: a nested
 * fault, or another thread running while this one waits, overwrites it.
 */
static bool
fault_fill(const struct trapframe *tf)
{
	struct task	*t;
	uint64_t	 cr2;
	bool		 from_user;
	bool		 write;
	int		 rv;

	if (tf->tf_trapno != 14)
		return (false);
	/*
	 * A protection violation means the page is there and the access was
	 * refused.  Fatal, unless it was a write: that may be copy-on-write,
	 * which vm_fault decides.
	 */
	if ((tf->tf_err & 0x1) != 0 && (tf->tf_err & 0x2) == 0)
		return (false);
	if (current_thread == NULL)
		return (false);
	t = current_thread->th_task;
	if (t == NULL || t == kernel_task || t->t_map == NULL ||
	    t->t_pmap == NULL)
		return (false);

	cr2       = read_cr2();
	write     = (tf->tf_err & 0x2) != 0;
	from_user = (tf->tf_cs & 3) == 3;

	if (!preempt_is_enabled())		/* a lock held: cannot sleep */
		return (false);
	if (!from_user && (cr2 < VM_USER_VA_LO || cr2 >= VM_USER_VA_HI))
		return (false);			/* a kernel VA: a real bug */

	rv = vm_fault(t->t_map, t->t_pmap, cr2, write);
	return (rv == VM_FAULT_OK);
}

/*
 * A ring-3 exception prints an autopsy and kills only the offending user
 * thread; everything else (the shell, kbd-drv, uart-drv) keeps running.
 *
 * If an exception port is set (task_set_exception_port(9)), a
 * MACH_EXC_FAULT message goes to it with enough state for a debugger or
 * crash reporter to do its own autopsy.  Best-effort: a full queue or
 * dead port drops it and never delays the retirement.
 *
 * If the task opted into the reply protocol (EXC_FLAG_RESUMABLE), the
 * thread instead waits for the watcher's verdict: EXC_VERDICT_RESUME
 * advances tf_rip and returns to user mode, EXC_VERDICT_KILL retires it.
 */
static void
user_fault_die(struct trapframe *tf)
{
	struct task	*t;
	struct port	*exc;
	const char	*name;
	const char	*who;
	uint64_t	 cr2;
	uint64_t	 who_id;
	uint32_t	 task_flags;
	uint32_t	 verdict;
	uint32_t	 rip_advance;
	uint8_t		 code[RIP_DUMP_BYTES];
	unsigned int	 ncode;

	name = (tf->tf_trapno < 32)
	    ? exception_names[tf->tf_trapno]
	    : "(out-of-range)";
	cr2 = (tf->tf_trapno == 14) ? read_cr2() : 0;
	who    = "?";
	who_id = 0;
	if (current_thread != NULL && current_thread->th_task != NULL) {
		if (current_thread->th_task->t_name != NULL)
			who = current_thread->th_task->t_name;
		who_id = current_thread->th_task->t_id;
	}
	ncode = rip_bytes((uint64_t)tf->tf_rip, true, code);

	/* One console write, so other CPUs' lines land around it. */
	tty_batch_begin();
	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	kprintf("\n*** user fault in '%s' (task %llu): %s (vec %u, "
	    "err=0x%lx)\n", who, (unsigned long long)who_id,
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
	rip_bytes_print(code, ncode);
	tty_batch_end();

	/*
	 * Resolve the port and take a local SEND ref, so it survives a
	 * concurrent SYS_*_SET_EXC_PORTS or teardown until the post; the
	 * slot keeps its own ref.
	 *
	 * The thread-level slot wins over the task-level one, as in Mach.
	 *
	 * The thread-level read takes no lock.  th_exc_ports[] is written
	 * only by SYS_THREAD_SET_EXC_PORTS on the calling thread itself, and
	 * this runs from a trap on that same thread, so the two cannot
	 * overlap.  Taking th_lock here would close a lock-order cycle with
	 * thread_block_release(external = p_lock), which takes th_lock under
	 * p_lock; WITNESS panics on it.
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
 * Decode a #PF error code into one line.  The bits:
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
 * Copy up to RIP_DUMP_BYTES at the faulting RIP into `buf' without
 * faulting again; returns the count, 0 when nothing there may be read.  A
 * kernel RIP is read only inside the identity map, [0, VM_USER_VA_LO).  A
 * user RIP, and only one that faulted in ring 3, is read only from pages
 * present in the task's pmap: an instruction-fetch fault, or a RIP in the
 * last bytes of a mapping, would otherwise fault in here and turn a user
 * fault into a panic.  The copy stops at the first page not present.
 *
 * Called before the autopsy takes the console: pmap_extract takes
 * pm_lock, and the shootdown path prints under pm_lock.
 */
static unsigned int
rip_bytes(uint64_t rip, bool from_user, uint8_t *buf)
{
	const uint8_t	*p;
	struct pmap	*pm;
	unsigned int	 i;
	unsigned int	 n;
	bool		 is_user;

	n = RIP_DUMP_BYTES;
	is_user = rip >= VM_USER_VA_LO && rip < VM_USER_VA_HI;
	if (is_user) {
		if (!from_user || current_thread == NULL ||
		    current_thread->th_task == NULL)
			return (0);
		pm = current_thread->th_task->t_pmap;
		if (pm == NULL || pmap_extract(pm, rip) == PA_INVALID)
			return (0);
		if (pmap_extract(pm, rip + n - 1) == PA_INVALID)
			n = (unsigned int)(PAGE_SIZE - (rip & (PAGE_SIZE - 1)));
	} else if (rip > VM_USER_VA_LO - n) {
		return (0);
	}

	/* A user-VA read needs the SMAP bracket, kept off the console. */
	p = (const uint8_t *)(uintptr_t)rip;
	if (is_user)
		smap_user_access_begin();
	for (i = 0; i < n; i++)
		buf[i] = p[i];
	if (is_user)
		smap_user_access_end();
	return (n);
}

static void
rip_bytes_print(const uint8_t *buf, unsigned int n)
{
	unsigned int	i;

	if (n == 0) {
		kprintf("code @rip: (not readable, skipped)\n");
		return;
	}
	kprintf("code @rip:");
	for (i = 0; i < n; i++)
		kprintf(" %02x", (unsigned int)buf[i]);
	kprintf("\n");
}
