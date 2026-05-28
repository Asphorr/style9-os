/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * loopchild -- pure ring-3 compute-loop daemon for the async-kill v2
 * demo.  Registers its task-self port under bootstrap so the parent can
 * acquire a SEND right + issue SYS_TASK_KILL, then enters a syscall-
 * free spin.  Validates the IRQ-return-to-user detection point: PIT
 * timer hits the kernel, intr_dispatch's tail kill check fires,
 * thread_exit retires the thread before iretq'ing back to ring 3.
 *
 * Without that detection point the child would loop forever -- it
 * never voluntarily enters the kernel, so the syscall-boundary check
 * (which covers echod's mach_msg_recv pattern) cannot catch it.
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
		 * Duplicate-name on a re-run is the common case: a previous
		 * loopchild was killed before getting a chance to deregister,
		 * so the bootstrap entry lingers.  We don't strictly need the
		 * registration when the parent already holds the taskport via
		 * SYS_SPAWN_RETURNS_TASKPORT.  Carry on into the compute loop
		 * so both the bootstrap-mediated and the parent-managed kill
		 * paths can exercise this binary.
		 */
		printf("loopchild: bootstrap_register rv=%d "
		    "(continuing, parent must hold taskport)\n", rv);
	} else {
		printf("loopchild: registered '%s' under bootstrap\n",
		    LOOPCHILD_TPORT_NAME);
	}

	printf("loopchild: entering syscall-free compute loop\n");

	/*
	 * Pure ring-3 work.  No syscalls -- specifically NO printf or
	 * yield inside the loop, otherwise the syscall-boundary kill
	 * check would catch us before the IRQ-return path gets a chance
	 * to.  spin_counter is volatile so the compiler can't strip the
	 * loop as dead code.
	 */
	while (1)
		spin_counter++;

	/* NOTREACHED -- parent kills us via task_kill. */
	return (0);
}
