/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "mutex.h"
#include "kprintf.h"
#include "panic.h"
#include "sched.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

/* See mutex.h. */

/*
 * Counted so a test can tell whether it exercised the park and the wake,
 * not just uncontended acquisition (see mutex_blocks).
 */
static uint64_t	mutex_n_acquire;	/* (g) successful mutex_lock calls */
static uint64_t	mutex_n_slept;		/* (g) ...that had to block first  */

/*
 * Waiters are linked through th_wait_link, the field object queues own,
 * shared with the port waiter lists in mach/port_msg.c: a thread is on at
 * most one of them at a time.  Never th_runq_link (kern/thread.h says
 * why).
 */
static void
waiter_push(struct mutex *m, struct thread *th)
{

	th->th_wait_link = NULL;
	if (m->mtx_waiters_tail != NULL)
		m->mtx_waiters_tail->th_wait_link = th;
	else
		m->mtx_waiters_head = th;
	m->mtx_waiters_tail = th;
}

static struct thread *
waiter_pop(struct mutex *m)
{
	struct thread	*th;

	th = m->mtx_waiters_head;
	if (th == NULL)
		return (NULL);
	m->mtx_waiters_head = th->th_wait_link;
	if (m->mtx_waiters_head == NULL)
		m->mtx_waiters_tail = NULL;
	th->th_wait_link = NULL;
	return (th);
}

/*
 * Take `th' off the waiter list if it is still on it -- the unconditional
 * detach after a park, as port_unbind_waiter_locked does for ports.
 */
static void
waiter_unbind(struct mutex *m, struct thread *th)
{
	struct thread	**pp;
	struct thread	 *prev;

	prev = NULL;
	for (pp = &m->mtx_waiters_head; *pp != NULL;
	    pp = &(*pp)->th_wait_link) {
		if (*pp == th) {
			*pp = th->th_wait_link;
			if (m->mtx_waiters_tail == th)
				m->mtx_waiters_tail = prev;
			th->th_wait_link = NULL;
			break;
		}
		prev = *pp;
	}
}

void
mutex_init(struct mutex *m, const char *name)
{

	spin_init(&m->mtx_guard, name);
	m->mtx_owner        = NULL;
	m->mtx_waiters_head = NULL;
	m->mtx_waiters_tail = NULL;
	m->mtx_name         = name;
}

void
mutex_lock(struct mutex *m)
{

	/*
	 * Checked before the guard is taken, since taking it disables
	 * preemption.  Preemption already off means interrupt context or a
	 * spinlock held, where the block below could never be woken; better
	 * a panic naming the lock than that wedge.
	 */
	KASSERT(preempt_is_enabled(),
	    "mutex_lock with preemption disabled -- spinlock held, or IRQ?");

	spin_lock(&m->mtx_guard);
	while (m->mtx_owner != NULL) {
		KASSERT(m->mtx_owner != current_thread,
		    "recursive mutex_lock");
		mutex_n_slept++;
		waiter_push(m, current_thread);
		thread_wait_note(current_thread, &m->mtx_waiters_head,
		    &m->mtx_waiters_tail, &m->mtx_guard);
		/*
		 * Drops the guard under sched_lock, so a racing unlock spins
		 * on sched_lock and cannot slip its wake in before the switch.
		 */
		thread_block_release(THREAD_BLOCK_SLEEP, m, &m->mtx_guard);
		spin_lock(&m->mtx_guard);
		/*
		 * Off the list unconditionally, whatever ended the park.  An
		 * unlock that woke us already popped us; a park declined for a
		 * pending wake did not, and the next push would then link the
		 * tail to itself.
		 */
		waiter_unbind(m, current_thread);
		thread_wait_forget(current_thread);
		/*
		 * Re-check: being woken means the lock was free, not that it
		 * is ours -- a thread that never slept may have taken it.  A
		 * direct handoff would make the woken thread an owner before
		 * it has even run.
		 */
	}
	m->mtx_owner = current_thread;
	/*
	 * Read by the kill checks (th_mutex_depth, kern/thread.h).  Only
	 * this thread moves it, here and in unlock: no lock needed.
	 */
	current_thread->th_mutex_depth++;
	mutex_n_acquire++;
	spin_unlock(&m->mtx_guard);
}

uint64_t
mutex_blocks(void)
{

	return (mutex_n_slept);
}

void
mutex_stats(void)
{

	if (mutex_n_acquire == 0)
		return;
	kprintf("mutex: %llu acquisitions, %llu had to sleep\n",
	    (unsigned long long)mutex_n_acquire,
	    (unsigned long long)mutex_n_slept);
}

bool
mutex_trylock(struct mutex *m)
{
	bool	got;

	spin_lock(&m->mtx_guard);
	got = m->mtx_owner == NULL;
	if (got) {
		m->mtx_owner = current_thread;
		current_thread->th_mutex_depth++;
	}
	spin_unlock(&m->mtx_guard);
	return (got);
}

void
mutex_unlock(struct mutex *m)
{
	struct thread	*w;

	spin_lock(&m->mtx_guard);
	KASSERT(m->mtx_owner == current_thread,
	    "mutex_unlock by a thread that does not hold it");
	KASSERT(current_thread->th_mutex_depth > 0,
	    "mutex_unlock: the held-mutex count is already zero");
	m->mtx_owner = NULL;
	current_thread->th_mutex_depth--;
	w = waiter_pop(m);
	/*
	 * Hold taken under the guard, where the waiter is provably alive: it
	 * cannot finish exiting without taking the guard to unbind itself.
	 * After the unlock below it can (a kill wakes it, it retires, the
	 * reaper runs), and the hold keeps the reaper off until our wake
	 * lands.
	 */
	if (w != NULL)
		thread_hold(w);
	spin_unlock(&m->mtx_guard);

	/* The wake itself goes outside the guard. */
	if (w != NULL) {
		thread_wake(w);
		thread_unhold(w);
	}
}

bool
mutex_held(const struct mutex *m)
{

	/*
	 * No guard needed: the only actionable answer is "yes, I hold it",
	 * and only the owner can change that.
	 */
	return (m->mtx_owner == current_thread);
}

/* ---- selftest -------------------------------------------------------- */

/*
 * A kill landing on a thread that holds one lock and waits for another --
 * the shape of every disk write (fs_lock held, waiting on the drive),
 * here without the driver so it is deterministic.  The victim takes the
 * outer lock and parks acquiring the inner one, which this test holds.
 * The kill lands mid-park and is declined (th_mutex_depth is up), so the
 * victim stays parked, owning the outer lock.  Once the inner lock is
 * released it takes it, gives both back, and only then retires.  Checked:
 * both locks free afterwards, and a flag showing it outlived the kill.
 */
#define	MK_WAIT_MS	4000u

static struct mutex	mk_outer = MUTEX_INIT("mk-outer");
static struct mutex	mk_inner = MUTEX_INIT("mk-inner");

static struct {
	volatile int	 mk_holding;	/* victim owns the outer lock  */
	volatile int	 mk_done;	/* ...and gave both back alive */
	struct thread	*mk_thread;
} mk;

static void
mutex_kill_entry(void *arg)
{

	(void)arg;
	mutex_lock(&mk_outer);
	mk.mk_holding = 1;
	mutex_lock(&mk_inner);
	mutex_unlock(&mk_inner);
	mutex_unlock(&mk_outer);
	mk.mk_done = 1;
	/* The trampoline's thread_exit retires us at last, empty-handed. */
}

void
mutex_kill_selftest(void)
{
	struct task	*vt;
	unsigned int	 i;

	mutex_lock(&mk_inner);

	vt = task_create("mutex-kill");
	if (vt == NULL) {
		kprintf("mutex-kill: FAIL no task to kill\n");
		mutex_unlock(&mk_inner);
		return;
	}
	mk.mk_holding = 0;
	mk.mk_done    = 0;
	mk.mk_thread  = thread_create(vt, mutex_kill_entry, NULL,
	    "mutex-kill");
	if (mk.mk_thread == NULL) {
		kprintf("mutex-kill: FAIL no thread to kill\n");
		mutex_unlock(&mk_inner);
		task_deref(vt);
		return;
	}
	thread_start(mk.mk_thread);

	for (i = 0; i < MK_WAIT_MS && mk.mk_holding == 0; i++)
		sched_nap_ms(1);
	for (i = 0; i < MK_WAIT_MS &&
	    mk_inner.mtx_waiters_head != mk.mk_thread; i++)
		sched_nap_ms(1);
	if (mk.mk_holding == 0 ||
	    mk_inner.mtx_waiters_head != mk.mk_thread) {
		kprintf("mutex-kill: FAIL the victim never parked on the "
		    "inner lock\n");
		mutex_unlock(&mk_inner);
		task_deref(vt);
		return;
	}

	task_request_terminate(vt->t_id);

	/*
	 * Let the kill's wake land and be declined: the victim wakes, sees
	 * it holds the outer lock, and parks again.  So it must not have got
	 * past the inner lock, and the outer lock must still be owned -- a
	 * victim retired anyway would leave it latched, which the trylock
	 * probes.
	 */
	sched_nap_ms(50);
	if (mk.mk_done != 0) {
		kprintf("mutex-kill: FAIL the victim got past a lock this "
		    "test is still holding\n");
		mutex_unlock(&mk_inner);
		task_deref(vt);
		return;
	}
	if (mutex_trylock(&mk_outer)) {
		mutex_unlock(&mk_outer);
		kprintf("mutex-kill: FAIL the kill tore the outer lock out "
		    "of the victim's hand\n");
		mutex_unlock(&mk_inner);
		task_deref(vt);
		return;
	}

	mutex_unlock(&mk_inner);

	for (i = 0; i < MK_WAIT_MS && mk.mk_done == 0; i++)
		sched_nap_ms(1);
	if (mk.mk_done == 0) {
		kprintf("mutex-kill: FAIL the victim never came back for "
		    "the inner lock\n");
		task_deref(vt);
		return;
	}
	if (!mutex_trylock(&mk_outer)) {
		kprintf("mutex-kill: FAIL the outer lock did not come home\n");
		task_deref(vt);
		return;
	}
	mutex_unlock(&mk_outer);
	if (!mutex_trylock(&mk_inner)) {
		kprintf("mutex-kill: FAIL the inner lock did not come home\n");
		task_deref(vt);
		return;
	}
	mutex_unlock(&mk_inner);
	task_deref(vt);

	kprintf("mutex-kill: PASS -- a thread killed holding one lock and "
	    "waiting for another was left to finish the walk, gave both "
	    "locks back, and retired empty-handed\n");
}
