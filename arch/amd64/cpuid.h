/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_CPUID_H_
#define	_MACHINE_CPUID_H_

#include <stdint.h>

/*
 * CPUID with an explicit subleaf in ECX: leaves 7 (SMAP among the feature
 * flags) and 0xB (topology) answer differently per subleaf.
 */
static inline void
cpuid_count(uint32_t leaf, uint32_t subleaf,
    uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{

	__asm__ __volatile__ ("cpuid"
	    : "=a" (*eax), "=b" (*ebx), "=c" (*ecx), "=d" (*edx)
	    : "0" (leaf), "2" (subleaf));
}

/*
 * This processor's initial local-APIC id, available before the APIC is
 * found, mapped or enabled.
 */
#define	CPUID_1_EBX_APICID(ebx)		((uint32_t)((ebx) >> 24))

#endif /* !_MACHINE_CPUID_H_ */
