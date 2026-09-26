/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * echod -- persistent daemon that launchd/launchctl bring up and tear
 * down.
 *
 *	1. Register an "echo" service under the bootstrap port.
 *	2. Loop: mach_msg_recv on the service port and reply on msgh_local
 *	   with the same msgh_id.
 *	3. No self-exit.  launchctl UNLOAD makes launchd request an async
 *	   kill (task_request_terminate); the parked receive wakes with
 *	   t_killed set and the thread retires inside the kernel, so the
 *	   cleanup after the loop does not run.  Task teardown reclaims
 *	   the port space and vm_map.
 */

#include "style9.h"

#define	ECHOD_SVC_NAME	"echo"

int
main(void)
{
	struct mach_msg_header	req;
	struct mach_msg_header	reply;
	mach_port_name_t	svc;
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
	printf("echod: serving '%s' (svc=0x%x, kill-to-stop)\n",
	    ECHOD_SVC_NAME, (unsigned)svc);

	for (;;) {
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
	return (0);
}
