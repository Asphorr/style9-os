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
 * Wire structs below are ABI-stable: existing fields keep their offsets,
 * new fields append, and the size is pinned by _Static_assert.  Reordering
 * an existing field breaks any consumer compiled against an older layout.
 * Each is preceded by a WIRE FORMAT banner for grep-ability.
 */

/* ---- "clock" service ---- */
#define	SVC_CLOCK_NAME		"clock"
#define	CLOCK_OP_GET		1

/* WIRE FORMAT.  ABI-stable. */
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

/* WIRE FORMAT.  ABI-stable. */
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

/* WIRE FORMAT.  ABI-stable. */
struct svc_tasks_entry {
	uint64_t	te_task_id;
	uint32_t	te_nthreads;
	uint32_t	te_nports;	/* names in t_port_space        */
	uint32_t	te_nvm_regions;	/* live entries in t_map        */
	uint32_t	te_pad;
	char		te_name[SVC_TASKS_NAME_MAX];
};

_Static_assert(sizeof(struct svc_tasks_entry) == 48,
    "svc_tasks_entry must be 48 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable. */
struct svc_tasks_reply {
	uint32_t		tr_count;
	uint32_t		tr_pad;
	struct svc_tasks_entry	tr_entries[SVC_TASKS_MAX];
};

_Static_assert(sizeof(struct svc_tasks_reply) ==
    8 + SVC_TASKS_MAX * sizeof(struct svc_tasks_entry),
    "svc_tasks_reply layout pinned");

/* ---- "man" service ---- */
/*
 * Manual-page registry.  Holds a small static table of embedded
 * mdoc-rendered text blobs (built from the docs/man .9 pages via the Makefile
 * mandoc pipeline).  Op MAN_OP_GET takes the page name in the inline
 * body and ships the rendered text back as a single OOL descriptor.
 *
 * Wire format (request):
 *	mach_msg_header	(msgh_id = MAN_OP_GET, msgh_size = header + body)
 *	body bytes	NUL-terminated short name (e.g. "port"), <= 32 chars
 *
 * Wire format (reply, success):
 *	mach_msg_header	(MACH_MSGH_BITS_COMPLEX)
 *	mach_msg_body	(descriptor_count = 1)
 *	mach_msg_ool_descriptor	(PHYSICAL_COPY, points at receiver VA)
 *
 * Wire format (reply, not found):
 *	mach_msg_header	(no COMPLEX bit, msgh_id = MAN_NOT_FOUND)
 *
 * Pages are auto-rendered at build time via `mandoc -Tutf8 | col -b`
 * (no backspace overstrike) and embedded as ld -b binary blobs.
 * Adding a new page = drop docs/man/<name>.9 on disk and register it
 * by name in mach/services.c.
 */
#define	SVC_MAN_NAME		"man"
#define	MAN_OP_GET		1
#define	MAN_NOT_FOUND		0xFFFFFFFFu
#define	MAN_NAME_MAX		32

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

/* ---- "launchd" service ---- */
/*
 * Minimal launchd analog: a registry of managed services.  Each entry
 * names a registered string (the "label" -- Apple calls it Label),
 * the program to spawn (a name registered with progreg, e.g. "echod"),
 * and tracks lifecycle state + task id.
 *
 * v1 ops:
 *	LAUNCHCTL_OP_LIST	enumerate every loaded entry.  Re-validates
 *				task liveness via task_is_alive at snapshot
 *				time -- if a RUNNING entry's task is gone,
 *				its row is updated to EXITED before being
 *				written to the reply.
 *	LAUNCHCTL_OP_LOAD	register a label+program pair, spawn the
 *				program immediately, and record the task id.
 *				State transitions: RUNNING on success, FAILED
 *				if the spawn returned an error.  Duplicate
 *				labels are rejected with MACH_E_INVAL.
 *	LAUNCHCTL_OP_UNLOAD	drop the entry from the registry.  Real
 *				kill not implemented yet (no SYS_TASK_KILL);
 *				a still-running task continues until it
 *				exits on its own.  Operation succeeds either
 *				way; the entry is gone from launchd's table.
 *
 * Wire shapes are inline-only -- no OOL.  Total reply for LIST is
 * 8 + LAUNCHD_MAX_SERVICES * sizeof(entry).
 *
 * Deferred to v2: STOP / START (need a kill primitive), restart-on-
 * exit policies, .plist-equivalent manifest loading from disk, cross-
 * task auth, persistent service catalog.
 */
#define	SVC_LAUNCHD_NAME	"launchd"

#define	LAUNCHCTL_OP_LIST	1
#define	LAUNCHCTL_OP_LOAD	2
#define	LAUNCHCTL_OP_UNLOAD	3
#define	LAUNCHCTL_OP_STOP	4	/* kill task, keep entry (v2)    */
#define	LAUNCHCTL_OP_START	5	/* respawn a stopped entry (v2)  */

#define	LAUNCHD_MAX_SERVICES	8
#define	LAUNCHD_NAME_MAX	24
#define	LAUNCHD_PROGRAM_MAX	24

/* LOAD-request flag bits (lr_flags). */
#define	LAUNCHD_LOAD_FLAG_KEEPALIVE	0x1u	/* respawn on unexpected exit */

/*
 * State machine.  LOAD always tries to spawn, so an entry never
 * lingers in a fresh "loaded but never started" state -- that maps
 * directly to RUNNING-or-FAILED.  EXITED is the catch-all "task is
 * gone now" reached either when LIST observes task_is_alive == false
 * or (future) when a death notification fires.
 */
#define	LAUNCHD_STATE_RUNNING	0
#define	LAUNCHD_STATE_EXITED	1
#define	LAUNCHD_STATE_FAILED	2
#define	LAUNCHD_STATE_STOPPED	3	/* explicit STOP; no auto-restart */
#define	LAUNCHD_STATE_THROTTLED	4	/* keep_alive gave up: respawned   */
					/* too fast too many times.  A     */
					/* launchctl START clears it.      */

/* WIRE FORMAT.  ABI-stable.  LOAD request body. */
struct svc_launchctl_load_req {
	char		lr_name[LAUNCHD_NAME_MAX];
	char		lr_program[LAUNCHD_PROGRAM_MAX];
	uint32_t	lr_flags;	/* LAUNCHD_LOAD_FLAG_*           */
	uint32_t	lr_pad;
};

_Static_assert(sizeof(struct svc_launchctl_load_req) == 56,
    "svc_launchctl_load_req must be 56 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  UNLOAD request body (label only). */
struct svc_launchctl_byname_req {
	char		lr_name[LAUNCHD_NAME_MAX];
};

_Static_assert(sizeof(struct svc_launchctl_byname_req) == 24,
    "svc_launchctl_byname_req must be 24 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  LOAD / UNLOAD reply body. */
struct svc_launchctl_status_reply {
	int32_t		ls_status;	/* MACH_MSG_OK or MACH_E_*       */
	uint32_t	ls_state;	/* LAUNCHD_STATE_* after the op  */
	uint64_t	ls_task_id;	/* 0 if not running              */
	uint32_t	ls_taskport;	/* mach_port_name_t: SEND on the */
					/* child task-self port installed*/
					/* in the caller's space on LOAD */
					/* success; 0 otherwise          */
	uint32_t	ls_pad;
};

_Static_assert(sizeof(struct svc_launchctl_status_reply) == 24,
    "svc_launchctl_status_reply must be 24 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  One row in a LIST reply. */
struct svc_launchctl_entry {
	char		le_name[LAUNCHD_NAME_MAX];
	char		le_program[LAUNCHD_PROGRAM_MAX];
	uint32_t	le_state;
	uint32_t	le_pad;
	uint64_t	le_task_id;
};

_Static_assert(sizeof(struct svc_launchctl_entry) == 64,
    "svc_launchctl_entry must be 64 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  LIST reply body. */
struct svc_launchctl_list_reply {
	uint32_t			ll_count;
	uint32_t			ll_pad;
	struct svc_launchctl_entry	ll_entries[LAUNCHD_MAX_SERVICES];
};

_Static_assert(sizeof(struct svc_launchctl_list_reply) ==
    8 + LAUNCHD_MAX_SERVICES * sizeof(struct svc_launchctl_entry),
    "svc_launchctl_list_reply layout pinned");

/* Bring up + register all four services.  Call after bootstrap_init
 * and task_subsystem_init (so kernel_task exists for thread/task ID
 * accounting). */
void	services_init(void);

#endif /* !_SYS_SERVICES_H_ */
