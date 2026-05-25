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
	 * kernel_task uses the pre-existing kernel_space rather than a
	 * fresh one -- otherwise the ports already allocated during
	 * port_subsystem_init would belong to nobody.
	 */
	port_space_destroy(kernel_task->t_port_space);
	kernel_task->t_port_space = kernel_space;

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

	t->t_name      = name != NULL ? name : "(anon)";
	t->t_threads   = NULL;
	t->t_nthreads  = 0;
	t->t_refs      = 1;

	t->t_port_space = port_space_new();
	if (t->t_port_space == NULL) {
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

	if (t->t_port_space != NULL && t != kernel_task)
		port_space_destroy(t->t_port_space);
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
