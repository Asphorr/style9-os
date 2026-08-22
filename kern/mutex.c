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

/* See mutex.h for what this is and why it had to exist. */

/*
 * Counted because a lock that is never contended never exercises the half of
 * itself that is hard: the park and the wake.  A test that passes while
 * mutex_n_slept stayed at zero has proved only that uncontended acquisition
 * works, and would go on passing while the sleep path was broken.
 */
static uint64_t	mutex_n_acquire;	/* (g) successful mutex_lock calls */
static uint64_t	mutex_n_slept;		/* (g) ...that had to block first  */

/*
 * Waiters are linked through th_wait_link -- the field an OBJECT'S queue owns,
 * shared with the port waiter lists in mach/port_msg.c because a thread is on
 * exactly one of them at a time.  Not through th_runq_link, which it used to
 * be: see the field's own comment in kern/thread.h for what the scheduler does
 * to a list it does not know it is holding.
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
 * Take a specific thread off the waiter list if it is still on it, exactly
 * as the port layer's port_unbind_waiter_locked does and for the same
 * caller: the unconditional detach after a park, whatever ended it.
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
	 * Checked before the guard is taken, because taking it disables
	 * preemption and would make the question unanswerable.  A caller
	 * arriving here with preemption already off is either in interrupt
	 * context or holding a spinlock; both mean the block below would park
	 * this thread where nothing can wake it, and a panic naming the lock
	 * is worth far more than the wedge that would otherwise follow.
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
		 * Drops the guard under sched_lock, so an unlock racing this
		 * park has to spin on sched_lock and cannot slip its wake in
		 * between the release and the switch.
		 */
		thread_block_release(THREAD_BLOCK_SLEEP, m, &m->mtx_guard);
		spin_lock(&m->mtx_guard);
		/*
		 * Off the list unconditionally, whatever it was that ended
		 * the park -- the lesson the port's recv loop wrote down and
		 * this loop had not yet learned.  An unlock that woke us
		 * already popped us; a park that DECLINED TO PARK (a wake
		 * was pending) did not, and the push above would then link
		 * the tail into itself: a one-element cycle, and every wake
		 * the unlock hands out goes to the phantom for ever.
		 */
		waiter_unbind(m, current_thread);
		thread_wait_forget(current_thread);
		/*
		 * Re-check rather than assume.  Being woken means the lock was
		 * free at that moment, not that it is ours: a thread that never
		 * slept can take it between the wake and this reacquisition.
		 * Handing ownership over directly would fix that and introduce
		 * a worse problem, since the winner would then be holding a
		 * lock it has not yet returned to.
		 */
	}
	m->mtx_owner = current_thread;
	/*
	 * The count the kill checks read (see th_mutex_depth in
	 * kern/thread.h): while it is up, a kill declines to retire this
	 * thread, because a mutex dies with its owner.  Moved only by its
	 * own thread, here and in unlock, so it needs no lock of its own.
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
	 * Held from under the guard, where the waiter is provably alive: a
	 * thread on this list cannot finish exiting without taking the guard
	 * to unbind itself.  Between the unlock below and the wake, it can --
	 * a kill fan-out wakes it, it retires, the reaper frees it -- and
	 * the hold is what makes the reaper wait for our wake to land.
	 */
	if (w != NULL)
		thread_hold(w);
	spin_unlock(&m->mtx_guard);

	/*
	 * Outside the guard.  thread_wake takes sched_lock, and this kernel
	 * drains deferred wakes only when no lock is held -- waking from under
	 * the guard would queue the wake behind the very release that is
	 * trying to let someone run.
	 */
	if (w != NULL) {
		thread_wake(w);
		thread_unhold(w);
	}
}

bool
mutex_held(const struct mutex *m)
{

	/*
	 * Read without the guard on purpose.  The only answer a caller can act
	 * on is "yes, I hold it", and that one cannot change underneath the
	 * thread asking, because only the owner releases it.
	 */
	return (m->mtx_owner == current_thread);
}

/* ---- selftest -------------------------------------------------------- */

/*
 * WHAT A KILL DOES TO A THREAD HOLDING ONE LOCK AND WAITING FOR ANOTHER --
 * which is every disk write in this kernel, seen from close up: the outer
 * lock is fs_lock, the inner wait is the drive's interrupt, and a kill that
 * landed there used to retire the thread on the spot, taking the outer lock
 * to the grave with it.  Every later acquirer then parks behind a corpse,
 * which for fs_lock means the machine never touches the disk again.
 *
 * The scene is that shape with the driver taken out, so it is deterministic:
 * the victim takes an outer lock and parks acquiring an inner one this test
 * is holding.  The kill lands mid-park and DECLINES -- th_mutex_depth is up
 * -- so the victim stays parked, still owning what it owned.  Then the inner
 * lock is released, and a thread the kernel has already agreed to kill
 * finishes the walk: takes the inner lock, gives both back, and only then
 * retires.  The asserts are on what it leaves behind: two locks anyone can
 * take, and a flag proving it lived past its own death warrant.
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
	 * Let the kill's wake fan-out land and be declined.  The victim
	 * wakes, reads its own death warrant, sees the outer lock in its
	 * hand, and parks again -- so nothing here may have changed: the
	 * victim has not moved past a lock this test still holds, and the
	 * outer lock still has a living owner.  (A kill that retired it
	 * anyway would leave the outer lock latched for ever, which is
	 * exactly what the trylock probes.)
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
