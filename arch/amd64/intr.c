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
#include "sched.h"
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
static void	user_fault_die(const struct trapframe *)
		    __attribute__((noreturn));
static void	pf_print_err(uint64_t err);
static void	rip_byte_dump(uint64_t rip);

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
		if ((tf->tf_cs & 3) == 3)
			user_fault_die(tf);
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
		return;
	}

	/* Vector >= 48: spurious / not installed -- ignore quietly. */
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
 * BSD-style fault retirement: a #PF / #GP / #UD / etc. that originates
 * in ring 3 prints an autopsy and then kills just the offending user
 * thread.  Sibling kernel threads (shell, kbd-drv, uart-drv) keep
 * running, mirroring the SIGSEGV-then-die model.
 */
static void
user_fault_die(const struct trapframe *tf)
{
	const char	*name;
	uint64_t	 cr2;

	name = (tf->tf_trapno < 32)
	    ? exception_names[tf->tf_trapno]
	    : "(out-of-range)";

	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	kprintf("\n*** user fault: %s (vec %u, err=0x%lx)\n",
	    name, (unsigned int)tf->tf_trapno,
	    (unsigned long)tf->tf_err);
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));

	if (tf->tf_trapno == 14) {
		cr2 = read_cr2();
		kprintf("  cr2 = 0x%lx (faulting VA)\n",
		    (unsigned long)cr2);
		pf_print_err((uint64_t)tf->tf_err);
	}

	kprintf("  rip=0x%lx  rsp=0x%lx  rbp=0x%lx\n",
	    (unsigned long)tf->tf_rip,
	    (unsigned long)tf->tf_rsp,
	    (unsigned long)tf->tf_rbp);
	kprintf("  rax=0x%lx  rdi=0x%lx  rsi=0x%lx  rdx=0x%lx\n",
	    (unsigned long)tf->tf_rax,
	    (unsigned long)tf->tf_rdi,
	    (unsigned long)tf->tf_rsi,
	    (unsigned long)tf->tf_rdx);
	rip_byte_dump((uint64_t)tf->tf_rip);

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
	unsigned int	 i;

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

	p = (const uint8_t *)(uintptr_t)rip;
	kprintf("code @rip:");
	for (i = 0; i < 16; i++)
		kprintf(" %02x", (unsigned int)p[i]);
	kprintf("\n");
}
