/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "fpu.h"
#include "kprintf.h"
#include "panic.h"

/*
 * Clean FXSAVE image captured once by fpu_init: x87 in its post-FNINIT state
 * with the default MXCSR (round-nearest, all SIMD exceptions masked).  Copied
 * into every new thread's FXSAVE area so the first FXRSTOR loads a sane state.
 * 16-byte aligned because FXSAVE writes it.
 */
static uint8_t	fpu_template[FPU_XSAVE_AREA_SIZE] __attribute__((aligned(16)));

void
fpu_init(void)
{
	uint64_t	cr0;
	uint64_t	cr4;
	uint32_t	mxcsr;

	__asm __volatile("mov %%cr0, %0" : "=r"(cr0));
	cr0 &= ~(1ULL << 2);	/* EM = 0: execute x87/SSE, do not emulate    */
	cr0 |=  (1ULL << 1);	/* MP = 1: monitor coprocessor                */
	cr0 &= ~(1ULL << 3);	/* TS = 0: eager FPU -- never trap #NM        */
	__asm __volatile("mov %0, %%cr0" : : "r"(cr0));

	__asm __volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ULL << 9);	/* OSFXSR: SSE + FXSAVE/FXRSTOR enabled       */
	cr4 |= (1ULL << 10);	/* OSXMMEXCPT: SIMD FP faults reported as #XM */
	__asm __volatile("mov %0, %%cr4" : : "r"(cr4));

	/*
	 * Bring the x87 + SSE state to a known-good baseline, then snapshot it
	 * as the per-thread template.  FNINIT resets the x87 control/status;
	 * 0x1F80 is the reset MXCSR (round-nearest, every SIMD exception
	 * masked) so ring-3 SSE never trips an unexpected #XM.
	 */
	__asm __volatile("fninit");
	mxcsr = 0x1F80;
	__asm __volatile("ldmxcsr %0" : : "m"(mxcsr));
	__asm __volatile("fxsave (%0)" : : "r"(fpu_template) : "memory");

	kprintf("fpu: SSE enabled for ring 3 (CR4.OSFXSR|OSXMMEXCPT), "
	    "FXSAVE template captured\n");
}

void
fpu_clean_state(void *area)
{
	const uint8_t	*src;
	uint8_t		*dst;
	size_t		 i;

	KASSERT(((uintptr_t)area & 15) == 0,
	    "fpu_clean_state: FXSAVE area not 16-byte aligned");

	src = fpu_template;
	dst = (uint8_t *)area;
	for (i = 0; i < FPU_XSAVE_AREA_SIZE; i++)
		dst[i] = src[i];
}
