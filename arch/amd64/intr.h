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
 * Trap frame built by the stubs in isr.S.  Field order is frozen against
 * isr_common's push order: the 15 GPRs are pushed r15 last, so it lands
 * at the lowest address, the first field.  In long mode the CPU pushes
 * SS:RSP on every trap, CPL change or not.
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

/* First vector above the 8259's window: the local APIC's from here up. */
#define	INTR_LOCAL_BASE		48

/*
 * Inter-processor interrupt vectors, below the timer's 0xF0: the APIC
 * serves the highest pending vector first, but all these handlers are a
 * few microseconds long.  What bounds how long a shootdown can go
 * unanswered is pmap_tlb_poll, called from the loops that spin with
 * interrupts off, not the vector number.
 */
#define	INTR_VEC_TLB		0xE0	/* forget these translations       */
#define	INTR_VEC_RESCHED	0xE1	/* look at the runqueue            */
#define	INTR_VEC_WHERE		0xE2	/* say where you are               */

/*
 * Install a handler for a local APIC vector (its timer, an IPI), as
 * opposed to irq_install's 8259 lines; the dispatcher acknowledges each
 * to its own chip.  A vector with no handler is ignored and acknowledged
 * to nobody, as the spurious vector requires.
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
 * Disable interrupts and report whether they had been enabled, so the
 * caller can restore them as it found them.  This is what lets critical
 * sections nest: the inner one restores "off", and interrupts stay off
 * until the outer one ends.
 *
 * The read and the clear are one asm block so that no interrupt can land
 * between the pushfq and the cli.
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
