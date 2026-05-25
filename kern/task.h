/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_TASK_H_
#define	_SYS_TASK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spinlock.h"

/*
 * Mach-style task: the resource container.
 *
 * A task owns an address space (later -- everything shares the kernel
 * address space for now), a port name space, and the set of threads
 * scheduled within it.  A thread always belongs to exactly one task,
 * but a task may have zero, one, or many threads.
 *
 * The split between task and thread is deliberate: unlike UNIX where
 * fork() conflates resource container and execution unit, here you
 * can talk about "spawn a worker thread inside this task" without
 * any chicken-and-egg.  kthreads, userland threads, and threads-of-
 * another-task all use the same struct thread; the task pointer is
 * what distinguishes them.
 */

struct port_space;
struct thread;

struct task {
	struct spinlock		 t_lock;
	uint64_t		 t_id;		/* (c) printable id        */
	const char		*t_name;	/* (c) for ps-style listing */
	struct port_space	*t_port_space;	/* (c) name table          */
	struct thread		*t_threads;	/* (t) head of thread list */
	uint32_t		 t_nthreads;	/* (t) count                */
	uint32_t		 t_refs;	/* (t) lifetime refs        */
};

extern struct task		*kernel_task;

void			 task_subsystem_init(void);
struct task		*task_create(const char *name);
void			 task_ref(struct task *);
void			 task_deref(struct task *);

/*
 * Linked-list helpers used by thread.c on the t_threads list, which
 * is threaded through struct thread.  Defined here so both files
 * agree on the lock acquisition order (t_lock then th_lock).
 */
void			 task_attach_thread(struct task *, struct thread *);
void			 task_detach_thread(struct task *, struct thread *);

void			 task_print(struct task *);
void			 task_list_print(void);

#endif /* !_SYS_TASK_H_ */
