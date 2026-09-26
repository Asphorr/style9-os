/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Thread-level vs task-level exception precedence.  This child installs
 * the parent-injected port at both the task-level and the thread-level
 * BAD_INSTRUCTION slot, then executes UD2.  user_fault_die checks the
 * thread level first, so exactly one MACH_EXC_FAULT should arrive; the
 * parent's timed receive of a second one must end in MACH_E_TIMEOUT,
 * proving the task-level slot did not also fire.
 */

#include "style9.h"

int
main(void)
{
	int	rv;

	rv = task_set_exception_ports(EXC_MASK_BAD_INSTRUCTION,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK)
		return (1);

	rv = thread_set_exception_ports(EXC_MASK_BAD_INSTRUCTION,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK)
		return (2);

	/* #UD via UD2.  Thread-level slot wins. */
	__asm __volatile("ud2");

	/* Unreachable. */
	return (0);
}
