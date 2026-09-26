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
 * registered with bootstrap under a fixed name by services_init, and
 * reached the same way from a kernel thread (mach_msg_rpc in
 * kernel_space) or ring 3 (bootstrap_lookup, then SYS_MSG_RPC).
 *
 * WIRE FORMAT structs are ABI-stable, as in port.h.
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

/* ---- "progreg" service ---- */
/*
 * What can be spawned: the names in the program registry, for the
 * shell.
 *
 * The names come back packed, NUL-separated in one byte array.  Fixed
 * slots would bring the reply close to svc_reply_inline's 1024-byte
 * ceiling; packed, the current registry takes about 350 bytes.
 *
 * pr_count names fit, pr_total exist.  If the registry outgrows the
 * array the two differ, and the caller can say the list is short
 * rather than print one that looks complete.
 */
#define	SVC_PROGREG_NAME	"progreg"
#define	PROGREG_OP_LIST		1
#define	SVC_PROGREG_BYTES	768

/*
 * pr_macho has one bit per packed name, in packing order: set if the
 * image is Mach-O, a Darwin binary rather than a native ELF, so the
 * shell can show which is which.  64 bits cover PROGREG_MAX.
 */

/* WIRE FORMAT.  ABI-stable. */
struct svc_progreg_reply {
	uint32_t	pr_count;	/* names packed into pr_names   */
	uint32_t	pr_total;	/* names the registry holds     */
	uint64_t	pr_macho;	/* bit per packed name           */
	char		pr_names[SVC_PROGREG_BYTES];
};

_Static_assert(sizeof(struct svc_progreg_reply) == 16 + SVC_PROGREG_BYTES,
    "svc_progreg_reply layout pinned");

/* ---- "man" service ---- */
/*
 * Manual pages: the .9 pages in docs/man, rendered at build time with
 * `mandoc -Tutf8 | col -b` and linked in with objcopy.  MAN_OP_GET takes
 * a page name and returns the text as one OOL descriptor.  A new page is
 * a docs/man/<name>.9 plus an entry in mach/services.c.
 *
 * Request:
 *	mach_msg_header	(msgh_id = MAN_OP_GET, msgh_size = header + body)
 *	body bytes	name (e.g. "port"), NUL included, <= MAN_NAME_MAX
 *
 * Reply, found:
 *	mach_msg_header	(MACH_MSGH_BITS_COMPLEX)
 *	mach_msg_body	(descriptor_count = 1)
 *	mach_msg_ool_descriptor	(PHYSICAL_COPY, points at receiver VA)
 *
 * Reply, not found:
 *	mach_msg_header	(no COMPLEX bit, msgh_id = MAN_NOT_FOUND)
 */
#define	SVC_MAN_NAME		"man"
#define	MAN_OP_GET		1
#define	MAN_NOT_FOUND		0xFFFFFFFFu
#define	MAN_NAME_MAX		32

/* ---- "echool" service ---- */
/*
 * OOL oracle.  The caller sends one OOL descriptor; the dispatcher reads
 * the payload straight from the sender's address space and replies with
 * a bare header whose msgh_id is its FNV-1a checksum.  Lets ring 3 check
 * its OOL descriptors against the kernel's parser; the receive side is
 * covered by stress_ool.
 */
#define	SVC_ECHOOL_NAME		"echool"
#define	ECHOOL_OP_CHECKSUM	1

/* ---- "launchd" service ---- */
/*
 * launchd (mach/launchd.c): a registry of managed jobs, each a label
 * (Apple's Label), a progreg program name (e.g. "echod"), a state and a
 * task id.
 *
 *	LAUNCHCTL_OP_LIST	every entry; a RUNNING entry whose task is
 *				gone is marked EXITED first.
 *	LAUNCHCTL_OP_LOAD	register a label + program and spawn it:
 *				RUNNING, or FAILED if the spawn failed.  A
 *				duplicate label is MACH_E_INVAL.
 *	LAUNCHCTL_OP_UNLOAD	remove the entry and kill its task.
 *	LAUNCHCTL_OP_STOP	kill the task, keep the entry STOPPED.
 *	LAUNCHCTL_OP_START	respawn an entry that is not RUNNING.
 *
 * All inline, no OOL.  Not done: loading job files from disk, auth
 * between tasks.
 */
#define	SVC_LAUNCHD_NAME	"launchd"

#define	LAUNCHCTL_OP_LIST	1
#define	LAUNCHCTL_OP_LOAD	2
#define	LAUNCHCTL_OP_UNLOAD	3
#define	LAUNCHCTL_OP_STOP	4	/* kill task, keep entry         */
#define	LAUNCHCTL_OP_START	5	/* respawn a stopped entry       */

#define	LAUNCHD_MAX_SERVICES	8
#define	LAUNCHD_NAME_MAX	24
#define	LAUNCHD_PROGRAM_MAX	24

/* LOAD-request flag bits (lr_flags). */
#define	LAUNCHD_LOAD_FLAG_KEEPALIVE	0x1u	/* respawn on unexpected exit */

/*
 * Entry states.  LOAD always spawns, so a loaded entry is RUNNING or
 * FAILED; only a catalog job without runatload starts out STOPPED.
 * EXITED means the task was found gone (LIST, STOP, START).  The
 * keep_alive worker respawns a dead keep_alive job, or parks it
 * THROTTLED.
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

/* WIRE FORMAT.  ABI-stable.  UNLOAD / STOP / START request body. */
struct svc_launchctl_byname_req {
	char		lr_name[LAUNCHD_NAME_MAX];
};

_Static_assert(sizeof(struct svc_launchctl_byname_req) == 24,
    "svc_launchctl_byname_req must be 24 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  Reply body for all but LIST. */
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

/*
 * Create and register the services above, launchd included.  After
 * bootstrap_init and task_subsystem_init.
 */
void	services_init(void);

#endif /* !_SYS_SERVICES_H_ */
