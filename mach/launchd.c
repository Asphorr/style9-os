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
 * Minimal launchd: a service supervisor behind the "launchd" service
 * port, plus a keep_alive worker and a compiled-in boot catalog.
 *
 *	LIST	enumerate the registry.  Each RUNNING entry's task is
 *		checked with task_is_alive first, and a dead one flips
 *		to EXITED in place.
 *
 *	LOAD	register a label + program pair and spawn it with
 *		progreg_spawn.  Duplicate labels are refused; a failed
 *		spawn leaves the entry FAILED for LIST to show.
 *
 *	UNLOAD	remove the entry and task_request_terminate its task.
 *		The kill is asynchronous: the task dies at its next
 *		syscall or wake.
 *
 *	STOP	kill the task but keep the entry, STOPPED.
 *
 *	START	respawn a stopped, exited, failed or throttled entry.
 *
 * The registry is a fixed array of LAUNCHD_MAX_SERVICES cells, all
 * fields under s9launchd_lock.  The lock is dropped around anything
 * heavier -- progreg_spawn, task_request_terminate, arm_keepalive, the
 * reply -- so it nests only over task_is_alive's tasks_lock.
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
 * keep_alive respawn throttle.  After LAUNCHD_THROTTLE_MAX deaths in a
 * row each under LAUNCHD_THROTTLE_MS after spawn, the worker stops
 * respawning the job and parks it THROTTLED until a launchctl START.  A
 * run that lasts LAUNCHD_THROTTLE_MS resets the count.  It gives up
 * rather than retry later because the worker has no timer to schedule a
 * deferred respawn against.
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
 * keep_alive plumbing.  The worker blocks on the death port (RECEIVE +
 * SEND in kernel_space).  For each running keep_alive job, load, start
 * and respawn arm a DEAD_NAME watch on the child's task-self port,
 * tagged with the cell index; the child's death lands here and the
 * worker respawns it.
 */
static mach_port_name_t		 s9launchd_death_name;	/* (c) */
static struct port		*s9launchd_death_port;	/* (c) */

/*
 * The boot catalog, in place of launchd.plist: line-oriented stanzas,
 * each `job <label>', indented keys, then `end'.  Keys:
 *	program <name>	progreg image to run (required)
 *	keepalive	respawn on unexpected exit
 *	runatload	spawn at boot (else registered STOPPED, for
 *			launchctl start)
 * `#' lines and blank lines are ignored.  Compiled in; parsed once by
 * launchd_load_catalog.  (c)
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

/* Bounded copy and compare over fixed-size, NUL-padded fields. */
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
 * Mark a RUNNING entry EXITED if its task is gone.  Caller holds
 * s9launchd_lock; task_is_alive takes tasks_lock inside it, and nothing
 * takes the two the other way round.
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
 * Stamp the spawn time, under s9launchd_lock, when a (re)spawn records
 * its task id; launchd_handle_death measures the run against it for the
 * throttle.  lc_fast_crashes belongs to the death path.
 */
static void
note_spawn_locked(struct launchd_cell *c)
{

	c->lc_last_spawn_ms = clock_uptime_ms();
}

/*
 * Watch `task_id`'s task-self port for death, tagged with the cell
 * index.  Best-effort: a task already gone is not watched, so an
 * instance that dies within the spawn-to-arm window is not restarted.
 * Called without s9launchd_lock.
 *
 * task_self_port_for's SEND ref only pins the port across the arm (the
 * watch holds its own ref on the death port), and is dropped at once.
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
 * Reply to req->msgh_local with a bare [header | body] message, as in
 * services.c.  At most 1024 bytes of body.
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
	 * No empty labels or programs: an empty name would match any
	 * cell's NUL padding.
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
	 * Claim the slot before dropping the lock, so two concurrent LOADs
	 * cannot both take it.  The lock is dropped around progreg_spawn.
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
	 * Give the caller a SEND right on the child's task-self port, so
	 * launchctl can watch for its death instead of polling task_alive.
	 * space_install takes its own ref; the transient one from
	 * task_self_port_for is dropped.  Best-effort: on failure the name
	 * is 0 and launchctl polls.
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

	/* keep_alive: launchd's own watch, independent of the caller's. */
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
	 * Kill after dropping s9launchd_lock: task_request_terminate takes
	 * tasks_lock, t_lock and sched_lock and fans out wakes.  It ignores
	 * kernel_task and ids that have already exited.
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
 * Kill the entry's task but keep the entry, STOPPED, for op_start to
 * revive.  STOPPED also tells the keep_alive worker the coming death is
 * intentional.  It is set before the kill, so the worker never sees
 * RUNNING with a dead task and respawns it.
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
 * Respawn a non-running entry; a RUNNING one is left alone and reported
 * as success.  The program name is copied under the lock and the spawn
 * issued after dropping it, as in op_load.  hit_idx is assumed to name
 * the same entry after the relock; nothing re-checks it.
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
		/* A manual START clears the throttle's count. */
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
 * A DEAD_NAME notification for cell `idx`.  Respawn only if the cell is
 * still a RUNNING keep_alive job whose task is really gone: that
 * rejects entries stopped or unloaded on purpose, and a stale
 * notification for a reused cell whose current task is alive.  The
 * spawn and re-arm run outside the lock, as in op_load.
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
			/* Ran long enough: forgive past crashes. */
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
	 * idx is assumed to name the same job still; nothing re-checks
	 * that a concurrent UNLOAD did not free and reuse the cell.
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
 * The worker, a kernel_task thread: receive on the death port for ever
 * and hand each DEAD_NAME notification to launchd_handle_death.
 * Anything else, and receive errors, are ignored.
 */
static void
launchd_worker(void *arg)
{
	struct mach_notify_header	nh;
	int				rv;

	(void)arg;

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
 * Copy the argument after a keyword into `dst' (bounded, NUL-terminated):
 * skip blanks from `after_kw', stop at a blank, newline or NUL.
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
 * Enter one catalog job.  runatload jobs are spawned now (and watched if
 * keepalive); others are registered STOPPED for `launchctl start'.  The
 * catalog is trusted, so there is no duplicate check.
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
 * Parse the catalog a line at a time, committing each stanza at `end'.
 * No allocation; every copy is bounded by its field.
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
	 * The keep_alive worker and its death port, RECEIVE + SEND in
	 * kernel_space: the worker receives on the name, and the port is
	 * the notify target arm_keepalive registers.
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
	 * Not the catalog: progreg_init has not run yet.  kmain calls
	 * launchd_load_catalog once it has.
	 */
}

/*
 * Load the boot catalog, starting every runatload job.  Must run after
 * progreg_init, which fills the registry progreg_spawn resolves program
 * names in; kmain calls it explicitly rather than leave it to the worker
 * thread, which may run before progreg_init.  Idempotent.
 */
void
launchd_load_catalog(void)
{
	static bool	loaded = false;

	if (loaded)
		return;
	loaded = true;
	launchd_parse_catalog(s9launchd_catalog);
}
