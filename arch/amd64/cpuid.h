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
 * CPUID, in one place, on the third copy of it -- the same rule msr.h was
 * written under.  Two identical static inlines in two files is a coincidence;
 * three is a header nobody got round to.
 *
 * The subleaf argument is not optional padding.  Leaves 7 and 0xB take one in
 * ECX and answer differently per value, so a wrapper that only passes a leaf
 * cannot ask them anything -- and those are exactly the leaves that describe
 * SMAP and the topology.
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
 * CPUID_1_EBX_APICID: this processor's INITIAL local-APIC id, which is worth
 * naming because of when it can be asked.  The APIC's own ID register needs
 * the APIC found, mapped and enabled; this needs nothing at all, so a
 * processor can learn which one it is before it has touched a page table --
 * which is precisely the position an application processor wakes up in.
 */
#define	CPUID_1_EBX_APICID(ebx)		((uint32_t)((ebx) >> 24))

#endif /* !_MACHINE_CPUID_H_ */
