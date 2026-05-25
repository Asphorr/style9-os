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
 * Plain test-and-set spinlock with debug instrumentation.
 *
 * Lock key on the struct itself:
 *	(a)	atomic; written via __atomic_exchange / __atomic_store
 *	(s)	written only while the lock is held -- safe to read
 *		without a lock once you have observed sl_state == 1
 *
 * The holder fields are purely diagnostic; spin_lock records the call
 * site so that a hang or a SPINLOCK_ASSERT_HELD failure can pinpoint
 * which routine last touched it.  Single-CPU today, but the structure
 * is set up so adding a per-CPU id and a WITNESS-style lock-order
 * checker later is additive rather than disruptive.
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

#define	SPINLOCK_ASSERT_HELD(sl)					\
	KASSERT(spin_held(sl),						\
	    "spinlock not held where expected")

#endif /* !_SYS_SPINLOCK_H_ */
