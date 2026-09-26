/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Child for hello.elf's demo_exception (spawned via SYS_SPAWN_WITH_PORT):
 * install the parent port at MACH_PORT_PARENT as the task's exception
 * port, then fault in ring 3.  The kernel posts a MACH_EXC_FAULT to that
 * port before retiring this thread.
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
		 * No parent port: not spawned via SYS_SPAWN_WITH_PORT.
		 * Exit without faulting; the parent's timed receive fails.
		 */
		return (1);
	}

	/*
	 * #PF by a NULL-page write.  The message is posted before the
	 * thread retires, so the parent's receive always sees it.
	 * volatile keeps the store from being optimised away.
	 */
	np = (volatile int *)0;
	*np = 0xDEADu;

	/* Unreachable: the store above #PF's. */
	return (0);
}
