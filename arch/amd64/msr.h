/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_MSR_H_
#define	_MACHINE_MSR_H_

#include <stdint.h>

/*
 * Model-specific registers and the two instructions that reach them.  The
 * numbers are kept in one place because a typo in one is a write to
 * unrelated CPU state.
 */

#define	MSR_APIC_BASE		0x0000001Bu	/* IA32_APIC_BASE       */
#define	MSR_EFER		0xC0000080u
#define	MSR_STAR		0xC0000081u
#define	MSR_LSTAR		0xC0000082u
#define	MSR_FMASK		0xC0000084u

/*
 * IA32_GS_BASE, and its shadow IA32_KERNEL_GS_BASE that SWAPGS exchanges
 * with it.  The kernel writes the first and leaves the second alone; see
 * machine/cpu.h for why, and for what changes when ring 3 wants %gs.
 */
#define	MSR_GS_BASE		0xC0000101u
#define	MSR_KERNEL_GS_BASE	0xC0000102u

#define	EFER_SCE		(1u << 0)	/* SYSCALL/SYSRET enable */

/* IA32_APIC_BASE fields. */
#define	APIC_BASE_BSP		(1ull << 8)	/* this is the boot CPU     */
#define	APIC_BASE_EXTD		(1ull << 10)	/* x2APIC mode enabled      */
#define	APIC_BASE_EN		(1ull << 11)	/* APIC globally enabled    */
#define	APIC_BASE_ADDR_MASK	0x000FFFFFFFFFF000ull

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t	lo;
	uint32_t	hi;

	__asm__ __volatile__ ("rdmsr"
	    : "=a" (lo), "=d" (hi)
	    : "c" (msr));
	return (((uint64_t)hi << 32) | lo);
}

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
	uint32_t	lo;
	uint32_t	hi;

	lo = (uint32_t)(val & 0xFFFFFFFFu);
	hi = (uint32_t)(val >> 32);
	__asm__ __volatile__ ("wrmsr"
	    :
	    : "c" (msr), "a" (lo), "d" (hi));
}

#endif /* !_MACHINE_MSR_H_ */
