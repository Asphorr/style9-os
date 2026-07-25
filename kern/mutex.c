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
 * Waiters are linked through th_runq_link, which is free while a thread is
 * blocked -- the same field and the same reasoning the port waiter lists use
 * (mach/port_msg.c).  A thread is on exactly one of the two at a time, and
 * being on a waiter list is precisely the state of not being runnable.
 */
static void
waiter_push(struct mutex *m, struct thread *th)
{

	th->th_runq_link = NULL;
	if (m->mtx_waiters_tail != NULL)
		m->mtx_waiters_tail->th_runq_link = th;
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
	m->mtx_waiters_head = th->th_runq_link;
	if (m->mtx_waiters_head == NULL)
		m->mtx_waiters_tail = NULL;
	th->th_runq_link = NULL;
	return (th);
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
		/*
		 * Drops the guard under sched_lock, so an unlock racing this
		 * park has to spin on sched_lock and cannot slip its wake in
		 * between the release and the switch.
		 */
		thread_block_release(THREAD_BLOCK_SLEEP, m, &m->mtx_guard);
		spin_lock(&m->mtx_guard);
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
	if (got)
		m->mtx_owner = current_thread;
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
	m->mtx_owner = NULL;
	w = waiter_pop(m);
	spin_unlock(&m->mtx_guard);

	/*
	 * Outside the guard.  thread_wake takes sched_lock, and this kernel
	 * drains deferred wakes only when no lock is held -- waking from under
	 * the guard would queue the wake behind the very release that is
	 * trying to let someone run.
	 */
	if (w != NULL)
		thread_wake(w);
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
