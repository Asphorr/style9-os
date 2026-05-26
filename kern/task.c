/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

/*
 * Lock-key:
 *	(c) const after task_create
 *	(t) protected by the task's own t_lock
 *	(g) protected by tasks_lock (global)
 */

struct task		*kernel_task;

static struct spinlock	tasks_lock = SPINLOCK_INIT("tasks");
static uint64_t		next_task_id;		/* (g) */
static struct task	*tasks_head;		/* (g) all live tasks      */

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
	 * kernel_task is the unique exception that uses the pre-existing
	 * kernel_space rather than the fresh one task_create made.  Two
	 * fix-ups follow:
	 *	1. Release the install task_create did into the throwaway
	 *	   space (port_release_task_self drops the kernel-side
	 *	   RECEIVE ref) so the port can be reaped along with that
	 *	   space when we destroy it.
	 *	2. Repoint t_port_space at kernel_space and re-run the
	 *	   install there.  Because kernel_space is currently empty
	 *	   (no allocations happen between port_subsystem_init and
	 *	   here), the SEND right lands at name 1 -- the well-known
	 *	   MACH_PORT_TASK_SELF slot.
	 */
	port_release_task_self(kernel_task);
	port_space_destroy(kernel_task->t_port_space);
	kernel_task->t_port_space = kernel_space;
	if (port_install_task_self(kernel_task) != MACH_MSG_OK)
		panic("task_subsystem_init: install task_self in kernel_space");
	if (port_install_bootstrap(kernel_task) != MACH_MSG_OK)
		panic("task_subsystem_init: install bootstrap in kernel_space");

	kprintf("task: kernel_task id=%llu name=%s, %u initial tasks\n",
	    (unsigned long long)kernel_task->t_id,
	    kernel_task->t_name, 1);
}

struct task *
task_create(const char *name)
{
	struct task	*t;

	t = (struct task *)kmalloc(sizeof(*t));
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

	t->t_port_space = port_space_new();
	if (t->t_port_space == NULL) {
		kfree(t);
		return (NULL);
	}

	if (port_install_task_self(t) != MACH_MSG_OK) {
		port_space_destroy(t->t_port_space);
		kfree(t);
		return (NULL);
	}

	/*
	 * Install the SEND right to the global bootstrap port at the
	 * second slot (MACH_PORT_BOOTSTRAP).  bootstrap_init must have
	 * run before any task_create call -- kmain wires this in the
	 * order port_subsystem_init -> bootstrap_init -> task_subsystem_init.
	 */
	if (port_install_bootstrap(t) != MACH_MSG_OK) {
		port_release_task_self(t);
		port_space_destroy(t->t_port_space);
		kfree(t);
		return (NULL);
	}

	/* Link into global task list. */
	spin_lock(&tasks_lock);
	/*
	 * Reuse t_lock's _next_ slot via th_link in the future; for
	 * now we keep the global list head pointer in tasks_head and
	 * thread struct's intrusive pointer below.
	 *
	 * Single-linked list: insert at head.
	 */
	{
		/* Type-pun next via t_refs's high half is ugly; use a real field. */
		extern void task__chain_insert(struct task *);
		task__chain_insert(t);
	}
	spin_unlock(&tasks_lock);

	return (t);
}

/*
 * Intrusive list-of-all-tasks: stored in a small parallel structure
 * since struct task should stay focused on its own fields.  A simple
 * static array is enough -- there will not be hundreds of tasks any
 * time soon, and it sidesteps cyclic includes between task.h and
 * thread.h.
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
	 * Order: drop the per-space SEND on task_self BEFORE releasing
	 * the kernel-side RECEIVE.  port_space_destroy walks every entry
	 * (incl. our well-known name 1) and port_dereps each one, so by
	 * the time the call returns there are no remaining SEND refs;
	 * port_release_task_self then drops the last RECEIVE and the port
	 * reaches zero refs and is reclaimed inside that deref.
	 *
	 * kernel_task is the bootstrap exception -- it shares
	 * kernel_space, which lives for the duration of the kernel.
	 * Its task_self port stays alive too; never get here in practice.
	 */
	if (t != kernel_task) {
		port_space_destroy(t->t_port_space);
		port_release_task_self(t);
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
	for (cur = t->t_threads; cur != NULL; cur = cur->th_task_link)
		kprintf("  thread id=%llu name=%s state=%s\n",
		    (unsigned long long)cur->th_id,
		    cur->th_name,
		    thread_state_name(cur->th_state));
	spin_unlock(&t->t_lock);
}

/*
 * Synchronous dispatcher for messages addressed to a task's task_self
 * port.  Invoked from inside mach_msg_send (port.c) for every send
 * whose destination's p_special tag is PORT_SPECIAL_TASK_SELF, so the
 * reply path executes in the caller's context -- there is no server
 * thread, no queueing on the request side, just a direct lookup + an
 * inline send of the reply via the caller's reply port (msgh_local).
 *
 * The reply uses COPY_SEND on msgh_local because mach_msg_rpc
 * allocates that reply port with RECEIVE+SEND in the caller's space;
 * the SEND right we lean on here was already there before the request
 * crossed into the kernel.
 */
int
task_self_dispatch(struct task *target, const struct mach_msg_header *req,
    struct port_space *from)
{
	uint8_t		bytes[sizeof(struct mach_msg_header) +
			    sizeof(struct task_info_reply)];
	struct mach_msg_header	*rhdr;
	struct task_info_reply	*body;
	size_t			 i;

	if (target == NULL || req == NULL || from == NULL)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	rhdr = (struct mach_msg_header *)bytes;
	body = (struct task_info_reply *)(bytes +
	    sizeof(struct mach_msg_header));

	switch (req->msgh_id) {
	case TASK_OP_GET_INFO:
		spin_lock(&target->t_lock);
		body->tir_task_id  = target->t_id;
		body->tir_nthreads = target->t_nthreads;
		spin_unlock(&target->t_lock);
		body->tir_pad = 0;
		for (i = 0; i < sizeof(body->tir_name); i++)
			body->tir_name[i] = 0;
		if (target->t_name != NULL) {
			for (i = 0; i < sizeof(body->tir_name) - 1 &&
			    target->t_name[i] != '\0'; i++)
				body->tir_name[i] = target->t_name[i];
		}

		rhdr->msgh_bits    =
		    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		rhdr->msgh_size    = sizeof(bytes);
		rhdr->msgh_remote  = req->msgh_local;
		rhdr->msgh_local   = MACH_PORT_NULL;
		rhdr->msgh_voucher = 0;
		rhdr->msgh_id      = req->msgh_id;
		return (mach_msg_send(from, rhdr));

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
