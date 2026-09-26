/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_SMAP_H_
#define	_MACHINE_SMAP_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Supervisor Mode Access Prevention.
 *
 * With CR4.SMAP set, a ring-0 access to a user (U=1) page faults unless
 * EFLAGS.AC is 1; STAC sets AC, CLAC clears it.  Kernel code that means to
 * touch a user pointer brackets it with smap_user_access_begin/end, so an
 * accidental user dereference outside a bracket faults.
 *
 * The helpers test smap_enabled and emit STAC/CLAC only when it is set,
 * which it never is on a CPU without SMAP (CPUID.7.0:EBX bit 20) -- the
 * Nehalem model QEMU runs has none -- so one image boots on both.
 *
 * Tight brackets only: the copy loop and nothing else, so no asynchronous
 * trap can leak AC=1 into a fault handler.
 */

extern bool	smap_enabled;	/* (a) set once by smap_enable_runtime */
extern bool	smap_supported;	/* (c) const after smap_init           */

void		smap_init(void);
bool		smap_enable_runtime(void);
bool		smep_enable(void);

/*
 * Set CR4.SMAP and CR4.SMEP on the calling CPU, each if the kernel has
 * turned it on.  The flags are kernel-wide; the register bits are per CPU.
 */
void		smap_init_cpu(void);

static inline void
smap_user_access_begin(void)
{

	if (smap_enabled)
		__asm __volatile("stac" ::: "cc", "memory");
}

static inline void
smap_user_access_end(void)
{

	if (smap_enabled)
		__asm __volatile("clac" ::: "cc", "memory");
}

#endif /* !_MACHINE_SMAP_H_ */
