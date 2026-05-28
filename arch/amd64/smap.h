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
 * When CR4.SMAP is set, a ring-0 access to a user-VA (U=1) page faults
 * unless EFLAGS.AC is 1.  STAC sets AC; CLAC clears it.  Any kernel
 * code that intentionally dereferences a user pointer must bracket the
 * touch with smap_user_access_begin / smap_user_access_end so an
 * accidental user-VA dereference outside the bracket fails closed.
 *
 * Both calls compile to "test then jump-and-emit-instr" and are cheap;
 * STAC/CLAC themselves are 3-byte single-cycle instructions on every
 * SMAP-capable microarch.
 *
 * If the boot CPU does not advertise SMAP (CPUID.7.0:EBX bit 20),
 * smap_init leaves smap_enabled false and the helpers compile down to
 * nothing.  This keeps the same kernel image bootable on pre-SMAP
 * machines (older QEMU CPU models, ancient hardware).
 *
 * Tight brackets only.  The bracket window must be short and known
 * to be free of asynchronous traps that could leak AC=1 to fault
 * handlers; in practice that means "the copy loop and nothing else".
 */

extern bool	smap_enabled;	/* (a) set once by smap_enable_runtime */
extern bool	smap_supported;	/* (c) const after smap_init           */

void		smap_init(void);
bool		smap_enable_runtime(void);

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
