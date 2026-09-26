/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * selfkill -- SYS_TASK_KILL on oneself: every task holds a send right to
 * its own task port at MACH_PORT_TASK_SELF, so task_kill of that is the
 * simplest use of the syscall.
 *
 * Detection point #5 (the syscall-exit kill check) retires the thread
 * before sysretq, so nothing after task_kill runs and the "BUG" line must
 * never appear in the boot transcript.
 */

#include "style9.h"

int
main(void)
{
	int	rv;

	printf("selfkill: about to call task_kill(MACH_PORT_TASK_SELF=%u)\n",
	    (unsigned)MACH_PORT_TASK_SELF);

	rv = task_kill(MACH_PORT_TASK_SELF);

	/*
	 * Reached only if the syscall-exit check failed.  The entry check
	 * (#1) on this printf's write would then retire us, but only after
	 * user code ran, which is the bug the line reports.
	 */
	printf("selfkill: BUG -- still alive after task_kill (rv=%d)\n", rv);
	return (99);
}
