/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * top(1) -- periodic ring-3 process monitor.
 *
 * A close cousin of tasks(1): both RPC the kernel "tasks" Mach service
 * for a live snapshot of the task table.  Where tasks(1) prints one
 * shot and exits, top(1) samples TOP_SAMPLES times with a yield gap
 * between samples, and prints the richer per-task resource columns the
 * service grew for this tool -- thread count, port-name count, and live
 * VM-region count.  Darwin's top(1) leans on host_processor_info +
 * task_info; we get the equivalent through a single Mach RPC, no
 * per-resource syscall surface.
 *
 * No argv yet, so the sample count + cadence are compile-time.  The
 * point of multiple samples is to show the table is genuinely live:
 * spawn/exit churn between samples shows up as rows appearing and
 * disappearing and as the global task/thread totals moving.
 */

#include "style9.h"

#define	TOP_SAMPLES	3u
#define	TOP_YIELD_GAP	24	/* yields between consecutive samples */

static int
top_sample(mach_port_name_t svc, uint32_t sample)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header	hdr;
		struct svc_tasks_reply	body;
	} reply;
	uint32_t		i, threads, ports, regions;
	int			rv;

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = TASKS_OP_LIST;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 2000);
	if (rv != MACH_MSG_OK) {
		printf("top: rpc rv=%d\n", rv);
		return (rv);
	}
	if (reply.body.tr_count > SVC_TASKS_MAX)
		reply.body.tr_count = SVC_TASKS_MAX;

	threads = 0;
	ports   = 0;
	regions = 0;
	for (i = 0; i < reply.body.tr_count; i++) {
		threads += reply.body.tr_entries[i].te_nthreads;
		ports   += reply.body.tr_entries[i].te_nports;
		regions += reply.body.tr_entries[i].te_nvm_regions;
	}

	printf("top -- sample %u/%u: %u tasks, %u threads, "
	    "%u ports, %u regions\n",
	    sample, TOP_SAMPLES, reply.body.tr_count, threads, ports,
	    regions);
	printf("  %-8s  %-20s  %-4s  %-5s  %-6s\n",
	    "task_id", "name", "thr", "ports", "vmregs");
	for (i = 0; i < reply.body.tr_count; i++) {
		struct svc_tasks_entry *e = &reply.body.tr_entries[i];

		printf("  %-8llu  %-20s  %-4u  %-5u  %-6u\n",
		    (unsigned long long)e->te_task_id,
		    e->te_name,
		    e->te_nthreads,
		    e->te_nports,
		    e->te_nvm_regions);
	}
	return (MACH_MSG_OK);
}

int
main(void)
{
	mach_port_name_t	svc;
	uint32_t		s;
	int			i;

	svc = bootstrap_lookup(SVC_TASKS_NAME);
	if (svc == MACH_PORT_NULL) {
		printf("top: bootstrap_lookup('%s') failed\n", SVC_TASKS_NAME);
		return (1);
	}

	for (s = 1; s <= TOP_SAMPLES; s++) {
		if (top_sample(svc, s) != MACH_MSG_OK) {
			(void)mach_port_deallocate(svc);
			return (2);
		}
		if (s != TOP_SAMPLES) {
			for (i = 0; i < TOP_YIELD_GAP; i++)
				(void)yield();
		}
	}

	(void)mach_port_deallocate(svc);
	return (0);
}
