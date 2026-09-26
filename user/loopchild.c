/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * loopchild -- pure ring-3 compute loop for the async-kill demo.
 * Registers its task-self port under bootstrap so the parent can get a
 * send right and issue SYS_TASK_KILL, then spins without syscalls.  It
 * never enters the kernel on its own, so only the IRQ-return check can
 * kill it: the timer interrupt's tail in intr_dispatch sees the kill and
 * thread_exit retires the thread before the iretq to ring 3.
 */

#include "style9.h"

#define	LOOPCHILD_TPORT_NAME	"loopchild.tport"

static volatile uint64_t	spin_counter;

int
main(void)
{
	int	rv;

	printf("loopchild: about to publish task-self port at '%s'\n",
	    LOOPCHILD_TPORT_NAME);

	rv = bootstrap_register_service(LOOPCHILD_TPORT_NAME,
	    MACH_PORT_TASK_SELF);
	if (rv != MACH_MSG_OK) {
		/*
		 * Usually a duplicate name: an earlier loopchild was killed
		 * before it could deregister.  A parent that spawned us with
		 * SYS_SPAWN_RETURNS_TASKPORT already holds the task port, so
		 * carry on; both kill paths use this binary.
		 */
		printf("loopchild: bootstrap_register rv=%d "
		    "(continuing, parent must hold taskport)\n", rv);
	} else {
		printf("loopchild: registered '%s' under bootstrap\n",
		    LOOPCHILD_TPORT_NAME);
	}

	printf("loopchild: entering syscall-free compute loop\n");

	/*
	 * No syscalls (no printf, no yield) in the loop, or the
	 * syscall-boundary check would catch us first.  spin_counter is
	 * volatile so the loop is not stripped as dead code.
	 */
	while (1)
		spin_counter++;

	/* NOTREACHED -- parent kills us via task_kill. */
	return (0);
}
