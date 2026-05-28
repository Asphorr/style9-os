/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bootstrap.h"
#include "clock.h"
#include "kmem.h"
#include "kprintf.h"
#include "launchd.h"
#include "panic.h"
#include "port.h"
#include "port_internal.h"
#include "progreg.h"
#include "services.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

/*
 * Minimal launchd: a service supervisor with three operations.
 *
 * The contract is the bare minimum to demonstrate a launchd-shaped
 * surface on top of the bootstrap port:
 *
 *	LIST	enumerate the current registry, re-validating every
 *		RUNNING entry's task liveness via task_is_alive at
 *		snapshot time.  Stale entries flip to EXITED in place
 *		before the reply is built so the caller sees a fresh
 *		state machine without having to re-query.
 *
 *	LOAD	register a label + program-name pair, immediately
 *		spawn the program via progreg_spawn, and stash the
 *		new task id.  Duplicate labels are rejected.  Spawn
 *		failures land the entry in FAILED so the operator can
 *		see what happened on a subsequent LIST.
 *
 *	UNLOAD	drop the entry from the registry AND request async
 *		termination of the underlying task via
 *		task_request_terminate (v2: was warn-and-orphan in v1
 *		when no kill primitive existed).  Termination is
 *		asynchronous: the call returns immediately and the
 *		target dies at its next syscall / wake.  A subsequent
 *		LIST shows the entry gone; a subsequent task_alive
 *		probe on the cached task_id returns false once the
 *		zombie has been reaped by idle.
 *
 * Internal layout: one bounded-size array (LAUNCHD_MAX_SERVICES rows)
 * under one spinlock.  No dynamic allocation per row; the limit is
 * fine for the small-system workloads this kernel targets and avoids
 * a kmem path on every load.  Per-row state:
 *	lc_used		false when the slot is free
 *	lc_name		caller-supplied label (LAUNCHD_NAME_MAX bytes)
 *	lc_program	progreg name to spawn
 *	lc_state	LAUNCHD_STATE_* -- updated on LOAD, refreshed at LIST
 *	lc_task_id	0 when not running
 *
 * Locking: all of the above is `(s)` under `s9launchd_lock`.  The
 * lock is taken across the whole dispatcher body except the actual
 * mach_msg_send (the reply), which happens after dropping the lock
 * to keep IPC fan-out outside the critical section.
 *
 * No WITNESS cycle concerns: s9launchd_lock is a leaf -- the only
 * thing held inside it is the bounded loop over our own array.
 */

struct launchd_cell {
	bool		lc_used;
	bool		lc_keepalive;	/* respawn on unexpected exit    */
	uint8_t		lc_state;	/* LAUNCHD_STATE_*               */
	uint8_t		lc_pad;
	uint32_t	lc_fast_crashes; /* consecutive sub-throttle exits */
	uint64_t	lc_task_id;
	uint64_t	lc_last_spawn_ms; /* clock_uptime_ms at last spawn */
	char		lc_name[LAUNCHD_NAME_MAX];
	char		lc_program[LAUNCHD_PROGRAM_MAX];
};

/*
 * keep_alive respawn throttle.  A keep_alive job that dies in under
 * LAUNCHD_THROTTLE_MS lived "too fast"; after LAUNCHD_THROTTLE_MAX such
 * fast deaths in a row the worker stops respawning it and parks it in
 * LAUNCHD_STATE_THROTTLED (a launchctl START clears the count and
 * revives it).  Any single run lasting at least LAUNCHD_THROTTLE_MS
 * resets the counter -- the job proved it can stay up.  This is the
 * crash-loop backoff every supervisor grows; we give up rather than
 * delay-and-retry because the cooperative worker has no timer wheel to
 * schedule a deferred respawn against.
 */
#define	LAUNCHD_THROTTLE_MS	1000u
#define	LAUNCHD_THROTTLE_MAX	3u

static struct spinlock		 s9launchd_lock = SPINLOCK_INIT("s9launchd");
static struct launchd_cell	 s9launchd_cells[LAUNCHD_MAX_SERVICES];

extern struct port_space	*kernel_space;
extern struct port		*port_create_kernel_owned(uint8_t kind,
				    void *arg);
extern int			 port_install_send_in_kernel(struct port *,
				    mach_port_name_t *name_out);
static struct port		*s9launchd_port;	/* (c) */

/*
 * keep_alive plumbing.  The worker thread blocks on s9launchd_death_port
 * (a kernel-owned RECV+SEND port, named s9launchd_death_name in
 * kernel_space for the recv).  For every RUNNING + keep_alive cell the
 * load/start/respawn paths arm a DEAD_NAME watch on the child's
 * task-self port via port_arm_dead_name_object, tagging it with the cell
 * index; when the child dies the notification lands here and the worker
 * respawns it.  Both (c): set once at init, never reassigned.
 */
static mach_port_name_t		 s9launchd_death_name;	/* (c) */
static struct port		*s9launchd_death_port;	/* (c) */

/*
 * The boot catalog -- the style9 answer to launchd.plist.  Where Apple
 * encodes a job as an XML property list, the s9 jobspec is line-oriented
 * + brace-free: one `job <label>' stanza, indented keys, terminated by
 * `end'.  Keys:
 *	program <name>	progreg image to run (required)
 *	keepalive	respawn on unexpected exit
 *	runatload	spawn at boot (else registered STOPPED, started on
 *			demand via launchctl start)
 * `#' lines + blank lines are ignored.  Compiled in (no root filesystem
 * to read a catalog from yet); parsed once by the worker thread.  (c).
 */
static const char	s9launchd_catalog[] =
	"# style9 launchd boot catalog -- s9 jobspec v1\n"
	"# the brace-free, no-XML answer to launchd.plist\n"
	"\n"
	"job com.style9.heartbeat\n"
	"    program heartbeatd\n"
	"    keepalive\n"
	"    runatload\n"
	"end\n"
	"\n"
	"job com.style9.ondemand\n"
	"    program heartbeatd\n"
	"end\n";

/*
 * String helpers.  Bounded byte-by-byte copy + compare; the wire-
 * format buffers are fixed-size + NUL-padded by the sender so the
 * loop never reads past the field.
 */
static void
copy_bounded(char *dst, const char *src, size_t cap)
{
	size_t	i;

	for (i = 0; i < cap; i++) {
		dst[i] = src[i];
		if (src[i] == '\0')
			break;
	}
	for (; i < cap; i++)
		dst[i] = '\0';
}

static bool
eq_bounded(const char *a, const char *b, size_t cap)
{
	size_t	i;

	for (i = 0; i < cap; i++) {
		if (a[i] != b[i])
			return (false);
		if (a[i] == '\0')
			return (true);
	}
	return (true);
}

/*
 * Refresh an entry's runtime state without taking any lock other
 * than the caller's already-held s9launchd_lock.  task_is_alive does
 * its own tasks_lock dance internally; we just call it under our
 * own lock since the lock orders never cross.
 */
static void
refresh_state_locked(struct launchd_cell *c)
{

	if (c->lc_state == LAUNCHD_STATE_RUNNING && c->lc_task_id != 0) {
		if (!task_is_alive(c->lc_task_id))
			c->lc_state = LAUNCHD_STATE_EXITED;
	}
}

/*
 * Stamp the cell's spawn time, under s9launchd_lock, right after a
 * (re)spawn records the new task id.  launchd_handle_death diffs the
 * death time against this stamp to classify the run as a fast crash or
 * a healthy run for the respawn throttle.  Leaves lc_fast_crashes alone
 * -- the death path owns that counter.
 */
static void
note_spawn_locked(struct launchd_cell *c)
{

	c->lc_last_spawn_ms = clock_uptime_ms();
}

/*
 * arm_keepalive: install a DEAD_NAME watch on `task_id`'s task-self port
 * so the worker is notified when the task dies, tagged with the cell
 * index so the worker knows which entry to respawn.  Best-effort: a
 * task that already died (or has no self port) just won't be watched --
 * the worst case is a missed restart of an instance that lived for less
 * than the spawn-to-arm window.  Called OUTSIDE s9launchd_lock so the
 * port/task locks never nest under our leaf lock.
 *
 * task_self_port_for hands us a transient SEND ref purely to keep the
 * port pinned across the arm; port_arm_dead_name_object takes its own
 * ref on the death port (released when the one-shot fires), so we drop
 * ours immediately after.
 */
static void
arm_keepalive(uint64_t task_id, int idx)
{
	struct port	*sp;

	if (s9launchd_death_port == NULL)
		return;
	sp = task_self_port_for(task_id);
	if (sp == NULL)
		return;
	(void)port_arm_dead_name_object(sp, s9launchd_death_port,
	    (uint32_t)idx);
	port_deref(sp, MACH_PORT_RIGHT_SEND);
}

/*
 * Common reply-shaping helper, copied from services.c's pattern.
 * Builds a header in `buf`, points the inline payload bytes at `body`
 * of `body_size`, and sends back to req->msgh_local via COPY_SEND on
 * the caller's space.
 */
static int
svc_reply_inline(const struct mach_msg_header *req, struct port_space *from,
    const void *body, size_t body_size)
{
	uint8_t		buf[sizeof(struct mach_msg_header) + 1024];
	struct mach_msg_header	*rhdr;
	const uint8_t	*src;
	uint8_t		*dst;
	size_t		 total, i;

	total = sizeof(struct mach_msg_header) + body_size;
	if (total > sizeof(buf))
		return (MACH_E_NOMEM);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	rhdr = (struct mach_msg_header *)buf;
	rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	rhdr->msgh_size    = (uint32_t)total;
	rhdr->msgh_remote  = req->msgh_local;
	rhdr->msgh_local   = MACH_PORT_NULL;
	rhdr->msgh_voucher = 0;
	rhdr->msgh_id      = req->msgh_id;

	dst = buf + sizeof(struct mach_msg_header);
	src = (const uint8_t *)body;
	for (i = 0; i < body_size; i++)
		dst[i] = src[i];

	return (mach_msg_send(from, rhdr));
}

/* ---- op handlers ---------------------------------------------------- */

static int
op_list(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_launchctl_list_reply	r;
	size_t				i, j, n;

	for (i = 0; i < sizeof(r); i++)
		((uint8_t *)&r)[i] = 0;

	spin_lock(&s9launchd_lock);
	n = 0;
	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		struct launchd_cell *c = &s9launchd_cells[i];

		if (!c->lc_used)
			continue;
		refresh_state_locked(c);

		copy_bounded(r.ll_entries[n].le_name,    c->lc_name,
		    LAUNCHD_NAME_MAX);
		copy_bounded(r.ll_entries[n].le_program, c->lc_program,
		    LAUNCHD_PROGRAM_MAX);
		r.ll_entries[n].le_state   = c->lc_state;
		r.ll_entries[n].le_pad     = 0;
		r.ll_entries[n].le_task_id = c->lc_task_id;
		n++;
	}
	spin_unlock(&s9launchd_lock);

	r.ll_count = (uint32_t)n;
	r.ll_pad   = 0;
	(void)j;
	return (svc_reply_inline(req, from, &r, sizeof(r)));
}

static int
op_load(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_launchctl_load_req		body;
	struct svc_launchctl_status_reply	reply;
	const uint8_t				*p;
	size_t					 body_off, body_size, i;
	long					 child_id;
	int					 free_idx;
	int					 dup_idx;

	body_off  = sizeof(struct mach_msg_header);
	body_size = req->msgh_size > body_off ?
	    (size_t)(req->msgh_size - body_off) : 0;
	if (body_size < sizeof(body))
		return (MACH_E_INVAL);

	p = (const uint8_t *)req + body_off;
	for (i = 0; i < sizeof(body); i++)
		((uint8_t *)&body)[i] = p[i];

	/*
	 * Reject empty labels + empty programs so the registry never
	 * carries a zero-length name that would silently match the
	 * first byte of any other entry's NUL padding.
	 */
	if (body.lr_name[0] == '\0' || body.lr_program[0] == '\0')
		return (MACH_E_INVAL);

	spin_lock(&s9launchd_lock);
	free_idx = -1;
	dup_idx  = -1;
	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		struct launchd_cell *c = &s9launchd_cells[i];

		if (!c->lc_used) {
			if (free_idx < 0)
				free_idx = (int)i;
			continue;
		}
		if (eq_bounded(c->lc_name, body.lr_name, LAUNCHD_NAME_MAX)) {
			dup_idx = (int)i;
			break;
		}
	}

	if (dup_idx >= 0) {
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_E_INVAL;
		reply.ls_state   = LAUNCHD_STATE_FAILED;
		reply.ls_task_id = 0;
		reply.ls_taskport = MACH_PORT_NULL;
		reply.ls_pad     = 0;
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}
	if (free_idx < 0) {
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_E_NOSPACE;
		reply.ls_state   = LAUNCHD_STATE_FAILED;
		reply.ls_task_id = 0;
		reply.ls_taskport = MACH_PORT_NULL;
		reply.ls_pad     = 0;
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}

	/*
	 * Claim the slot before dropping the lock so two concurrent
	 * LOADs of the same name don't both find free_idx and race.
	 * We hold the lock across progreg_spawn -- spawn does take
	 * tasks_lock internally, which is finer-grained than ours;
	 * no cycle.
	 */
	{
		struct launchd_cell *c = &s9launchd_cells[free_idx];

		c->lc_used = true;
		copy_bounded(c->lc_name,    body.lr_name,    LAUNCHD_NAME_MAX);
		copy_bounded(c->lc_program, body.lr_program, LAUNCHD_PROGRAM_MAX);
		c->lc_keepalive = (body.lr_flags &
		    LAUNCHD_LOAD_FLAG_KEEPALIVE) != 0;
		c->lc_state        = LAUNCHD_STATE_FAILED;	/* pessimistic */
		c->lc_task_id      = 0;
		c->lc_fast_crashes = 0;

		spin_unlock(&s9launchd_lock);

		child_id = progreg_spawn(c->lc_program);

		spin_lock(&s9launchd_lock);
		if (child_id < 0) {
			c->lc_state   = LAUNCHD_STATE_FAILED;
			c->lc_task_id = 0;
			reply.ls_status  = (int32_t)child_id;
		} else {
			c->lc_state   = LAUNCHD_STATE_RUNNING;
			c->lc_task_id = (uint64_t)child_id;
			note_spawn_locked(c);
			reply.ls_status  = MACH_MSG_OK;
		}
		reply.ls_state   = c->lc_state;
		reply.ls_task_id = c->lc_task_id;
	}
	spin_unlock(&s9launchd_lock);

	/*
	 * On success, hand the caller a SEND right on the child's
	 * task-self port: take a transient ref via task_self_port_for,
	 * install it as a fresh name in the caller's space, then drop the
	 * transient ref (space_install took its own ref for the installed
	 * name).  The name lets launchctl arm a DEAD_NAME notification and
	 * learn of the child's death by notification instead of polling
	 * task_alive.  Done outside s9launchd_lock so the port + space
	 * locks never nest under our leaf lock.  Best-effort: a failed
	 * install reports 0 and launchctl falls back to the poll path.
	 */
	reply.ls_taskport = MACH_PORT_NULL;
	reply.ls_pad      = 0;
	if (reply.ls_status == MACH_MSG_OK && reply.ls_task_id != 0) {
		struct port	*sp;

		sp = task_self_port_for(reply.ls_task_id);
		if (sp != NULL) {
			mach_port_name_t	tpn;

			if (space_install(from, sp, MACH_PORT_RIGHT_SEND,
			    &tpn) == MACH_MSG_OK)
				reply.ls_taskport = tpn;
			port_deref(sp, MACH_PORT_RIGHT_SEND);
		}
	}

	/*
	 * keep_alive: arm launchd's own DEAD_NAME watch on the child so the
	 * worker respawns it on unexpected exit.  Independent of the SEND
	 * handed to the caller above (each takes its own ref).
	 */
	if (reply.ls_status == MACH_MSG_OK && reply.ls_task_id != 0 &&
	    (body.lr_flags & LAUNCHD_LOAD_FLAG_KEEPALIVE) != 0)
		arm_keepalive(reply.ls_task_id, free_idx);

	return (svc_reply_inline(req, from, &reply, sizeof(reply)));
}

static int
op_unload(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_launchctl_byname_req		body;
	struct svc_launchctl_status_reply	reply;
	const uint8_t				*p;
	size_t					 body_off, body_size, i;
	int					 hit_idx;
	uint8_t					 prev_state;
	uint64_t				 prev_task;

	body_off  = sizeof(struct mach_msg_header);
	body_size = req->msgh_size > body_off ?
	    (size_t)(req->msgh_size - body_off) : 0;
	if (body_size < sizeof(body))
		return (MACH_E_INVAL);

	p = (const uint8_t *)req + body_off;
	for (i = 0; i < sizeof(body); i++)
		((uint8_t *)&body)[i] = p[i];

	if (body.lr_name[0] == '\0')
		return (MACH_E_INVAL);

	spin_lock(&s9launchd_lock);
	hit_idx = -1;
	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		struct launchd_cell *c = &s9launchd_cells[i];

		if (!c->lc_used)
			continue;
		if (eq_bounded(c->lc_name, body.lr_name, LAUNCHD_NAME_MAX)) {
			hit_idx = (int)i;
			break;
		}
	}

	if (hit_idx < 0) {
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_E_NAME;
		reply.ls_state   = LAUNCHD_STATE_EXITED;
		reply.ls_task_id = 0;
		reply.ls_taskport = MACH_PORT_NULL;
		reply.ls_pad     = 0;
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}

	prev_state = s9launchd_cells[hit_idx].lc_state;
	prev_task  = s9launchd_cells[hit_idx].lc_task_id;

	s9launchd_cells[hit_idx].lc_used    = false;
	s9launchd_cells[hit_idx].lc_state   = LAUNCHD_STATE_EXITED;
	s9launchd_cells[hit_idx].lc_task_id = 0;
	for (i = 0; i < LAUNCHD_NAME_MAX; i++)
		s9launchd_cells[hit_idx].lc_name[i] = '\0';
	for (i = 0; i < LAUNCHD_PROGRAM_MAX; i++)
		s9launchd_cells[hit_idx].lc_program[i] = '\0';
	spin_unlock(&s9launchd_lock);

	/*
	 * Issue async termination AFTER dropping s9launchd_lock.
	 * task_request_terminate takes tasks_lock + t_lock + sched_lock;
	 * keeping the kill outside our own lock keeps the s9launchd
	 * leaf-lock leaf-shaped and avoids a long fan-out under it.  No
	 * cycle if we'd done it inside either, but the discipline is
	 * cleaner.  Skips kernel_task internally + silently no-ops on
	 * ids that have already exited (the race vs. a self-exiting
	 * daemon is harmless).
	 */
	if (prev_state == LAUNCHD_STATE_RUNNING && prev_task != 0)
		task_request_terminate(prev_task);

	reply.ls_status  = MACH_MSG_OK;
	reply.ls_state   = prev_state;
	reply.ls_task_id = prev_task;
	reply.ls_taskport = MACH_PORT_NULL;
	reply.ls_pad     = 0;
	return (svc_reply_inline(req, from, &reply, sizeof(reply)));
}

/*
 * op_stop: kill the entry's task but KEEP the registry row, parking it
 * in STATE_STOPPED.  Distinct from UNLOAD (which removes the row): a
 * STOPPED entry can be revived with op_start, and -- crucially -- the
 * STOPPED state tells the keep_alive worker the death it is about to
 * observe was intentional, so it must not respawn.  The state flip is
 * published BEFORE the kill so the worker can never catch a window of
 * RUNNING + dead-task and race a spurious restart in.
 */
static int
op_stop(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_launchctl_byname_req		body;
	struct svc_launchctl_status_reply	reply;
	const uint8_t				*p;
	size_t					 body_off, body_size, i;
	int					 hit_idx;
	uint8_t					 prev_state;
	uint64_t				 prev_task;

	body_off  = sizeof(struct mach_msg_header);
	body_size = req->msgh_size > body_off ?
	    (size_t)(req->msgh_size - body_off) : 0;
	if (body_size < sizeof(body))
		return (MACH_E_INVAL);

	p = (const uint8_t *)req + body_off;
	for (i = 0; i < sizeof(body); i++)
		((uint8_t *)&body)[i] = p[i];

	if (body.lr_name[0] == '\0')
		return (MACH_E_INVAL);

	reply.ls_taskport = MACH_PORT_NULL;
	reply.ls_pad      = 0;

	spin_lock(&s9launchd_lock);
	hit_idx = -1;
	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		struct launchd_cell *c = &s9launchd_cells[i];

		if (!c->lc_used)
			continue;
		if (eq_bounded(c->lc_name, body.lr_name, LAUNCHD_NAME_MAX)) {
			hit_idx = (int)i;
			break;
		}
	}
	if (hit_idx < 0) {
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_E_NAME;
		reply.ls_state   = LAUNCHD_STATE_EXITED;
		reply.ls_task_id = 0;
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}

	refresh_state_locked(&s9launchd_cells[hit_idx]);
	prev_state = s9launchd_cells[hit_idx].lc_state;
	prev_task  = s9launchd_cells[hit_idx].lc_task_id;

	s9launchd_cells[hit_idx].lc_state   = LAUNCHD_STATE_STOPPED;
	s9launchd_cells[hit_idx].lc_task_id = 0;
	spin_unlock(&s9launchd_lock);

	if (prev_state == LAUNCHD_STATE_RUNNING && prev_task != 0)
		task_request_terminate(prev_task);

	reply.ls_status  = MACH_MSG_OK;
	reply.ls_state   = LAUNCHD_STATE_STOPPED;
	reply.ls_task_id = prev_task;
	return (svc_reply_inline(req, from, &reply, sizeof(reply)));
}

/*
 * op_start: (re)spawn a non-running entry, returning it to RUNNING.
 * Idempotent on an already-RUNNING entry (no-op success).  The program
 * name is snapshotted under the lock and the spawn issued after
 * dropping it, mirroring op_load -- progreg_spawn takes tasks_lock,
 * finer than ours, and never yields, so hit_idx stays valid across the
 * unlock/relock.
 */
static int
op_start(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_launchctl_byname_req		body;
	struct svc_launchctl_status_reply	reply;
	const uint8_t				*p;
	char					 program[LAUNCHD_PROGRAM_MAX];
	size_t					 body_off, body_size, i;
	long					 child_id;
	int					 hit_idx;

	body_off  = sizeof(struct mach_msg_header);
	body_size = req->msgh_size > body_off ?
	    (size_t)(req->msgh_size - body_off) : 0;
	if (body_size < sizeof(body))
		return (MACH_E_INVAL);

	p = (const uint8_t *)req + body_off;
	for (i = 0; i < sizeof(body); i++)
		((uint8_t *)&body)[i] = p[i];

	if (body.lr_name[0] == '\0')
		return (MACH_E_INVAL);

	reply.ls_taskport = MACH_PORT_NULL;
	reply.ls_pad      = 0;

	spin_lock(&s9launchd_lock);
	hit_idx = -1;
	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		struct launchd_cell *c = &s9launchd_cells[i];

		if (!c->lc_used)
			continue;
		if (eq_bounded(c->lc_name, body.lr_name, LAUNCHD_NAME_MAX)) {
			hit_idx = (int)i;
			break;
		}
	}
	if (hit_idx < 0) {
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_E_NAME;
		reply.ls_state   = LAUNCHD_STATE_EXITED;
		reply.ls_task_id = 0;
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}

	refresh_state_locked(&s9launchd_cells[hit_idx]);
	if (s9launchd_cells[hit_idx].lc_state == LAUNCHD_STATE_RUNNING) {
		reply.ls_task_id = s9launchd_cells[hit_idx].lc_task_id;
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_MSG_OK;
		reply.ls_state   = LAUNCHD_STATE_RUNNING;
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}

	copy_bounded(program, s9launchd_cells[hit_idx].lc_program,
	    LAUNCHD_PROGRAM_MAX);
	s9launchd_cells[hit_idx].lc_state = LAUNCHD_STATE_FAILED;	/* pessimistic */
	spin_unlock(&s9launchd_lock);

	child_id = progreg_spawn(program);

	spin_lock(&s9launchd_lock);
	if (child_id < 0) {
		s9launchd_cells[hit_idx].lc_state   = LAUNCHD_STATE_FAILED;
		s9launchd_cells[hit_idx].lc_task_id = 0;
		reply.ls_status  = (int32_t)child_id;
	} else {
		/*
		 * A manual START is an operator override: clear the fast-crash
		 * count so a job that had been THROTTLED gets a clean slate.
		 */
		s9launchd_cells[hit_idx].lc_state        = LAUNCHD_STATE_RUNNING;
		s9launchd_cells[hit_idx].lc_task_id      = (uint64_t)child_id;
		s9launchd_cells[hit_idx].lc_fast_crashes = 0;
		note_spawn_locked(&s9launchd_cells[hit_idx]);
		reply.ls_status  = MACH_MSG_OK;
	}
	reply.ls_state   = s9launchd_cells[hit_idx].lc_state;
	reply.ls_task_id = s9launchd_cells[hit_idx].lc_task_id;
	spin_unlock(&s9launchd_lock);

	/* Re-arm the keep_alive watch on the freshly respawned instance. */
	if (reply.ls_status == MACH_MSG_OK && reply.ls_task_id != 0 &&
	    s9launchd_cells[hit_idx].lc_keepalive)
		arm_keepalive(reply.ls_task_id, hit_idx);

	return (svc_reply_inline(req, from, &reply, sizeof(reply)));
}

/* ---- dispatcher entry ----------------------------------------------- */

static int
svc_launchd_dispatch(const struct mach_msg_header *req, struct port_space *from)
{

	switch (req->msgh_id) {
	case LAUNCHCTL_OP_LIST:
		return (op_list(req, from));
	case LAUNCHCTL_OP_LOAD:
		return (op_load(req, from));
	case LAUNCHCTL_OP_UNLOAD:
		return (op_unload(req, from));
	case LAUNCHCTL_OP_STOP:
		return (op_stop(req, from));
	case LAUNCHCTL_OP_START:
		return (op_start(req, from));
	default:
		return (MACH_E_INVAL);
	}
}

/* ---- keep_alive worker ---------------------------------------------- */

/*
 * launchd_handle_death: a DEAD_NAME notification fired for cell `idx`.
 * Respawn iff the cell is still a live keep_alive job whose recorded
 * task is genuinely gone.  The !task_is_alive guard rejects two cases
 * the bare DEAD_NAME tag cannot distinguish: an entry STOPPED or
 * UNLOADED on purpose (state no longer RUNNING / lc_used cleared), and
 * a stale notification for a since-reused cell index (whose current
 * task is alive).  Program name is snapshotted under the lock; the
 * spawn + re-arm run outside it, mirroring op_load / op_start.
 */
static void
launchd_handle_death(int idx)
{
	char		program[LAUNCHD_PROGRAM_MAX];
	long		child_id;
	uint32_t	crashes;
	bool		respawn;
	bool		throttled;

	if (idx < 0 || idx >= LAUNCHD_MAX_SERVICES)
		return;

	respawn   = false;
	throttled = false;
	crashes   = 0;
	spin_lock(&s9launchd_lock);
	if (s9launchd_cells[idx].lc_used &&
	    s9launchd_cells[idx].lc_keepalive &&
	    s9launchd_cells[idx].lc_state == LAUNCHD_STATE_RUNNING &&
	    !task_is_alive(s9launchd_cells[idx].lc_task_id)) {
		struct launchd_cell	*c;
		uint64_t		 lifetime_ms;

		c = &s9launchd_cells[idx];
		lifetime_ms = clock_uptime_ms() - c->lc_last_spawn_ms;
		copy_bounded(program, c->lc_program, LAUNCHD_PROGRAM_MAX);
		c->lc_task_id = 0;

		if (lifetime_ms >= LAUNCHD_THROTTLE_MS) {
			/* Ran long enough to be healthy: forgive past crashes. */
			c->lc_fast_crashes = 0;
			c->lc_state        = LAUNCHD_STATE_FAILED;
			respawn            = true;
		} else if (++c->lc_fast_crashes >= LAUNCHD_THROTTLE_MAX) {
			/* Crash loop: stop respawning until a manual START. */
			c->lc_state = LAUNCHD_STATE_THROTTLED;
			throttled   = true;
			crashes     = c->lc_fast_crashes;
		} else {
			c->lc_state = LAUNCHD_STATE_FAILED;
			respawn     = true;
		}
	}
	spin_unlock(&s9launchd_lock);

	if (throttled) {
		kprintf("launchd: keep_alive '%s' respawning too fast "
		    "(%u crashes under %ums) -- throttled, START to revive\n",
		    program, (unsigned)crashes, LAUNCHD_THROTTLE_MS);
		return;
	}
	if (!respawn)
		return;

	child_id = progreg_spawn(program);

	spin_lock(&s9launchd_lock);
	/*
	 * idx still names the same job: the cooperative kernel never
	 * preempts us here and progreg_spawn does not yield, so no
	 * concurrent UNLOAD could have freed + reused the cell.
	 */
	if (child_id < 0) {
		s9launchd_cells[idx].lc_state   = LAUNCHD_STATE_FAILED;
		s9launchd_cells[idx].lc_task_id = 0;
	} else {
		s9launchd_cells[idx].lc_state   = LAUNCHD_STATE_RUNNING;
		s9launchd_cells[idx].lc_task_id = (uint64_t)child_id;
		note_spawn_locked(&s9launchd_cells[idx]);
	}
	spin_unlock(&s9launchd_lock);

	if (child_id >= 0) {
		kprintf("launchd: keep_alive respawned '%s' -> task %llu\n",
		    program, (unsigned long long)child_id);
		arm_keepalive((uint64_t)child_id, idx);
	} else {
		kprintf("launchd: keep_alive respawn of '%s' failed (rv=%ld)\n",
		    program, child_id);
	}
}

static void	launchd_parse_catalog(const char *text);
static void	launchd_worker(void *arg) __attribute__((noreturn));

/*
 * The worker: block on the death port forever, dispatching each
 * DEAD_NAME notification to launchd_handle_death.  Non-DEAD_NAME
 * messages + recv errors are ignored (the death port only ever
 * receives kernel-synthesised notifications).  Runs on kernel_task,
 * structurally identical to the dev uart/kbd driver stream threads.
 */
static void
launchd_worker(void *arg)
{
	struct mach_notify_header	nh;
	int				rv;

	(void)arg;

	/*
	 * Materialise the boot catalog here, not in launchd_subsystem_init:
	 * init runs inside services_init (kmain), BEFORE progreg_init has
	 * populated the program registry and before the scheduler starts
	 * dispatching user tasks.  By the time this worker thread is first
	 * scheduled, kmain has passed progreg_init and spawned its first
	 * ring-3 task, so progreg_spawn here resolves + runs safely.
	 */
	launchd_parse_catalog(s9launchd_catalog);

	for (;;) {
		rv = mach_msg_recv_block(kernel_space, s9launchd_death_name,
		    &nh.hdr, sizeof(nh));
		if (rv != MACH_MSG_OK)
			continue;
		if (nh.hdr.msgh_id != (uint32_t)MACH_NOTIFY_DEAD_NAME)
			continue;
		launchd_handle_death((int)nh.nh_msgid);
	}
}

/* ---- boot catalog (s9 jobspec) -------------------------------------- */

/* Token compare: does s[0..n) equal NUL-terminated `lit' exactly? */
static bool
kw_eq(const char *s, size_t n, const char *lit)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		if (lit[i] == '\0' || s[i] != lit[i])
			return (false);
	}
	return (lit[n] == '\0');
}

/*
 * Copy the whitespace-delimited argument that follows a keyword into
 * `dst' (bounded, NUL-terminated).  `after_kw' points just past the
 * keyword; leading blanks are skipped, the token ends at the next
 * blank / newline / NUL.
 */
static void
copy_token_arg(char *dst, size_t cap, const char *after_kw)
{
	const char	*a;
	size_t		 i;

	a = after_kw;
	while (*a == ' ' || *a == '\t')
		a++;
	for (i = 0; i + 1 < cap && a[i] != '\0' && a[i] != '\n' &&
	    a[i] != ' ' && a[i] != '\t'; i++)
		dst[i] = a[i];
	dst[i] = '\0';
}

/*
 * Materialise one catalog job into the registry.  runatload jobs are
 * spawned immediately (RUNNING, keep_alive armed if requested); others
 * are registered STOPPED so `launchctl start' can bring them up later.
 * Boot-time, single-threaded with respect to the registry, so the
 * free-slot scan needs no dup-check beyond a trusted catalog.
 */
static void
launchd_boot_load(const char *label, const char *program, bool keepalive,
    bool runatload)
{
	struct launchd_cell	*c;
	long			 child_id;
	int			 free_idx;
	size_t			 i;

	spin_lock(&s9launchd_lock);
	free_idx = -1;
	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		if (!s9launchd_cells[i].lc_used) {
			free_idx = (int)i;
			break;
		}
	}
	if (free_idx < 0) {
		spin_unlock(&s9launchd_lock);
		kprintf("launchd: catalog full, dropping '%s'\n", label);
		return;
	}

	c = &s9launchd_cells[free_idx];
	c->lc_used      = true;
	copy_bounded(c->lc_name,    label,   LAUNCHD_NAME_MAX);
	copy_bounded(c->lc_program, program, LAUNCHD_PROGRAM_MAX);
	c->lc_keepalive    = keepalive;
	c->lc_task_id      = 0;
	c->lc_fast_crashes = 0;

	if (!runatload) {
		c->lc_state = LAUNCHD_STATE_STOPPED;
		spin_unlock(&s9launchd_lock);
		kprintf("launchd: catalog registered '%s' (%s) [stopped]\n",
		    label, program);
		return;
	}

	c->lc_state = LAUNCHD_STATE_FAILED;	/* pessimistic, pre-spawn */
	spin_unlock(&s9launchd_lock);

	child_id = progreg_spawn(program);

	spin_lock(&s9launchd_lock);
	if (child_id < 0) {
		s9launchd_cells[free_idx].lc_state   = LAUNCHD_STATE_FAILED;
		s9launchd_cells[free_idx].lc_task_id = 0;
	} else {
		s9launchd_cells[free_idx].lc_state   = LAUNCHD_STATE_RUNNING;
		s9launchd_cells[free_idx].lc_task_id = (uint64_t)child_id;
		note_spawn_locked(&s9launchd_cells[free_idx]);
	}
	spin_unlock(&s9launchd_lock);

	if (child_id < 0) {
		kprintf("launchd: catalog job '%s' (%s) spawn failed rv=%ld\n",
		    label, program, child_id);
		return;
	}
	kprintf("launchd: catalog loaded '%s' (%s) -> task %llu%s\n",
	    label, program, (unsigned long long)child_id,
	    keepalive ? " [keepalive]" : "");
	if (keepalive)
		arm_keepalive((uint64_t)child_id, free_idx);
}

/*
 * Parse the compiled-in catalog one line at a time, accumulating the
 * current stanza's fields and committing on `end'.  A bounded scan: no
 * allocation, every copy capped to its destination field.
 */
static void
launchd_parse_catalog(const char *text)
{
	char		 label[LAUNCHD_NAME_MAX];
	char		 program[LAUNCHD_PROGRAM_MAX];
	const char	*p;
	const char	*kw;
	size_t		 kwlen;
	bool		 have_job, keepalive, runatload;

	have_job  = false;
	keepalive = false;
	runatload = false;
	label[0]   = '\0';
	program[0] = '\0';

	p = text;
	while (*p != '\0') {
		while (*p == ' ' || *p == '\t')
			p++;
		kw = p;
		kwlen = 0;
		while (kw[kwlen] != '\0' && kw[kwlen] != '\n' &&
		    kw[kwlen] != ' ' && kw[kwlen] != '\t')
			kwlen++;

		if (kwlen == 0 || kw[0] == '#') {
			/* blank line or comment */
		} else if (kw_eq(kw, kwlen, "job")) {
			copy_token_arg(label, sizeof(label), kw + kwlen);
			have_job   = true;
			keepalive  = false;
			runatload  = false;
			program[0] = '\0';
		} else if (kw_eq(kw, kwlen, "program")) {
			copy_token_arg(program, sizeof(program), kw + kwlen);
		} else if (kw_eq(kw, kwlen, "keepalive")) {
			keepalive = true;
		} else if (kw_eq(kw, kwlen, "runatload")) {
			runatload = true;
		} else if (kw_eq(kw, kwlen, "end")) {
			if (have_job && program[0] != '\0')
				launchd_boot_load(label, program, keepalive,
				    runatload);
			have_job = false;
		}

		while (*p != '\0' && *p != '\n')
			p++;
		if (*p == '\n')
			p++;
	}
}

/* ---- bring-up -------------------------------------------------------- */

void
launchd_subsystem_init(void)
{
	struct thread		*wth;
	mach_port_name_t	kn;
	uint8_t			dummy;
	size_t			i;

	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		s9launchd_cells[i].lc_used         = false;
		s9launchd_cells[i].lc_keepalive    = false;
		s9launchd_cells[i].lc_state        = LAUNCHD_STATE_EXITED;
		s9launchd_cells[i].lc_fast_crashes = 0;
		s9launchd_cells[i].lc_task_id      = 0;
		s9launchd_cells[i].lc_last_spawn_ms = 0;
		s9launchd_cells[i].lc_name[0]    = '\0';
		s9launchd_cells[i].lc_program[0] = '\0';
	}

	s9launchd_port = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)svc_launchd_dispatch);
	if (s9launchd_port == NULL)
		panic("launchd: port_create_kernel_owned");

	if (port_install_send_in_kernel(s9launchd_port, &kn) != MACH_MSG_OK)
		panic("launchd: install SEND in kernel_space");

	if (bootstrap_register(SVC_LAUNCHD_NAME, kn) != MACH_MSG_OK)
		panic("launchd: bootstrap_register");

	kprintf("svc: %s -> kernel name %u\n", SVC_LAUNCHD_NAME, (unsigned)kn);

	/*
	 * keep_alive worker: a kernel thread that blocks on the death port
	 * and respawns keep_alive jobs whose tasks die.  Mirrors the
	 * dev uart/kbd driver kernel-thread + kernel_space recv-port pattern.  The
	 * death port is RECV+SEND in kernel_space: the RECV name backs the
	 * worker's mach_msg_recv_block, and the port object is the notify
	 * target arm_keepalive registers.
	 */
	s9launchd_death_name = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (s9launchd_death_name == MACH_PORT_NULL)
		panic("launchd: death port allocate");

	s9launchd_death_port = space_lookup(kernel_space, s9launchd_death_name,
	    MACH_PORT_RIGHT_RECEIVE, &dummy);
	if (s9launchd_death_port == NULL)
		panic("launchd: death port lookup");

	wth = thread_create(kernel_task, launchd_worker, NULL, "launchd");
	if (wth == NULL)
		panic("launchd: worker thread_create");
	thread_start(wth);

	kprintf("launchd: keep_alive worker up (death port=%u)\n",
	    (unsigned)s9launchd_death_name);

	/*
	 * The boot catalog is materialised by the worker thread itself (see
	 * launchd_worker): progreg_init has not run yet at this point in
	 * kmain, so spawning here would find an empty program registry.
	 */
}
