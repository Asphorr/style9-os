/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cpuid.h"
#include "kprintf.h"
#include "smap.h"

/*
 * Supervisor Mode Access Prevention.  CPUID.(EAX=7,ECX=0):EBX bit 20
 * advertises SMAP (bit 7 is SMEP, not used here).  smap_init only detects;
 * kmain calls smap_enable_runtime right after it to set CR4.SMAP.  The
 * bracket helpers (smap_user_access_begin/end) are no-ops until then, and
 * once it is set an unbracketed user-pointer dereference #PFs the kernel.
 */

bool	smap_enabled;
bool	smap_supported;

void
smap_init(void)
{
	uint32_t	eax;
	uint32_t	ebx;
	uint32_t	ecx;
	uint32_t	edx;
	uint32_t	maxleaf;

	cpuid_count(0, 0, &maxleaf, &ebx, &ecx, &edx);
	if (maxleaf < 7) {
		kprintf("smap: CPUID leaf 7 unavailable (maxleaf=%u)\n",
		    (unsigned)maxleaf);
		return;
	}

	cpuid_count(7, 0, &eax, &ebx, &ecx, &edx);
	if ((ebx & (1u << 20)) == 0) {
		kprintf("smap: not supported by this CPU\n");
		return;
	}

	smap_supported = true;
	kprintf("smap: supported by CPU\n");
}

/*
 * Set CR4.SMAP (bit 21) on this CPU and arm the bracket helpers.  Returns
 * false if the CPU lacks SMAP.  Idempotent.  Called from kmain before the
 * APs are released; they copy the bit in smap_init_cpu.
 */
bool
smap_enable_runtime(void)
{
	uint64_t	cr4;

	if (!smap_supported)
		return (false);
	if (smap_enabled)
		return (true);

	__asm __volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ull << 21);
	__asm __volatile("mov %0, %%cr4" : : "r"(cr4));

	smap_enabled = true;
	kprintf("smap: enabled (CR4.SMAP=1)\n");
	return (true);
}

/*
 * CR4.SMAP is per-CPU; smap_enabled is not.  A CPU brought up after the
 * enable would otherwise let the kernel touch user memory without a fault,
 * on that CPU only.
 */
void
smap_init_cpu(void)
{
	uint64_t	cr4;

	if (!smap_enabled)
		return;

	__asm __volatile("mov %%cr4, %0" : "=r"(cr4));
	cr4 |= (1ull << 21);
	__asm __volatile("mov %0, %%cr4" : : "r"(cr4));
}
