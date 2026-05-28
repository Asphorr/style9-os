/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_SERVICES_H_
#define	_SYS_SERVICES_H_

#include <stdint.h>

#include "port.h"

/*
 * Kernel-side Mach services.
 *
 * Each is a PORT_SPECIAL_SERVICE port with a synchronous dispatcher,
 * registered with the global bootstrap port under a stable string
 * name.  Reachable identically from any kernel thread (via
 * mach_msg_rpc against kernel_space) and from ring-3 (via SYS_MSG_RPC
 * preceded by a bootstrap_lookup).
 *
 * Boot-time wiring in services_init.
 *
 * Wire formats below are pinned by _Static_assert so a future ring-3
 * libc / sysctl shim can build against them without further coupling.
 */

/* ---- "clock" service ---- */
#define	SVC_CLOCK_NAME		"clock"
#define	CLOCK_OP_GET		1

struct svc_clock_reply {
	uint64_t	cr_uptime_ms;
	uint64_t	cr_uptime_us;
	uint64_t	cr_ticks;
};

_Static_assert(sizeof(struct svc_clock_reply) == 24,
    "svc_clock_reply must be 24 bytes (wire format)");

/* ---- "stats" service ---- */
#define	SVC_STATS_NAME		"stats"
#define	STATS_OP_GET		1

struct svc_stats_reply {
	uint64_t	sr_pmm_used_pages;
	uint64_t	sr_kmem_cached_pages;
	uint64_t	sr_kernel_inuse;
	uint64_t	sr_task_count;
	uint64_t	sr_thread_count;
	uint64_t	sr_ctx_switches;
	uint64_t	sr_pmm_total_pages;
};

_Static_assert(sizeof(struct svc_stats_reply) == 56,
    "svc_stats_reply must be 56 bytes (wire format)");

/* ---- "tasks" service ---- */
#define	SVC_TASKS_NAME		"tasks"
#define	TASKS_OP_LIST		1

#define	SVC_TASKS_MAX		16
#define	SVC_TASKS_NAME_MAX	24

struct svc_tasks_entry {
	uint64_t	te_task_id;
	uint32_t	te_nthreads;
	uint32_t	te_pad;
	char		te_name[SVC_TASKS_NAME_MAX];
};

_Static_assert(sizeof(struct svc_tasks_entry) == 40,
    "svc_tasks_entry must be 40 bytes (wire format)");

struct svc_tasks_reply {
	uint32_t		tr_count;
	uint32_t		tr_pad;
	struct svc_tasks_entry	tr_entries[SVC_TASKS_MAX];
};

_Static_assert(sizeof(struct svc_tasks_reply) ==
    8 + SVC_TASKS_MAX * sizeof(struct svc_tasks_entry),
    "svc_tasks_reply layout pinned");

/* ---- "echool" service ---- */
/*
 * Tiny OOL round-trip oracle.  Caller sends a complex message carrying
 * a single OOL descriptor; the dispatcher reads `size` bytes from the
 * sender's address (sender's pmap is current under a special-port
 * intercept), folds them through FNV-1a, and replies with the resulting
 * 32-bit checksum riding in msgh_id.  The reply is an inline bare
 * header on the reply port from msgh_local.
 *
 * Purpose: lets ring-3 prove its OOL wire-format construction byte-for-
 * byte against the kernel's parser without dragging in a worker-thread
 * receiver.  Stress_ool already covers the deliver_msg + recv_install_ool
 * leg via a kernel-space worker task.
 */
#define	SVC_ECHOOL_NAME		"echool"
#define	ECHOOL_OP_CHECKSUM	1

/* Bring up + register all four services.  Call after bootstrap_init
 * and task_subsystem_init (so kernel_task exists for thread/task ID
 * accounting). */
void	services_init(void);

#endif /* !_SYS_SERVICES_H_ */
