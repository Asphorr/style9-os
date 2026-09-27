/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * svcecho -- a launchd Mach service.  launchd keeps its port under the
 * label; svcecho checks the receive right in and answers each request
 * with its own msgh_id, until one asks it to crash.  Its port outlives
 * it: PORT_DESTROYED takes the right back to launchd with what is still
 * queued, for the instance keep_alive starts next.  launchctl's Mach
 * service scene crashes it with a request queued behind.
 */

#include "style9.h"

#define	SVCECHO_LABEL	"com.style9.svcecho"
#define	SVCECHO_CRASH	0x5EC0DEADu	/* exit, leaving the queue */
#define	CHECK_IN_TURNS	200		/* launchd notes the task late */

int
main(void)
{
	struct {
		struct mach_msg_header	hdr;
		uint8_t			body[64];
	} msg;
	struct mach_msg_header	reply;
	mach_port_name_t	port;
	int			i;

	/*
	 * launchd records this task as the job's once its spawn returns,
	 * which may be after this runs: until then check-in is refused.
	 */
	port = MACH_PORT_NULL;
	for (i = 0; i < CHECK_IN_TURNS && port == MACH_PORT_NULL; i++) {
		port = bootstrap_check_in(SVCECHO_LABEL);
		if (port == MACH_PORT_NULL)
			(void)poll_turn();
	}
	if (port == MACH_PORT_NULL) {
		printf("svcecho: check-in for '%s' refused\n", SVCECHO_LABEL);
		return (1);
	}
	printf("svcecho: checked in, serving '%s' on 0x%x\n", SVCECHO_LABEL,
	    (unsigned)port);

	while (mach_msg_recv(port, &msg.hdr, sizeof(msg)) == MACH_MSG_OK) {
		if (msg.hdr.msgh_id == SVCECHO_CRASH) {
			printf("svcecho: told to crash, exiting with the rest "
			    "of the queue unread\n");
			return (1);
		}
		if (msg.hdr.msgh_local == MACH_PORT_NULL)
			continue;
		reply.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		reply.msgh_size    = sizeof(reply);
		reply.msgh_remote  = msg.hdr.msgh_local;
		reply.msgh_local   = MACH_PORT_NULL;
		reply.msgh_voucher = 0;
		reply.msgh_id      = msg.hdr.msgh_id;
		(void)mach_msg_send_timed(&reply, MACH_TIMEOUT_NONE);
		(void)mach_port_deallocate(msg.hdr.msgh_local);
	}
	return (0);
}
