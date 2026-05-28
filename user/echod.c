/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * echod -- demo persistent daemon that the launchd/launchctl arc
 * brings up and tears down.
 *
 * Lifecycle:
 *	1. Register an "echo" service under the bootstrap port so
 *	   clients can find us by name.
 *	2. Loop: mach_msg_recv_block() on the service port; for each
 *	   incoming message, echo back a reply on msgh_local carrying
 *	   the same msgh_id (mirrors Mach RPC semantics).
 *	3. After a bounded number of round-trips, deregister and exit
 *	   so the demo terminates cleanly without needing a kill
 *	   primitive (deferred for launchd v2).
 *
 * This is the smallest "real" managed service we can demo: long-
 * lived enough that LIST sees it RUNNING, finite enough that the
 * EXITED transition happens naturally during the same boot.
 */

#include "style9.h"

#define	ECHOD_SVC_NAME		"echo"
#define	ECHOD_MAX_ROUNDS	8u

int
main(void)
{
	struct mach_msg_header	req;
	struct mach_msg_header	reply;
	mach_port_name_t	svc;
	uint32_t		i;
	int			rv;

	svc = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (svc == MACH_PORT_NULL) {
		printf("echod: port_allocate failed\n");
		return (1);
	}

	rv = bootstrap_register_service(ECHOD_SVC_NAME, svc);
	if (rv != MACH_MSG_OK) {
		printf("echod: bootstrap_register_service rv=%d\n", rv);
		(void)mach_port_deallocate(svc);
		return (2);
	}
	printf("echod: serving '%s' (svc=0x%x, %u-round limit)\n",
	    ECHOD_SVC_NAME, (unsigned)svc, ECHOD_MAX_ROUNDS);

	for (i = 0; i < ECHOD_MAX_ROUNDS; i++) {
		rv = mach_msg_recv(svc, &req, sizeof(req));
		if (rv != MACH_MSG_OK) {
			printf("echod: recv rv=%d, exiting\n", rv);
			break;
		}
		if (req.msgh_local == MACH_PORT_NULL) {
			printf("echod: req has no reply port (id=0x%x)\n",
			    (unsigned)req.msgh_id);
			continue;
		}

		reply.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
		    0);
		reply.msgh_size    = sizeof(reply);
		reply.msgh_remote  = req.msgh_local;
		reply.msgh_local   = MACH_PORT_NULL;
		reply.msgh_voucher = 0;
		reply.msgh_id      = req.msgh_id;
		(void)mach_msg_send(&reply);
	}

	(void)bootstrap_deregister_service(ECHOD_SVC_NAME);
	(void)mach_port_deallocate(svc);
	printf("echod: served %u rounds, exiting\n", i);
	return (0);
}
