/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "ddb.h"
#include "intr.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "sched.h"
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
		intr_panic(tf);
		/* NOTREACHED */
	}

	if (tf->tf_trapno < 48) {
		irq = (unsigned int)(tf->tf_trapno - 32);
		if (irq_handlers[irq] != NULL)
			irq_handlers[irq](tf);
		pic_eoi(irq);

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

	name = (tf->tf_trapno < 32)
	    ? exception_names[tf->tf_trapno]
	    : "(out-of-range)";

	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	kprintf("\n*** %s (vec %u, err=0x%016lx)\n",
	    name, (unsigned int)tf->tf_trapno, (unsigned long)tf->tf_err);
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	kprintf("rip=0x%016lx  cs=0x%04lx  rflags=0x%016lx\n",
	    (unsigned long)tf->tf_rip, (unsigned long)tf->tf_cs,
	    (unsigned long)tf->tf_rflags);
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

	backtrace_print((uintptr_t)tf->tf_rbp, 16);

	ddb_enter((uint64_t)tf->tf_rbp, tf);
	/* NOTREACHED -- ddb_enter is __dead. */
}
