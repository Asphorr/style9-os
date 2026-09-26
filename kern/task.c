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
#include "darwin.h"
#include "host.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "port.h"
#include "port_internal.h"
#include "sched.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "vm.h"

/*
 * Lock-key:
 *	(c) const after task_create
 *	(t) protected by the task's own t_lock
 *	(g) protected by tasks_lock (global)
 */

struct task		*kernel_task;

static struct spinlock	tasks_lock = SPINLOCK_INIT("tasks");
static uint64_t		next_task_id;		/* (g) */
static struct task	*tasks_head;		/* (g) unused; see task_list */

void
task_subsystem_init(void)
{

	next_task_id = 1;
	tasks_head   = NULL;

	if (kernel_space == NULL)
		panic("task_subsystem_init: port subsystem not ready");

	kernel_task = task_create("kernel");
	if (kernel_task == NULL)
		panic("task_subsystem_init: kernel_task allocation failed");

	/*
	 * kernel_task alone uses the pre-existing kernel_space rather than
	 * the fresh one task_create made:
	 *	1. Release the task-self install in the throwaway space
	 *	   (dropping the kernel-side RECEIVE ref) so the port is
	 *	   reaped with that space.
	 *	2. Point t_port_space at kernel_space and install there.
	 *	   kernel_space is still empty (nothing allocates between
	 *	   port_subsystem_init and here), so the SEND right lands at
	 *	   name 1, the well-known MACH_PORT_TASK_SELF.
	 */
	port_release_task_self(kernel_task);
	port_space_destroy(kernel_task->t_port_space);
	kernel_task->t_port_space = kernel_space;
	if (port_install_task_self(kernel_task) != MACH_MSG_OK)
		panic("task_subsystem_init: install task_self in kernel_space");
	if (port_install_bootstrap(kernel_task) != MACH_MSG_OK)
		panic("task_subsystem_init: install bootstrap in kernel_space");

	/*
	 * Likewise the pmap: kernel_task runs on kernel_pmap (the tree
	 * boot.S installed), not a fresh one, so switches between kernel
	 * threads never reload CR3.
	 */
	pmap_destroy(kernel_task->t_pmap);
	kernel_task->t_pmap = kernel_pmap;

	kprintf("task: kernel_task id=%llu name=%s, %u initial tasks\n",
	    (unsigned long long)kernel_task->t_id,
	    kernel_task->t_name, 1);
}

struct task *
task_create(const char *name)
{
	struct task	*t;

	t = kmalloc(sizeof(*t));
	if (t == NULL)
		return (NULL);

	spin_init(&t->t_lock, "task");

	spin_lock(&tasks_lock);
	t->t_id = next_task_id++;
	spin_unlock(&tasks_lock);

	t->t_name       = name != NULL ? name : "(anon)";
	t->t_threads    = NULL;
	t->t_nthreads   = 0;
	t->t_refs       = 1;
	t->t_self_port  = NULL;
	t->t_map        = NULL;
	t->t_pmap       = NULL;
	{
		unsigned	exi;

		for (exi = 0; exi < EXC_TYPE_COUNT; exi++)
			t->t_exc_ports[exi] = NULL;
	}
	t->t_exc_flags   = 0;
	t->t_killed      = false;
	t->t_personality = TASK_PERSONALITY_STYLE9;
	t->t_darwin_dylib_next = 0;
	t->t_darwin_ppid = 0;
	/*
	 * A task starts at the root; an empty cwd would make every resolved
	 * path lose its leading slash.
	 */
	t->t_darwin_cwd[0] = '/';
	t->t_darwin_cwd[1] = '\0';
	/* The traditional umask. */
	t->t_darwin_umask = 022;
	{
		size_t	fi;

		for (fi = 0; fi < DARWIN_NOFILE; fi++) {
			t->t_darwin_files[fi].of_pipe = NULL;
			t->t_darwin_files[fi].of_buf  = NULL;
			t->t_darwin_files[fi].of_size = 0;
			t->t_darwin_files[fi].of_off  = 0;
			t->t_darwin_files[fi].of_type = DARWIN_OF_FREE;
		}
	}
	{
		size_t	si;

		t->t_sig_pending = 0;
		t->t_sig_mask    = 0;
		t->t_sig_tramp   = 0;
		t->t_sig_mask_saved   = 0;
		t->t_sig_mask_restore = false;
		for (si = 0; si < DARWIN_NSIG; si++)
			t->t_sig_handler[si] = DARWIN_SIG_DFL;
	}

	t->t_port_space = port_space_new();
	if (t->t_port_space == NULL) {
		kfree(t);
		return (NULL);
	}

	/*
	 * The VM map records the user-VA ranges this task has staked out;
	 * the pmap is the page-table tree they land in.  Whoever
	 * vm_map_enters a range also pmap_enters its frames in t_pmap.
	 */
	t->t_map = vm_map_create(VM_USER_VA_LO, VM_USER_VA_HI);
	if (t->t_map == NULL) {
		port_space_destroy(t->t_port_space);
		kfree(t);
		return (NULL);
	}

	t->t_pmap = pmap_create();
	if (t->t_pmap == NULL) {
		vm_map_destroy(t->t_map);
		port_space_destroy(t->t_port_space);
		kfree(t);
		return (NULL);
	}

	if (port_install_task_self(t) != MACH_MSG_OK) {
		pmap_destroy(t->t_pmap);
		vm_map_destroy(t->t_map);
		port_space_destroy(t->t_port_space);
		kfree(t);
		return (NULL);
	}

	/*
	 * SEND right to the global bootstrap port at the second name
	 * (MACH_PORT_BOOTSTRAP).  Needs bootstrap_init to have run: kmain
	 * calls port_subsystem_init, bootstrap_init, task_subsystem_init in
	 * that order.
	 */
	if (port_install_bootstrap(t) != MACH_MSG_OK) {
		port_release_task_self(t);
		pmap_destroy(t->t_pmap);
		vm_map_destroy(t->t_map);
		port_space_destroy(t->t_port_space);
		kfree(t);
		return (NULL);
	}

	/* Into the global task table (task_list, below). */
	spin_lock(&tasks_lock);
	{
		extern void task__chain_insert(struct task *);
		task__chain_insert(t);
	}
	spin_unlock(&tasks_lock);

	return (t);
}

/*
 * All live tasks: a static array rather than a link in struct task.
 * TASK_LIST_MAX is plenty, and task__chain_insert panics when it is not.
 */
#define	TASK_LIST_MAX	64
static struct task	*task_list[TASK_LIST_MAX];	/* (g) */
static size_t		 task_list_count;		/* (g) */

void
task__chain_insert(struct task *t)
{
	size_t	i;

	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] == NULL) {
			task_list[i] = t;
			task_list_count++;
			return;
		}
	}
	panic("task__chain_insert: task_list full");
}

static void
task__chain_remove(struct task *t)
{
	size_t	i;

	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] == t) {
			task_list[i] = NULL;
			task_list_count--;
			return;
		}
	}
}

void
task_ref(struct task *t)
{

	spin_lock(&t->t_lock);
	KASSERT(t->t_refs > 0, "task_ref: zero refs");
	t->t_refs++;
	spin_unlock(&t->t_lock);
}

void
task_deref(struct task *t)
{
	bool	dead;

	spin_lock(&t->t_lock);
	KASSERT(t->t_refs > 0, "task_deref: underflow");
	t->t_refs--;
	dead = (t->t_refs == 0);
	spin_unlock(&t->t_lock);

	if (!dead)
		return;

	KASSERT(t->t_nthreads == 0, "task_deref: threads still attached");

	spin_lock(&tasks_lock);
	task__chain_remove(t);
	spin_unlock(&tasks_lock);

	/*
	 * Off the live list, so a parent counting children now counts one
	 * fewer: tell a parked wait4.  Here because every route out of a
	 * task passes through this point, unlike the several places a Darwin
	 * exit records a status.
	 */
	darwin_child_news(t->t_darwin_ppid);

	/*
	 * Drop the kernel-held SEND refs on the exception ports, before
	 * port_space_destroy so they fall outside its bulk walk; the ports
	 * live in some other task's space (typically a parent's).
	 */
	{
		unsigned	exi;

		for (exi = 0; exi < EXC_TYPE_COUNT; exi++) {
			if (t->t_exc_ports[exi] != NULL) {
				port_deref(t->t_exc_ports[exi],
				    MACH_PORT_RIGHT_SEND);
				t->t_exc_ports[exi] = NULL;
			}
		}
	}

	/*
	 * Release Darwin open files the task never closed (normally its last
	 * thread closed them in thread_exit; this is the fallback).  Dropping
	 * a pipe end is what turns the far reader's next read into EOF.
	 * kernel_task's slots are always empty.
	 */
	darwin_files_teardown(t);

	/*
	 * kernel_task shares kernel_space, which lives as long as the
	 * kernel, so it never gets here in practice.
	 */
	if (t != kernel_task) {
		/*
		 * Destroying the space drops every SEND in it, name 1's
		 * task-self SEND included; port_release_task_self then drops
		 * the last (RECEIVE) ref and the port is reclaimed.
		 */
		port_space_destroy(t->t_port_space);
		port_release_task_self(t);
		/*
		 * Free the anonymous frames the task's user VA pulled in
		 * (image pages, OOL receives, ...) before pmap_destroy, which
		 * frees only the page-table levels, not leaf frames.
		 */
		vm_map_release_anon(t->t_map, t->t_pmap);
		pmap_destroy(t->t_pmap);
		vm_map_destroy(t->t_map);
	}
	kfree(t);
}

void
task_attach_thread(struct task *t, struct thread *th)
{

	spin_lock(&t->t_lock);
	th->th_task_link = t->t_threads;
	t->t_threads = th;
	t->t_nthreads++;
	spin_unlock(&t->t_lock);

	task_ref(t);
}

void
task_detach_thread(struct task *t, struct thread *th)
{
	struct thread	*cur, *prev;

	spin_lock(&t->t_lock);
	prev = NULL;
	for (cur = t->t_threads; cur != NULL; cur = cur->th_task_link) {
		if (cur == th) {
			if (prev == NULL)
				t->t_threads = cur->th_task_link;
			else
				prev->th_task_link = cur->th_task_link;
			t->t_nthreads--;
			cur->th_task_link = NULL;
			break;
		}
		prev = cur;
	}
	spin_unlock(&t->t_lock);

	task_deref(t);
}

void
task_print(struct task *t)
{
	struct thread	*cur;

	spin_lock(&t->t_lock);
	kprintf("task id=%llu name=%s threads=%u refs=%u\n",
	    (unsigned long long)t->t_id, t->t_name,
	    t->t_nthreads, t->t_refs);
	/*
	 * For a blocked thread, say what it waits on: in a stall, that is
	 * the one fact that matters.
	 */
	for (cur = t->t_threads; cur != NULL; cur = cur->th_task_link) {
		if (cur->th_state != THREAD_BLOCKED) {
			kprintf("  thread id=%llu name=%s state=%s\n",
			    (unsigned long long)cur->th_id, cur->th_name,
			    thread_state_name(cur->th_state));
			continue;
		}
		kprintf("  thread id=%llu name=%s blocked on %s %p, "
		    "deadline %llu%s\n",
		    (unsigned long long)cur->th_id, cur->th_name,
		    thread_block_reason_name(cur->th_block_reason),
		    cur->th_block_target,
		    (unsigned long long)cur->th_wake_deadline_ms,
		    cur->th_wake_pending != 0 ? ", wake owed" : "");
	}
	spin_unlock(&t->t_lock);
}

int
task_set_exception_ports(struct task *t, uint32_t types_mask, struct port *port)
{
	struct port	*prev[EXC_TYPE_COUNT];
	unsigned	 i;

	if (t == NULL)
		return (MACH_E_INVAL);
	if ((types_mask & ~EXC_MASK_ALL) != 0)
		return (MACH_E_INVAL);
	if (types_mask == 0)
		return (MACH_MSG_OK);

	/*
	 * Take popcount(types_mask) refs on the new port before the swap,
	 * so a user_fault_die that snapshots a slot between the unlock and
	 * the drop of the previous occupant reads a live ref.
	 */
	if (port != NULL) {
		for (i = 0; i < EXC_TYPE_COUNT; i++) {
			if (types_mask & (1u << i))
				port_ref(port, MACH_PORT_RIGHT_SEND);
		}
	}

	for (i = 0; i < EXC_TYPE_COUNT; i++)
		prev[i] = NULL;

	spin_lock(&t->t_lock);
	for (i = 0; i < EXC_TYPE_COUNT; i++) {
		if ((types_mask & (1u << i)) == 0)
			continue;
		prev[i] = t->t_exc_ports[i];
		t->t_exc_ports[i] = port;
	}
	spin_unlock(&t->t_lock);

	for (i = 0; i < EXC_TYPE_COUNT; i++) {
		if (prev[i] != NULL)
			port_deref(prev[i], MACH_PORT_RIGHT_SEND);
	}
	return (MACH_MSG_OK);
}

/*
 * Reply for the task-self dispatcher: `body' back to req->msgh_local as a
 * bare [header | body] message, COPY_SEND on the caller's space.  As
 * host.c's host_reply_inline; these ops hand out no rights, so no
 * descriptors.
 */
static int
task_reply_inline(const struct mach_msg_header *req, struct port_space *from,
    const void *body, size_t body_size)
{
	uint8_t			 buf[sizeof(struct mach_msg_header) + 64];
	struct mach_msg_header	*rhdr;
	const uint8_t		*src;
	uint8_t			*dst;
	size_t			 total;
	size_t			 i;

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

/*
 * Map the SYS_E_* a syscall_vm_* helper returns onto the in-band MACH_E_*
 * status the task-port VM replies carry.  Only the two failure modes those
 * helpers actually produce are distinguished; anything else is INVAL.
 */
static int32_t
task_vm_status(long rv)
{

	if (rv == 0)
		return (MACH_MSG_OK);
	if (rv == SYS_E_NOMEM)
		return (MACH_E_NOMEM);
	return (MACH_E_INVAL);
}

/*
 * Reply for TASK_OP_GET_SPECIAL_PORT: a SEND right to the port named by
 * the persistent kernel_space name `kname', as a COMPLEX [header | body |
 * port_descriptor] message.  As mach/bootstrap.c's lookup reply: install
 * the caller's reply port in kernel_space, then send from kernel_space
 * with a MOVE_SEND header (consuming that name) and a COPY_SEND
 * descriptor (kname persists; the caller releases its copy).
 */
static int
task_reply_port(const struct mach_msg_header *req, struct port_space *from,
    mach_port_name_t kname)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_port_descriptor	pd;
	} reply;
	struct port		*reply_port;
	mach_port_name_t	 kreply;
	int			 rv;
	uint8_t			 dummy;

	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	reply_port = space_lookup(from, req->msgh_local,
	    MACH_PORT_RIGHT_SEND, &dummy);
	if (reply_port == NULL)
		return (MACH_E_RIGHT);
	rv = space_install(kernel_space, reply_port, MACH_PORT_RIGHT_SEND,
	    &kreply);
	if (rv != MACH_MSG_OK)
		return (rv);

	reply.hdr.msgh_bits    =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	reply.hdr.msgh_size    = sizeof(reply);
	reply.hdr.msgh_remote  = kreply;
	reply.hdr.msgh_local   = MACH_PORT_NULL;
	reply.hdr.msgh_voucher = 0;
	reply.hdr.msgh_id      = req->msgh_id;

	reply.body.msgh_descriptor_count = 1;

	reply.pd.name        = kname;
	reply.pd.pad1        = 0;
	reply.pd.disposition = MACH_MSG_TYPE_COPY_SEND;
	reply.pd.type        = MACH_MSG_PORT_DESCRIPTOR;
	reply.pd.pad2        = 0;

	rv = mach_msg_send(kernel_space, &reply.hdr);
	if (rv != MACH_MSG_OK)
		(void)space_drop_one_right(kernel_space, kreply,
		    MACH_PORT_RIGHT_SEND);
	return (rv);
}

/*
 * Synchronous dispatcher for messages to a task's task_self port, called
 * from mach_msg_send for a destination tagged PORT_SPECIAL_TASK_SELF.  It
 * runs in the sender's context: no server thread, no request queue.
 *
 * Ops (msgh_id; see TASK_OP_* in port.h):
 *	GET_INFO	task id, live thread count, name snapshot.
 *	VM_ALLOCATE	allocate an anonymous range in `target' and return
 *			its VA, via syscall_vm_allocate (the SYS_VM_ALLOCATE
 *			path).
 *	VM_DEALLOCATE	release a range allocated above.
 *	GET_SPECIAL_PORT a SEND right to the host or bootstrap port.
 *
 * Inline replies use COPY_SEND on msgh_local: mach_msg_rpc allocates
 * that reply port with RECEIVE+SEND in the caller's space.
 */
int
task_self_dispatch(struct task *target, const struct mach_msg_header *req,
    struct port_space *from)
{
	size_t	hdr;

	if (target == NULL || req == NULL || from == NULL)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	hdr = sizeof(struct mach_msg_header);

	switch (req->msgh_id) {
	case TASK_OP_GET_INFO: {
		struct task_info_reply	r;
		size_t			i;

		spin_lock(&target->t_lock);
		r.tir_task_id  = target->t_id;
		r.tir_nthreads = target->t_nthreads;
		spin_unlock(&target->t_lock);
		r.tir_pad = 0;
		for (i = 0; i < sizeof(r.tir_name); i++)
			r.tir_name[i] = 0;
		if (target->t_name != NULL) {
			for (i = 0; i < sizeof(r.tir_name) - 1 &&
			    target->t_name[i] != '\0'; i++)
				r.tir_name[i] = target->t_name[i];
		}
		return (task_reply_inline(req, from, &r, sizeof(r)));
	}

	case TASK_OP_VM_ALLOCATE: {
		struct task_vm_allocate_reply		 r;
		const struct task_vm_allocate_request	*rq;
		long					 rv;
		uint64_t				 va;

		if (req->msgh_size < hdr + sizeof(*rq))
			return (MACH_E_INVAL);
		rq = (const struct task_vm_allocate_request *)
		    ((const uint8_t *)req + hdr);

		va = 0;
		rv = syscall_vm_allocate(target, rq->tva_size,
		    rq->tva_prot, &va);
		r.tvar_address = (rv == 0) ? va : 0;
		r.tvar_status  = task_vm_status(rv);
		r.tvar_pad     = 0;
		return (task_reply_inline(req, from, &r, sizeof(r)));
	}

	case TASK_OP_VM_DEALLOCATE: {
		struct task_vm_deallocate_reply		 r;
		const struct task_vm_deallocate_request	*rq;
		long					 rv;

		if (req->msgh_size < hdr + sizeof(*rq))
			return (MACH_E_INVAL);
		rq = (const struct task_vm_deallocate_request *)
		    ((const uint8_t *)req + hdr);

		rv = syscall_vm_deallocate(target, rq->tvd_address,
		    rq->tvd_size);
		r.tvdr_status = task_vm_status(rv);
		r.tvdr_pad    = 0;
		return (task_reply_inline(req, from, &r, sizeof(r)));
	}

	case TASK_OP_GET_SPECIAL_PORT: {
		const struct task_special_port_request	*rq;
		mach_port_name_t			 kname;

		if (req->msgh_size < hdr + sizeof(*rq))
			return (MACH_E_INVAL);
		rq = (const struct task_special_port_request *)
		    ((const uint8_t *)req + hdr);

		switch (rq->tsp_which) {
		case TASK_SPECIAL_HOST:
			kname = host_get_kernel_name();
			break;
		case TASK_SPECIAL_BOOTSTRAP:
			kname = bootstrap_get_kernel_name();
			break;
		default:
			kname = MACH_PORT_NULL;
			break;
		}
		if (kname == MACH_PORT_NULL)
			return (task_reply_inline(req, from, NULL, 0));
		return (task_reply_port(req, from, kname));
	}

	default:
		return (MACH_E_INVAL);
	}
}

void
task_list_print(void)
{
	size_t	i;

	spin_lock(&tasks_lock);
	kprintf("%zu tasks live:\n", task_list_count);
	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] != NULL) {
			struct task *t = task_list[i];
			spin_unlock(&tasks_lock);
			task_print(t);
			spin_lock(&tasks_lock);
		}
	}
	spin_unlock(&tasks_lock);
}

size_t
task_snapshot(struct task **out, size_t max)
{
	size_t	i, n;

	if (out == NULL || max == 0)
		return (0);

	n = 0;
	spin_lock(&tasks_lock);
	for (i = 0; i < TASK_LIST_MAX && n < max; i++) {
		if (task_list[i] != NULL)
			out[n++] = task_list[i];
	}
	spin_unlock(&tasks_lock);
	return (n);
}

/*
 * task_is_alive: one scan of TASK_LIST_MAX slots under tasks_lock, no
 * ref taken -- a hint, stale once the lock drops (see task.h).
 */
bool
task_is_alive(uint64_t id)
{
	bool	alive;
	size_t	i;

	alive = false;
	spin_lock(&tasks_lock);
	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] != NULL && task_list[i]->t_id == id) {
			alive = true;
			break;
		}
	}
	spin_unlock(&tasks_lock);
	return (alive);
}

/*
 * Count live tasks whose t_darwin_ppid is `ppid`, restricted to t_id ==
 * pid when pid != 0.  Same best-effort discipline as task_is_alive --
 * see task.h for how wait4's loop keeps the staleness harmless.
 */
int
task_count_darwin_children(uint64_t ppid, uint64_t pid)
{
	size_t	i;
	int	n;

	n = 0;
	spin_lock(&tasks_lock);
	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] == NULL)
			continue;
		if (task_list[i]->t_darwin_ppid != ppid)
			continue;
		if (pid != 0 && task_list[i]->t_id != pid)
			continue;
		n++;
	}
	spin_unlock(&tasks_lock);
	return (n);
}

/*
 * task_self_port_for: see task.h.  Backs launchd's LOAD reply, which
 * hands launchctl the new child's task-self SEND so it can arm a
 * DEAD_NAME notification.
 *
 * Lock order: tasks_lock -> p_lock (in port_ref); p_lock is a leaf.  The
 * ref is taken under tasks_lock so the task cannot be removed and freed
 * between the match and the ref.
 */
struct port *
task_self_port_for(uint64_t task_id)
{
	struct port	*p;
	size_t		 i;

	p = NULL;
	spin_lock(&tasks_lock);
	for (i = 0; i < TASK_LIST_MAX; i++) {
		struct task	*t = task_list[i];

		if (t != NULL && t->t_id == task_id) {
			if (t != kernel_task && t->t_self_port != NULL) {
				p = t->t_self_port;
				port_ref(p, MACH_PORT_RIGHT_SEND);
			}
			break;
		}
	}
	spin_unlock(&tasks_lock);
	return (p);
}

/*
 * task_lookup_ref: task_self_port_for's scan, taking the task's own ref
 * under tasks_lock.  NULL for an unknown id -- e.g. a task-self port
 * whose task is gone.  kernel_task is returned like any other; callers
 * that must exclude it check the id.
 *
 * Lock order: tasks_lock -> t_lock (in task_ref), the edge
 * task_request_terminate already takes.
 */
struct task *
task_lookup_ref(uint64_t id)
{
	struct task	*t;
	size_t		 i;

	t = NULL;
	spin_lock(&tasks_lock);
	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] != NULL && task_list[i]->t_id == id) {
			t = task_list[i];
			task_ref(t);
			break;
		}
	}
	spin_unlock(&tasks_lock);
	return (t);
}

/*
 * task_request_terminate: async kill of a live task, in three steps.
 *
 *	1. Find the target under tasks_lock and take a ref so it cannot
 *	   be torn down under the wake walk.  kernel_task, which hosts
 *	   every kernel thread (idle, drivers, services), is refused.
 *
 *	2. Store t_killed with RELEASE, pairing with task_kill_pending's
 *	   ACQUIRE, still under tasks_lock so no other tasks_lock holder
 *	   sees the target before the flag.
 *
 *	3. thread_wake every thread in t_threads, under t_lock.  A
 *	   BLOCKED thread wakes; a READY or RUNNING one gets
 *	   th_wake_pending.  A thread mid-park, holding sched_lock but not
 *	   yet BLOCKED, sees t_killed in thread_block_release's pre-park
 *	   check and retires without a wake.
 *
 * Lock order: t_lock -> sched_lock -> th_lock, via the wake fan-out.
 * Nothing takes t_lock while holding sched_lock or th_lock.
 */
void
task_request_terminate(uint64_t task_id)
{
	struct task	*t;
	struct thread	*th;
	size_t		 i;

	t = NULL;
	spin_lock(&tasks_lock);
	for (i = 0; i < TASK_LIST_MAX; i++) {
		if (task_list[i] != NULL && task_list[i]->t_id == task_id) {
			t = task_list[i];
			break;
		}
	}
	if (t == NULL || t == kernel_task) {
		spin_unlock(&tasks_lock);
		return;
	}
	__atomic_store_n(&t->t_killed, true, __ATOMIC_RELEASE);
	task_ref(t);
	spin_unlock(&tasks_lock);

	spin_lock(&t->t_lock);
	for (th = t->t_threads; th != NULL; th = th->th_task_link)
		thread_wake(th);
	spin_unlock(&t->t_lock);

	task_deref(t);
}

bool
task_kill_pending(struct task *t)
{

	if (t == NULL)
		return (false);
	return (__atomic_load_n(&t->t_killed, __ATOMIC_ACQUIRE));
}
