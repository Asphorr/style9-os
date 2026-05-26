/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "panic.h"
#include "sched.h"
#include "spinlock.h"

/*
 * Single-CPU placeholder; once an SMP boot exists this becomes the
 * per-CPU LAPIC id.  Centralised so every reference reads from the
 * same source of truth.
 */
static inline int
cpu_id(void)
{

	return (0);
}

void
spin_init(struct spinlock *sl, const char *name)
{

	sl->sl_state      = 0;
	sl->sl_name       = name;
	sl->sl_holder_rip = 0;
	sl->sl_holder_cpu = -1;
}

void
spin_lock(struct spinlock *sl)
{
	uintptr_t	ra;

	ra = (uintptr_t)__builtin_return_address(0);

	/*
	 * Bump preempt-disable BEFORE attempting acquire, so a PIT
	 * IRQ that fires mid-acquire sees us as in a critical section
	 * and refuses to preempt.  Otherwise lock-holder preemption
	 * happens and other waiters spin pointlessly.
	 */
	preempt_disable();

	if (sl->sl_state != 0 && sl->sl_holder_cpu == cpu_id()) {
		panic("spin_lock(%s): recursive acquire from "
		    "rip=0x%016lx (prior holder rip=0x%016lx)",
		    sl->sl_name != NULL ? sl->sl_name : "?",
		    (unsigned long)ra,
		    (unsigned long)sl->sl_holder_rip);
	}

	while (__atomic_exchange_n(&sl->sl_state, 1u, __ATOMIC_ACQUIRE) != 0)
		__asm__ __volatile__ ("pause");

	sl->sl_holder_rip = ra;
	sl->sl_holder_cpu = cpu_id();
}

void
spin_unlock(struct spinlock *sl)
{

	KASSERT(sl->sl_state == 1, "spin_unlock of unheld lock");
	KASSERT(sl->sl_holder_cpu == cpu_id(),
	    "spin_unlock by non-owner CPU");

	sl->sl_holder_rip = 0;
	sl->sl_holder_cpu = -1;
	__atomic_store_n(&sl->sl_state, 0u, __ATOMIC_RELEASE);

	/*
	 * Re-enable preemption.  If this drops us out of every
	 * critical section AND PIT has set need_resched while we
	 * were inside one, preempt_enable yields the CPU here so
	 * the deferred schedule actually happens before the caller
	 * runs another loop iteration.
	 */
	preempt_enable();
}

bool
spin_held(const struct spinlock *sl)
{

	return (sl->sl_state == 1 && sl->sl_holder_cpu == cpu_id());
}

bool
spin_trylock(struct spinlock *sl)
{
	uintptr_t	ra;

	ra = (uintptr_t)__builtin_return_address(0);

	preempt_disable();
	if (__atomic_exchange_n(&sl->sl_state, 1u, __ATOMIC_ACQUIRE) != 0) {
		preempt_enable();
		return (false);
	}

	sl->sl_holder_rip = ra;
	sl->sl_holder_cpu = cpu_id();
	return (true);
}
