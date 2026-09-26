/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Per-type exception-port child.  Spawned via SYS_SPAWN_WITH_PORT with a
 * send right at MACH_PORT_PARENT, it installs that right as the
 * BAD_INSTRUCTION port only, then executes UD2.  Per-type dispatch must
 * pick that slot for trapno 6; misrouting (say, to BAD_ACCESS) finds an
 * empty slot and shows up as a timeout on the parent's receive.
 */

#include "style9.h"

int
main(void)
{
	int	rv;

	rv = task_set_exception_ports(EXC_MASK_BAD_INSTRUCTION,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK) {
		/* No parent port: not spawned via SYS_SPAWN_WITH_PORT. */
		return (1);
	}

	/*
	 * #UD: exc_type_from_trapno maps trapno 6 to
	 * EXC_TYPE_BAD_INSTRUCTION, and user_fault_die posts to our slot
	 * before retiring the thread.
	 */
	__asm __volatile("ud2");

	/* Unreachable. */
	return (0);
}
