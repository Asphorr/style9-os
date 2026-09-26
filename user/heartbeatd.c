/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * heartbeatd -- a minimal persistent daemon for the launchd boot catalog.
 * Unlike loopchild (a syscall-free spinner for the IRQ-return kill path)
 * it parks on a Mach receive and uses no CPU when idle.  The catalog
 * declares it runatload + keepalive, so it is up from boot and respawned
 * if it dies.  It registers no bootstrap name, so several instances
 * never collide.
 */

#include "style9.h"

int
main(void)
{
	struct mach_msg_header	msg;
	mach_port_name_t	park;
	int			rv;

	park = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (park == MACH_PORT_NULL) {
		printf("heartbeatd: port_allocate failed\n");
		return (1);
	}
	printf("heartbeatd: up, parking on idle port 0x%x (kill-to-stop)\n",
	    (unsigned)park);

	/*
	 * Block forever.  We hold a send right on `park' ourselves, so
	 * NO_SENDERS never fires; the only way out is
	 * task_request_terminate, whose wake with t_killed set retires the
	 * thread in thread_block_release.
	 */
	for (;;) {
		rv = mach_msg_recv(park, &msg, sizeof(msg));
		if (rv != MACH_MSG_OK)
			break;
	}

	(void)mach_port_deallocate(park);
	return (0);
}
