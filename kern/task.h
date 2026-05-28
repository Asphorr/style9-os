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

#include "port.h"
#include "spinlock.h"

/*
 * Mach-style task: the resource container.
 *
 * A task owns a per-task address space (t_pmap + t_map), a port name
 * space, and the set of threads scheduled within it.  A thread always
 * belongs to exactly one task, but a task may have zero, one, or many
 * threads.
 *
 * The split between task and thread is deliberate: unlike UNIX where
 * fork() conflates resource container and execution unit, here you
 * can talk about "spawn a worker thread inside this task" without
 * any chicken-and-egg.  kthreads, userland threads, and threads-of-
 * another-task all use the same struct thread; the task pointer is
 * what distinguishes them.
 */

struct mach_msg_header;
struct pmap;
struct port;
struct port_space;
struct thread;
struct vm_map;

struct task {
	struct spinlock		 t_lock;
	uint64_t		 t_id;		/* (c) printable id        */
	const char		*t_name;	/* (c) for ps-style listing */
	struct port_space	*t_port_space;	/* (c) name table          */
	struct port		*t_self_port;	/* (c) kernel-RECEIVE port  */
	struct vm_map		*t_map;		/* (c) per-task vm map      */
	struct pmap		*t_pmap;	/* (c) per-task page-table  */
	struct thread		*t_threads;	/* (t) head of thread list */
	uint32_t		 t_nthreads;	/* (t) count                */
	uint32_t		 t_refs;	/* (t) lifetime refs        */
	/*
	 * Per-type task-level exception ports.  user_fault_die maps the
	 * x86 trap vector down to an EXC_TYPE_* index and posts
	 * MACH_EXC_FAULT onto t_exc_ports[type] (NULL slots silently
	 * drop).  Each slot is independent: the kernel holds one SEND
	 * ref per non-NULL slot; refs balance on slot replace, on
	 * mask-clear, and on task destruction.  Set via
	 * SYS_TASK_SET_EXC_PORTS (mask form) or SYS_TASK_SET_EXC_PORT
	 * (sets every slot from a single port, back-compat with A v1).
	 * All under t_lock.
	 */
	struct port		*t_exc_ports[EXC_TYPE_COUNT];

	/*
	 * Behavior flags for the exception dispatch path.  Bitwise OR
	 * of EXC_FLAG_* (today: only EXC_FLAG_RESUMABLE).  When
	 * RESUMABLE is set, user_fault_die uses the reply protocol --
	 * post the exception with an implicit reply port and park the
	 * thread until a mach_exception_reply lands (or the timeout
	 * expires, which falls back to KILL).  Clear by default; opted
	 * into by passing flag bits in the high half of the mask
	 * argument to SYS_TASK_SET_EXC_PORTS.  (t) under t_lock.
	 */
	uint32_t		 t_exc_flags;

	/*
	 * Async-termination flag.  Set once by task_request_terminate
	 * via __atomic_store(RELEASE); never cleared.  Five detection
	 * points, every place a thread can transition from "still
	 * running this task" to "about to commit to more work":
	 *	1. syscall_dispatch ENTRY -- next syscall after a ring-3
	 *	   compute window with t_killed already set.
	 *	2. thread_block_release pre-park -- caught between wake-
	 *	   eligibility (BLOCKED) and actually-blocked, otherwise
	 *	   we'd park and never observe the wake.
	 *	3. thread_block_release post-wake -- woken by the kill,
	 *	   retire before returning into the caller's recv/RPC loop.
	 *	4. intr_dispatch tail when iretq'ing to ring 3 -- catches
	 *	   pure compute loops that never syscall.  PIT timer (or
	 *	   any IRQ) brings the thread into the kernel; we retire
	 *	   instead of resuming user code.
	 *	5. syscall_dispatch EXIT -- catches "this syscall ITSELF
	 *	   caused the kill" (SYS_TASK_KILL on self), so the user
	 *	   never observes a sysretq on the kill-issuing call.
	 *
	 * kernel_task is never killable -- task_request_terminate
	 * refuses task_id matching kernel_task and every check site
	 * fast-paths past kernel-task threads before the atomic load.
	 *
	 * (a) atomic, set-once semantics; readers use ACQUIRE load.
	 */
	volatile bool		 t_killed;
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

/*
 * Snapshot live-task pointers into the caller's `out` array (max
 * entries).  No refs are bumped -- the snapshot is best-effort and
 * intended for short-lived, kernel-side use (e.g. the `tasks` service
 * dispatcher building its reply payload).  Returns the number of
 * pointers written.
 */
size_t			 task_snapshot(struct task **out, size_t max);

/*
 * Best-effort liveness probe by task id.  Returns true if a task with
 * that id is in the global task_list under tasks_lock at the moment of
 * the call.  No ref is bumped -- the result is stale the instant
 * tasks_lock is released, so callers must treat this as a hint, not a
 * handle.  Powers the SYS_TASK_ALIVE syscall: the shell yield-spins
 * until a spawned child drops off the live list.
 */
bool			 task_is_alive(uint64_t id);

/*
 * Request asynchronous termination of the task identified by `task_id`.
 * Sets t_killed via __atomic_store(RELEASE) and then best-effort wakes
 * every thread on the target's t_threads list.  Any thread already in
 * BLOCKED state observes the wake, returns through thread_block_release's
 * post-wake check, and retires; any thread about to park observes
 * t_killed under sched_lock in the pre-park check and retires
 * immediately; threads running in ring 3 observe t_killed at the top
 * of their next syscall_dispatch.
 *
 * Lock-order: tasks_lock -> t_lock -> sched_lock -> th_lock.  The
 * tasks_lock dance owns task identification + the t_killed store; the
 * t_lock walk + thread_wake fan-out introduces the new t_lock ->
 * sched_lock edge but no existing path takes sched_lock then t_lock,
 * so no cycle.
 *
 * Pure ring-3 compute loops that never syscall will NOT terminate
 * under v1 -- the IRQ-return-to-user check is deferred for v2.
 *
 * task_id == kernel_task->t_id is silently refused; missing ids are
 * silently no-ops (the caller probably races a natural exit).
 */
void			 task_request_terminate(uint64_t task_id);

/*
 * Non-blocking accessor for the t_killed flag.  ACQUIRE load.  NULL
 * task pointer returns false.  Used at the syscall-dispatch entry +
 * thread_block_release detection sites.
 */
bool			 task_kill_pending(struct task *t);

/*
 * task_self_port_for: take one SEND ref on `task_id`'s task-self port
 * and return the port object (caller drops the ref via port_deref with
 * MACH_PORT_RIGHT_SEND).  NULL if the id is unknown, names kernel_task,
 * or has no self port.  Lets launchd hand a freshly spawned child's
 * task-self SEND to launchctl for DEAD_NAME arming.
 */
struct port		*task_self_port_for(uint64_t task_id);

/*
 * Synchronous dispatcher invoked by mach_msg_send when the destination
 * port is tagged PORT_SPECIAL_TASK_SELF.  Reads `req->msgh_id` to pick
 * an op (see TASK_OP_* in port.h), assembles a reply message, and
 * sends it back to `req->msgh_local` using the COPY_SEND right the
 * caller's space holds on that name.  Returns MACH_MSG_OK on success.
 */
int			 task_self_dispatch(struct task *target,
			    const struct mach_msg_header *req,
			    struct port_space *from);

/*
 * Install `port` into every slot of `t->t_exc_ports` named by
 * `types_mask` (a bitwise OR of EXC_MASK_* values).  Existing
 * occupants of those slots get port_deref'd; the kernel takes one
 * fresh SEND ref per slot in `types_mask` on the new port (so a
 * single port covering multiple types holds N refs, balanced at
 * teardown).  Passing port=NULL clears the named slots.
 *
 * Returns MACH_MSG_OK or MACH_E_INVAL if `types_mask` has bits
 * outside EXC_MASK_ALL.  Empty mask is a no-op success.
 */
int			 task_set_exception_ports(struct task *t,
			    uint32_t types_mask, struct port *port);

#endif /* !_SYS_TASK_H_ */
