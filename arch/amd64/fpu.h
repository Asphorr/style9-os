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
 * x87/SSE for ring 3.
 *
 * The kernel is built -mno-sse and never touches XMM, but ring-3 Darwin
 * code does: SSE2 is the x86_64 baseline for Apple binaries, and clang
 * vectorises our own Darwin-target sources.  Without OSFXSR the first XMM
 * instruction #UDs; with it, each thread's XMM state must be saved.
 *
 * fpu_init sets up this CPU (CR0.MP=1, EM=0, TS=0; CR4.OSFXSR=1,
 * OSXMMEXCPT=1), runs FNINIT, loads the default MXCSR and captures a clean
 * FXSAVE image as the template for new threads.  It must run before any
 * thread is created or switched: thread_switch_asm FXSAVEs/FXRSTORs th_fpu
 * on every switch.
 *
 * fpu_clean_state copies the template into a thread's FXSAVE area, so the
 * first FXRSTOR loads sane control words rather than zeros (a zero MXCSR
 * unmasks every SIMD exception).  The area is 512 bytes and must be 16-byte
 * aligned (FXSAVE/FXRSTOR #GP otherwise).
 */
#define	FPU_XSAVE_AREA_SIZE	512

void	fpu_init(void);

/*
 * Just the control-register bits, for a CPU that arrives after the template
 * was captured: CR0 and CR4 are per CPU, and one without OSFXSR #UDs at the
 * first FXRSTOR the scheduler does for it.
 */
void	fpu_init_cpu(void);
void	fpu_clean_state(void *area);

#endif /* !_AMD64_FPU_H_ */
