/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _AMD64_FPU_H_
#define	_AMD64_FPU_H_

#include <stddef.h>

/*
 * x87/SSE bring-up for ring 3.
 *
 * The kernel itself is built -mno-sse and never touches XMM, but ring-3 code
 * does: a real Apple binary is full of SSE2 (the x86_64 baseline), and even
 * clang on our own freestanding sources vectorises freely.  Until SSE is
 * enabled, the first XMM instruction in ring 3 faults #UD; once enabled, two
 * SSE-using tasks would clobber each other's register file without per-thread
 * save/restore.  This subsystem handles both.
 *
 * fpu_init enables the FPU + SSE on this CPU (CR0.MP=1, EM=0, TS=0;
 * CR4.OSFXSR=1, OSXMMEXCPT=1), runs FNINIT, loads the default MXCSR, and
 * captures a clean FXSAVE image as the template for new threads.  It MUST run
 * before any thread is created or any context switch (thread_switch_asm
 * FXSAVE/FXRSTORs through th_fpu on every switch).
 *
 * fpu_clean_state copies that template into a thread's FXSAVE area so its
 * first FXRSTOR loads sane control words rather than zeros (a zero MXCSR would
 * unmask every SIMD exception).  The area is architecturally 512 bytes and
 * must be 16-byte aligned (FXSAVE/FXRSTOR #GP otherwise).
 */
#define	FPU_XSAVE_AREA_SIZE	512

void	fpu_init(void);
void	fpu_clean_state(void *area);

#endif /* !_AMD64_FPU_H_ */
