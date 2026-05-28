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
#include "pmap.h"
#include "port.h"
#include "port_internal.h"
#include "sched.h"
#include "spinlock.h"
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

	/*
	 * The kernel runs on the page-table tree boot.S installed and
	 * pmap_bootstrap wrapped as kernel_pmap.  task_create allocated a
	 * fresh per-task pmap for kernel_task; drop it and point at the
	 * authoritative kernel pmap instead so context switches between
	 * kernel threads never reload CR3.
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
	t->t_map        = NULL;
	t->t_pmap       = NULL;
	{
		unsigned	exi;

		for (exi = 0; exi < EXC_TYPE_COUNT; exi++)
			t->t_exc_ports[exi] = NULL;
	}
	t->t_exc_flags = 0;
	t->t_killed    = false;

	t->t_port_space = port_space_new();
	if (t->t_port_space == NULL) {
		kfree(t);
		return (NULL);
	}

	/*
	 * Per-task VM map records the user-VA ranges this task has staked
	 * out; per-task pmap is the hardware page-table tree those entries
	 * land in.  The map is paired with the pmap from creation forward
	 * -- callers that vm_map_enter a range are also expected to
	 * pmap_enter the underlying frame in the same task->t_pmap.
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
	 * Install the SEND right to the global bootstrap port at the
	 * second slot (MACH_PORT_BOOTSTRAP).  bootstrap_init must have
	 * run before any task_create call -- kmain wires this in the
	 * order port_subsystem_init -> bootstrap_init -> task_subsystem_init.
	 */
	if (port_install_bootstrap(t) != MACH_MSG_OK) {
		port_release_task_self(t);
		pmap_destroy(t->t_pmap);
		vm_map_destroy(t->t_map);
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
	/*
	 * Release the kernel-held SEND ref on the task's exception port,
	 * if any.  Done BEFORE port_space_destroy so the deref happens
	 * outside the space's bulk-deref walk; the exc_port lives in some
	 * OTHER task's space (typically a parent's), so it's untouched
	 * by our own space destruction.
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

	if (t != kernel_task) {
		port_space_destroy(t->t_port_space);
		port_release_task_self(t);
		/*
		 * Reclaim the anonymous backing frames the task's user VA
		 * ever pulled in (image load pages, OOL recv installs, ...)
		 * before pmap_destroy frees the page-table tree itself.
		 * pmap_destroy only walks intermediate levels; without the
		 * vm_map_release_anon pass each task drag along its
		 * lifetime's worth of leaf frames forever.
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
	for (cur = t->t_threads; cur != NULL; cur = cur->th_task_link)
		kprintf("  thread id=%llu name=%s state=%s\n",
		    (unsigned long long)cur->th_id,
		    cur->th_name,
		    thread_state_name(cur->th_state));
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
	 * Take popcount(types_mask) refs on the new port BEFORE the
	 * swap so any interleaving user_fault_die that snapshots one
	 * of the slots between unlock and the prev-drop reads a live
	 * ref.  Ordering: take-new-refs -> swap -> drop-prev mirrors
	 * SYS_TASK_SET_EXC_PORT's A v1 pattern for the same reason.
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

/*
 * task_is_alive: best-effort, lock-only liveness check.  No ref bump --
 * the answer is stale once tasks_lock is dropped, so the shell must
 * yield-spin (and re-probe) until it stays 'false'.  Cheap enough for
 * that purpose: a single scan of TASK_LIST_MAX (64) slots under one
 * spinlock acquisition.
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
 * task_self_port_for: take one SEND ref on `task_id`'s task-self port
 * and hand the port object back, or NULL if the id is unknown / names
 * kernel_task / has no self port.  The returned ref is the caller's to
 * drop with port_deref(.., MACH_PORT_RIGHT_SEND).
 *
 * Powers the launchd LOAD reply: launchd hands the freshly spawned
 * child's task-self SEND right to launchctl as an implicit-local port
 * descriptor so launchctl can arm a DEAD_NAME notification on it --
 * death-by-notification instead of the v1 task_alive yield-poll.
 *
 * Lock-order: tasks_lock -> p_lock (inside port_ref).  p_lock is a
 * leaf and nothing takes tasks_lock while holding it, so no cycle.
 * Taking the ref under tasks_lock guarantees the task cannot be
 * chain-removed + freed between the match and the ref bump.
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
 * task_request_terminate: async-kill on a live target.  Three phases:
 *
 *	1. Identify the target under tasks_lock + bump a ref so it
 *	   cannot teardown out from under the wake walk.  Refuse the
 *	   request if it names kernel_task; the kernel task hosts every
 *	   kernel-side thread (idle, drivers, services) and is by
 *	   construction not killable.
 *
 *	2. Atomic-store t_killed = true with RELEASE so the matching
 *	   ACQUIRE in task_kill_pending (called from the detection sites
 *	   in syscall_dispatch + thread_block_release) reads the freshly
 *	   set flag.  Done while still holding tasks_lock so concurrent
 *	   tasks_lock holders cannot observe the target before the flag
 *	   is set.
 *
 *	3. Walk t_threads under t_lock and thread_wake each entry.
 *	   thread_wake is a no-op for non-BLOCKED threads, so READY and
 *	   RUNNING members are correctly ignored.  Threads that were
 *	   already mid-park (acquired sched_lock but not yet committed
 *	   to BLOCKED) observe t_killed under sched_lock in
 *	   thread_block_release's pre-park check and retire without
 *	   needing a wake -- this covers the race where the kill arrives
 *	   between the would-be-parker reading "no message available"
 *	   and committing to the BLOCKED state.
 *
 * Lock-order introduced: t_lock -> sched_lock -> th_lock (via the
 * thread_wake fan-out under t_lock).  Nothing in the existing code
 * acquires t_lock while holding sched_lock or th_lock, so the new
 * edge does not close a cycle.
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
