/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Per-type exception-port child.  Spawned via SYS_SPAWN_WITH_PORT with
 * a SEND right injected at MACH_PORT_PARENT; installs it as the
 * BAD_INSTRUCTION exception port (and *only* that one slot, leaving
 * the other types unbound) before deliberately executing UD2 to
 * trigger #UD (invalid opcode).  The kernel's per-type dispatch must
 * pick up the BAD_INSTRUCTION slot for trapno 6 and post
 * MACH_EXC_FAULT onto it; if the routing were broken (e.g. always
 * dispatching to BAD_ACCESS), the slot would be NULL and no message
 * would be delivered, surfacing as a timeout on the parent's recv.
 */

#include "style9.h"

int
main(void)
{
	int	rv;

	rv = task_set_exception_ports(EXC_MASK_BAD_INSTRUCTION,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK) {
		/*
		 * Parent slot stale or absent -- only meaningful when
		 * spawned via SYS_SPAWN_WITH_PORT.  Exit cleanly.
		 */
		return (1);
	}

	/*
	 * Deliberate #UD via UD2.  The kernel's exc_type_from_trapno
	 * routes trapno 6 to EXC_TYPE_BAD_INSTRUCTION; our slot is
	 * populated; user_fault_die posts the exception there before
	 * retiring this thread.
	 */
	__asm __volatile("ud2");

	/* Unreachable. */
	return (0);
}
