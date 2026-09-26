/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_SPINLOCK_H_
#define	_SYS_SPINLOCK_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Test-and-set spinlock with debug instrumentation.  A hold disables
 * interrupts (saved and restored, so sections nest) and preemption, and
 * spins answer TLB shootdowns; see spinlock.c.
 *
 * Lock key on the struct itself:
 *	(a)	atomic; written via __atomic_exchange / __atomic_store
 *	(c)	const after init
 *	(s)	written only while the lock is held -- safe to read
 *		without a lock once you have observed sl_state == 1
 *
 * The holder fields record the call site and CPU (machine/cpu.h) for
 * diagnostics, the recursive-acquire check and the non-owner-unlock
 * check.  Lock-order checking is kern/witness.h.
 *
 * Static initialiser:  static struct spinlock x = SPINLOCK_INIT("x");
 * Dynamic init:        spin_init(&lock, "name");
 */

struct spinlock {
	volatile uint32_t	sl_state;	/* (a) 0=free, 1=held    */
	const char		*sl_name;	/* (c) const after init  */
	uintptr_t		sl_holder_rip;	/* (s) caller RA on lock */
	int			sl_holder_cpu;	/* (s) -1 when unheld    */
};

#define	SPINLOCK_INIT(nm)						\
	{ .sl_state = 0, .sl_name = (nm),				\
	  .sl_holder_rip = 0, .sl_holder_cpu = -1 }

void	spin_init(struct spinlock *, const char *name);
void	spin_lock(struct spinlock *);
void	spin_unlock(struct spinlock *);
bool	spin_held(const struct spinlock *);

/*
 * Non-blocking acquire.  Returns true on success (lock now held), false
 * if the lock is busy.  Caller must spin_unlock on success; on failure
 * the interrupt state, preempt count and lock are as they were.  For
 * callers that would rather skip the work than wait, e.g. an interrupt
 * handler.
 */
bool	spin_trylock(struct spinlock *);

#define	SPINLOCK_ASSERT_HELD(sl)					\
	KASSERT(spin_held(sl),						\
	    "spinlock not held where expected")

#endif /* !_SYS_SPINLOCK_H_ */
