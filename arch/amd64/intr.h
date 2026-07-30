/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_INTR_H_
#define	_MACHINE_INTR_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Trap frame produced by the asm stubs in isr.S (x86_64).
 *
 * Field order is contractually frozen against isr_common's push order.
 * The 15 GPRs are pushed r15-last so it lands at the lowest address
 * (the trapframe's first field).  In long mode the CPU always pushes
 * SS:RSP on trap entry, regardless of CPL change.
 *
 * Reordering this struct without updating isr.S corrupts the frame in
 * flight.
 */
struct trapframe {
	/* Pushed by isr_common, low addresses first (i.e. pushed last). */
	uint64_t	tf_r15;
	uint64_t	tf_r14;
	uint64_t	tf_r13;
	uint64_t	tf_r12;
	uint64_t	tf_r11;
	uint64_t	tf_r10;
	uint64_t	tf_r9;
	uint64_t	tf_r8;
	uint64_t	tf_rdi;
	uint64_t	tf_rsi;
	uint64_t	tf_rbp;
	uint64_t	tf_rbx;
	uint64_t	tf_rdx;
	uint64_t	tf_rcx;
	uint64_t	tf_rax;

	/* Pushed by the per-vector stub. */
	uint64_t	tf_trapno;
	uint64_t	tf_err;

	/* Pushed by the CPU on trap entry. */
	uint64_t	tf_rip;
	uint64_t	tf_cs;
	uint64_t	tf_rflags;
	uint64_t	tf_rsp;
	uint64_t	tf_ss;
};

typedef void (*irq_handler_t)(struct trapframe *);

void	intr_dispatch(struct trapframe *);
void	irq_install(unsigned int irq, irq_handler_t);

/*
 * The first vector above the 8259's remapped window, and so the first that
 * belongs to the local APIC rather than to the machine's one shared
 * interrupt controller.
 */
#define	INTR_LOCAL_BASE		48

/*
 * Install a handler for a vector the LOCAL APIC delivers -- its timer, an
 * inter-processor interrupt -- as opposed to irq_install's 8259 lines.  The
 * two are different in a way that matters at the end of the handler: an
 * 8259 interrupt is acknowledged to the 8259, an APIC one to the APIC, and
 * the dispatcher writes the right one because it knows which table the
 * handler came out of.
 *
 * A vector left with NO handler is ignored quietly and acknowledged to
 * NOBODY, which is exactly what the spurious vector requires.
 */
void	intr_install_local(unsigned int vec, irq_handler_t);

/*
 * Resume ring 3 through a hand-built trapframe (isr.S).  Restores all 15
 * GPRs and RFLAGS via IRETQ, so unlike a SYSRET it can return a context
 * whose %rcx and %r11 must survive -- the asynchronous sigreturn path.
 * Never returns, and abandons the kernel stack it was called on.
 */
void	trapframe_iretq(struct trapframe *) __attribute__((noreturn));

static inline void
intr_enable(void)
{

	__asm__ __volatile__ ("sti");
}

static inline void
intr_disable(void)
{

	__asm__ __volatile__ ("cli");
}

/*
 * Disable interrupts and report whether they had been enabled, so that a
 * caller can put them back the way it found them rather than the way it
 * wishes they were.  The pair below is what makes a critical section nest:
 * two of them one inside the other leave interrupts off until the OUTER one
 * ends, because the inner one restores "off", which is what it saw.
 *
 * The read and the clear are one asm block on purpose.  Between a pushfq and
 * a cli there is room for an interrupt, and a caller that took one there would
 * be told interrupts were on -- true when it asked, and no longer true by the
 * time it acts on the answer.
 */
static inline bool
intr_save_disable(void)
{
	uint64_t	rf;

	__asm__ __volatile__ ("pushfq; popq %0; cli"
	    : "=r" (rf)
	    :
	    : "memory");
	return ((rf & (1u << 9)) != 0);		/* RFLAGS.IF */
}

static inline void
intr_restore(bool enabled)
{

	if (enabled)
		__asm__ __volatile__ ("sti");
}

#endif /* !_MACHINE_INTR_H_ */
