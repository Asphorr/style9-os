/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_INTR_H_
#define	_MACHINE_INTR_H_

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

#endif /* !_MACHINE_INTR_H_ */
