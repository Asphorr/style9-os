/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_MUTEX_H_
#define	_SYS_MUTEX_H_

#include <stdbool.h>
#include <stdint.h>

#include "spinlock.h"

/*
 * A lock a thread may hold across sleeping -- e.g. across disk I/O, which
 * waits for the drive's interrupt.  A spinlock cannot be: it keeps
 * preemption and interrupts off on its CPU, and deferred wakes are drained
 * only when the preempt count returns to zero.
 *
 * A thread that finds the lock held parks on its waiter list and is woken
 * when the holder releases.  Not recursive: a second acquire by the owner
 * panics.  Not a handoff: the woken thread re-checks the owner and may
 * lose to a newcomer, so the wait is a loop.
 *
 * Never from interrupt context (an IRQ has no thread to park), and never
 * while holding a spinlock: mutex_lock asserts that preemption is enabled,
 * catching the mistake when it is made.
 *
 * The guard spinlock is held only to inspect and update the owner and the
 * waiter list, never across the block: thread_block_release drops it under
 * sched_lock, so a release between the drop and the switch cannot lose
 * the wake.
 *
 * A holder's kill is deferred until it holds no mutex (th_mutex_depth in
 * kern/thread.h).
 *
 * Static initialiser:  static struct mutex m = MUTEX_INIT("name");
 * Dynamic init:        mutex_init(&m, "name");
 */

struct thread;

struct mutex {
	struct spinlock	 mtx_guard;	/* protects everything below   */
	struct thread	*mtx_owner;	/* (g) NULL when free          */
	struct thread	*mtx_waiters_head;	/* (g) FIFO, so a waiter */
	struct thread	*mtx_waiters_tail;	/* (g) cannot be starved */
	const char	*mtx_name;	/* (c) const after init        */
};

#define	MUTEX_INIT(nm)							\
	{ .mtx_guard = SPINLOCK_INIT(nm), .mtx_owner = NULL,		\
	  .mtx_waiters_head = NULL, .mtx_waiters_tail = NULL,		\
	  .mtx_name = (nm) }

void	mutex_init(struct mutex *, const char *name);

/*
 * Acquire, sleeping until it is ours.  Returns holding the lock.
 */
void	mutex_lock(struct mutex *);

/*
 * Release, waking the longest-waiting thread if there is one.  Only the
 * owner may release.
 */
void	mutex_unlock(struct mutex *);

/*
 * Acquire only if free: true holding it, false having done nothing.  It
 * never sleeps, so unlike mutex_lock it is legal with preemption disabled
 * -- though it records current_thread as the owner.
 */
bool	mutex_trylock(struct mutex *);

/* Does the calling thread hold it?  For assertions. */
bool	mutex_held(const struct mutex *);

/*
 * How many acquisitions, across every mutex, had to park.  A test that
 * passes with this at zero has exercised only the uncontended path.
 */
uint64_t	mutex_blocks(void);
void		mutex_stats(void);

/*
 * Boot scene for the kill-vs-held-mutex contract (th_mutex_depth in
 * kern/thread.h): a thread killed while holding one lock and parked on
 * another must be left to give both back before it dies.
 */
void		mutex_kill_selftest(void);

#define	MUTEX_ASSERT_HELD(m)						\
	KASSERT(mutex_held(m), "mutex not held where expected")

#endif /* !_SYS_MUTEX_H_ */
