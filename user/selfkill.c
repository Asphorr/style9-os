/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * selfkill -- demonstrates SYS_TASK_KILL with the self-targeting
 * capability path: every task has SEND on its own task-self port at
 * the well-known MACH_PORT_TASK_SELF slot, so task_kill(MACH_PORT_
 * TASK_SELF) is the simplest possible exercise of the syscall.
 *
 * The print after the kill is intentionally unreachable -- detection
 * point #5 (syscall-exit kill check) retires this thread before the
 * sysretq, so user code below the task_kill call never runs.  Hello
 * checks the absence of the "BUG" line in the boot transcript to
 * confirm the syscall-exit check held.
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
	 * If we reach here, the syscall-exit kill check failed to
	 * retire us.  The printf is a syscall that would catch us via
	 * detection point #1 (entry check) as a fallback -- but the
	 * BUG message itself proves the entry check happened "too late"
	 * by one user-visible instruction.  Should not appear in the
	 * boot transcript.
	 */
	printf("selfkill: BUG -- still alive after task_kill (rv=%d)\n", rv);
	return (99);
}
