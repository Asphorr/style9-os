/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * tasks.elf -- ring-3 wrapper around the kernel's "tasks" Mach service.
 *
 * Looks up dev "tasks", RPCs a TASKS_OP_LIST, walks the reply array,
 * and prints a small ps-style table.  Demonstrates that a userspace
 * program can read live kernel state through a port boundary without
 * any kernel-specific syscall surface beyond the generic Mach ABI.
 */

int
main(void)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header	hdr;
		struct svc_tasks_reply	body;
	} reply;
	mach_port_name_t	svc;
	uint32_t		i;
	int			rv;

	svc = bootstrap_lookup(SVC_TASKS_NAME);
	if (svc == MACH_PORT_NULL) {
		printf("tasks: bootstrap_lookup('%s') failed\n",
		    SVC_TASKS_NAME);
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = TASKS_OP_LIST;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("tasks: rpc rv=%d\n", rv);
		return (2);
	}

	printf("%s live tasks:\n", "                ");
	printf("  %-8s  %-24s  %s\n", "task_id", "name", "threads");
	for (i = 0; i < reply.body.tr_count && i < SVC_TASKS_MAX; i++) {
		struct svc_tasks_entry *e;

		e = &reply.body.tr_entries[i];
		printf("  %-8llu  %-24s  %u\n",
		    (unsigned long long)e->te_task_id,
		    e->te_name,
		    e->te_nthreads);
	}
	return (0);
}
