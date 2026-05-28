/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Tiny child program: verify it was spawned with a parent port at
 * MACH_PORT_PARENT, install that port as the task's exception port,
 * then trigger a deliberate ring-3 fault.  The kernel synthesises a
 * MACH_EXC_FAULT message and posts it to the exception port (== the
 * parent's port) before retiring this thread.  Used by hello.elf's
 * demo_exception via SYS_SPAWN_WITH_PORT.
 */

#include "style9.h"

int
main(void)
{
	volatile int	*np;
	int		 rv;

	rv = task_set_exception_port(MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK) {
		/*
		 * No parent slot or stale name -- this program is only
		 * meaningful when spawned via SYS_SPAWN_WITH_PORT.  Exit
		 * cleanly so the parent's task_alive yield-spin finishes
		 * promptly rather than waiting on a never-arriving fault.
		 */
		return (1);
	}

	/*
	 * Force a deliberate #PF: a NULL-page write.  The kernel's
	 * user-fault path posts the exception message to the parent
	 * BEFORE retiring this thread, so the parent's recv on the
	 * shared port reliably observes the MACH_EXC_FAULT.  The
	 * volatile keeps the compiler from optimising the store away.
	 */
	np = (volatile int *)0;
	*np = 0xDEADu;

	/* Unreachable: the store above #PF's. */
	return (0);
}
