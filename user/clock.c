/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * clock.elf -- ring-3 wrapper around the kernel's "clock" Mach service.
 *
 * Looks the service up via bootstrap_lookup, RPCs a CLOCK_OP_GET, and
 * prints the uptime in milliseconds + microseconds + raw tick count.
 * No arguments; demonstrates the bootstrap-lookup + service-call
 * idiom in twenty lines.
 */

int
main(void)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header	hdr;
		struct svc_clock_reply	body;
	} reply;
	uint64_t		seconds;
	mach_port_name_t	svc;
	int			rv;

	svc = bootstrap_lookup(SVC_CLOCK_NAME);
	if (svc == MACH_PORT_NULL) {
		printf("clock: bootstrap_lookup('%s') failed\n",
		    SVC_CLOCK_NAME);
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = CLOCK_OP_GET;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("clock: rpc rv=%d\n", rv);
		return (2);
	}

	seconds = reply.body.cr_uptime_ms / 1000ull;
	printf("uptime: %llu.%03llu s (%llu us, %llu ticks)\n",
	    (unsigned long long)seconds,
	    (unsigned long long)(reply.body.cr_uptime_ms % 1000ull),
	    (unsigned long long)reply.body.cr_uptime_us,
	    (unsigned long long)reply.body.cr_ticks);
	return (0);
}
