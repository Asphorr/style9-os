/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "kprintf.h"
#include "smap.h"

/*
 * Bring up Supervisor Mode Access Prevention infrastructure.
 *
 * Detection: CPUID leaf 7, sub-leaf 0, returns the structured extended
 * feature flags in EBX.  Bit 20 (1 << 20) advertises SMAP support; bit
 * 7 advertises SMEP (Supervisor Mode Execution Prevention -- not yet
 * wired here).
 *
 * v1 detects-and-records but does NOT yet set CR4.SMAP.  The kernel
 * has many ad-hoc user-pointer dereferences (msg->msgh_size, ...) that
 * still need to be wrapped before flipping the bit; the first
 * unwrapped deref after CR4.SMAP=1 #PFs the kernel.  See
 * smap_enable_runtime below: once the wrap arc completes, calling it
 * sets the bit and arms the discipline.
 *
 * The helpers (smap_user_access_begin / smap_user_access_end) gate on
 * smap_enabled, so brackets added today are no-ops until enable.
 * This lets coverage land incrementally without breaking boot.
 */

bool	smap_enabled;
bool	smap_supported;

static inline void
cpuid_count(uint32_t leaf, uint32_t subleaf,
    uint32_t *eax, uint32_t *ebx, uint32_t *ecx, uint32_t *edx)
{

	__asm __volatile("cpuid"
	    : "=a"(*eax), "=b"(*ebx), "=c"(*ecx), "=d"(*edx)
	    : "0"(leaf), "2"(subleaf));
}

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
	kprintf("smap: supported by CPU (not yet enabled -- "
	    "call smap_enable_runtime after wrapping coverage)\n");
}

/*
 * Opt-in: set CR4.SMAP and arm the bracket helpers.  Returns true if
 * the bit is now set, false if the CPU does not support SMAP at all.
 * Idempotent.  Intended to be called from a future bringup point once
 * every kernel-side user-pointer deref is bracketed.
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
