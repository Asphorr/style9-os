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
 * A lock a thread may hold across sleeping.
 *
 * WHY THIS HAD TO EXIST.  Until now the kernel's only lock was the spinlock,
 * and a spinlock in this kernel cannot be held across an operation that
 * blocks: preempt_count is global, so a thread that parks while holding one
 * leaves the count above zero, and the deferred wake queue is drained only
 * when it reaches zero.  Blocking under a spinlock therefore does not merely
 * stall the machine, it guarantees the sleeper is never woken.
 *
 * Every path that touches the disk blocks -- ata_kread and ata_kwrite both
 * wait for the drive's interrupt.  So there was no way to make a disk-touching
 * path mutually exclusive, and the code that needed it worked around the
 * absence: fs/bio.c marks a buffer busy, drops its spinlock, and has losers
 * spin on thread_yield until the flag clears.  That is a lock, hand-rolled out
 * of the only parts available, and it is one of ten such yield loops.
 *
 * The filesystem got away without any lock at all for a different reason: its
 * volume state is filled at mount and read-only afterwards, so readers had
 * nothing to serialise.  Writing ended that, which is what makes this the
 * moment to build the primitive rather than a tenth workaround.
 *
 * WHAT IT IS.  A thread that finds the lock held parks on its waiter list and
 * is woken when the holder releases.  Not recursive: a second acquire by the
 * owner is a bug, and panics rather than deadlocking silently.  Not a handoff
 * -- the woken thread re-checks the owner and may lose to a third thread that
 * arrived meanwhile, which is why the wait is a loop and not an if.
 *
 * WHAT IT IS NOT.  Never callable from interrupt context: an IRQ has no thread
 * to park.  Never acquirable while holding a spinlock, for the reason at the
 * top -- mutex_lock asserts that preemption is currently enabled, so that
 * mistake is caught at the moment it is made rather than as a wedge later.
 *
 * The guard spinlock inside is held only to inspect and update the owner and
 * the waiter list, never across the block itself: thread_block_release drops
 * it under sched_lock, so a release firing between the drop and the switch is
 * forced to spin on sched_lock and cannot lose the wake.
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
 * Release, waking the longest-waiting thread if there is one.  The wake is
 * posted AFTER the guard is dropped: this kernel drains deferred wakes only
 * once no lock is held, so waking from under one would defer the very wake
 * that lets the machine make progress.
 */
void	mutex_unlock(struct mutex *);

/*
 * Acquire only if free.  Returns true holding it, false having done nothing
 * and having slept not at all -- so unlike mutex_lock this is legal wherever
 * a spinlock would be, including with preemption disabled.
 */
bool	mutex_trylock(struct mutex *);

/* Does the calling thread hold it?  For assertions. */
bool	mutex_held(const struct mutex *);

/*
 * How many acquisitions have had to park, across every mutex.  A test that
 * passes with this at zero has exercised only the uncontended path and proves
 * nothing about the half that is hard, so the number is exposed rather than
 * inferred.
 */
uint64_t	mutex_blocks(void);
void		mutex_stats(void);

#define	MUTEX_ASSERT_HELD(m)						\
	KASSERT(mutex_held(m), "mutex not held where expected")

#endif /* !_SYS_MUTEX_H_ */
