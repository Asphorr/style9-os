/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * heartbeatd -- a minimal persistent daemon for the s9 launchd boot
 * catalog.  Unlike loopchild (a syscall-free CPU spinner used to stress
 * the IRQ-return kill path), heartbeatd PARKS on a Mach recv: zero CPU
 * when idle, the way a real daemon behaves.  The boot catalog declares
 * it runatload + keep_alive, so it is up from boot and the launchd
 * worker respawns it if it ever dies.  It registers no bootstrap name
 * (it serves no clients), so multiple instances never collide.
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
	 * Block forever.  We hold SEND on `park' ourselves so NO_SENDERS
	 * never fires and the recv never returns on its own; the only way
	 * out is task_request_terminate, which wakes the parked recv with
	 * t_killed set so thread_block_release's post-wake check retires
	 * us.  No CPU is consumed while parked.
	 */
	for (;;) {
		rv = mach_msg_recv(park, &msg, sizeof(msg));
		if (rv != MACH_MSG_OK)
			break;
	}

	(void)mach_port_deallocate(park);
	return (0);
}
