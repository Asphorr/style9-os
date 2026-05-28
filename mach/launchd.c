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
#include "kmem.h"
#include "kprintf.h"
#include "launchd.h"
#include "panic.h"
#include "port.h"
#include "progreg.h"
#include "services.h"
#include "spinlock.h"
#include "task.h"

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
 *	UNLOAD	drop the entry from the registry.  A still-running
 *		task is left to finish on its own -- we have no
 *		SYS_TASK_KILL primitive yet, and forging one through
 *		exception ports would force every managed daemon to
 *		opt into the reply protocol.  Deferred for v2.
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
	uint8_t		lc_state;	/* LAUNCHD_STATE_*               */
	uint16_t	lc_pad;
	uint32_t	lc_pad2;
	uint64_t	lc_task_id;
	char		lc_name[LAUNCHD_NAME_MAX];
	char		lc_program[LAUNCHD_PROGRAM_MAX];
};

static struct spinlock		 s9launchd_lock = SPINLOCK_INIT("s9launchd");
static struct launchd_cell	 s9launchd_cells[LAUNCHD_MAX_SERVICES];

extern struct port_space	*kernel_space;
extern struct port		*port_create_kernel_owned(uint8_t kind,
				    void *arg);
extern int			 port_install_send_in_kernel(struct port *,
				    mach_port_name_t *name_out);
static struct port		*s9launchd_port;	/* (c) */

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
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}
	if (free_idx < 0) {
		spin_unlock(&s9launchd_lock);
		reply.ls_status  = MACH_E_NOSPACE;
		reply.ls_state   = LAUNCHD_STATE_FAILED;
		reply.ls_task_id = 0;
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
		c->lc_state   = LAUNCHD_STATE_FAILED;	/* pessimistic */
		c->lc_task_id = 0;

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
			reply.ls_status  = MACH_MSG_OK;
		}
		reply.ls_state   = c->lc_state;
		reply.ls_task_id = c->lc_task_id;
	}
	spin_unlock(&s9launchd_lock);

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
		return (svc_reply_inline(req, from, &reply, sizeof(reply)));
	}

	prev_state = s9launchd_cells[hit_idx].lc_state;
	prev_task  = s9launchd_cells[hit_idx].lc_task_id;
	/*
	 * Best-effort kill is deferred (no SYS_TASK_KILL primitive yet).
	 * The bookkeeping cell is freed now so a subsequent LOAD of the
	 * same name succeeds; the underlying task continues until it
	 * exits on its own.  Loud kprintf so the operator notices.
	 */
	if (prev_state == LAUNCHD_STATE_RUNNING && prev_task != 0 &&
	    task_is_alive(prev_task)) {
		/*
		 * kprintf has no %.*s; lc_name is NUL-padded by
		 * copy_bounded so %s is safe.
		 */
		kprintf("launchd: unloading '%s' while task %llu still alive "
		    "(no kill primitive in v1, task continues)\n",
		    s9launchd_cells[hit_idx].lc_name,
		    (unsigned long long)prev_task);
	}

	s9launchd_cells[hit_idx].lc_used    = false;
	s9launchd_cells[hit_idx].lc_state   = LAUNCHD_STATE_EXITED;
	s9launchd_cells[hit_idx].lc_task_id = 0;
	for (i = 0; i < LAUNCHD_NAME_MAX; i++)
		s9launchd_cells[hit_idx].lc_name[i] = '\0';
	for (i = 0; i < LAUNCHD_PROGRAM_MAX; i++)
		s9launchd_cells[hit_idx].lc_program[i] = '\0';
	spin_unlock(&s9launchd_lock);

	reply.ls_status  = MACH_MSG_OK;
	reply.ls_state   = prev_state;
	reply.ls_task_id = prev_task;
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
	default:
		return (MACH_E_INVAL);
	}
}

/* ---- bring-up -------------------------------------------------------- */

void
launchd_subsystem_init(void)
{
	mach_port_name_t	kn;
	size_t			i;

	for (i = 0; i < LAUNCHD_MAX_SERVICES; i++) {
		s9launchd_cells[i].lc_used    = false;
		s9launchd_cells[i].lc_state   = LAUNCHD_STATE_EXITED;
		s9launchd_cells[i].lc_task_id = 0;
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
}
