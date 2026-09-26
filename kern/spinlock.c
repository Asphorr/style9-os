/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "intr.h"
#include "kprintf.h"
#include "ksym.h"
#include "panic.h"
#include "pmap.h"
#include "sched.h"
#include "spinlock.h"
#include "thread.h"
#include "witness.h"

/*
 * Spin watchdog.  On SMP a wait can be unbounded (two CPUs each holding
 * what the other wants), and it looks like a busy machine getting nothing
 * done.  So a pathologically long wait reports the lock, the waiting CPU
 * and site, and the holding CPU and site, both by symbol -- one address
 * alone names a victim, not a cause.
 *
 * Reported, then waited on anyway: a panic with interrupts off while the
 * other CPUs spin is worth less than the report.  Once per boot, since a
 * second report would be the same deadlock seen by the next CPU, on a
 * console the wedged CPUs are queueing for.
 */
#define	SPIN_WATCHDOG_SPINS	(1u << 28)

static volatile int	spin_watchdog_said;	/* (a) */

static void
spin_watchdog(const struct spinlock *sl, uintptr_t ra)
{
	int	expected;

	expected = 0;
	if (!__atomic_compare_exchange_n(&spin_watchdog_said, &expected, 1,
	    false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED))
		return;

	kprintf("\n*** spin_lock(%s): cpu %u has been waiting a very long "
	    "time\n", sl->sl_name != NULL ? sl->sl_name : "?", cpu_id());
	kprintf("  waiting from ");
	ksym_print((uint64_t)ra);
	kprintf("\n  held by cpu %d, taken at ", sl->sl_holder_cpu);
	ksym_print((uint64_t)sl->sl_holder_rip);
	kprintf("\n");
}

/*
 * Where a critical section's depth and saved interrupt state live: in the
 * running thread once there is one, in the CPU before that (thread.h,
 * th_spin_depth, says why per-thread).  The two are never mixed within one
 * acquire/release pair: only a thread coming into existence changes which
 * is in use, and that does not happen with a lock held.
 */
static inline int *
spin_depth_slot(void)
{
	struct thread	*th;

	th = current_thread;
	return (th != NULL ? &th->th_spin_depth : &curcpu()->cp_spin_depth);
}

static inline bool *
spin_saved_if_slot(void)
{
	struct thread	*th;

	th = current_thread;
	return (th != NULL ? &th->th_spin_saved_if :
	    &curcpu()->cp_spin_saved_if);
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
	uint32_t	spins;
	bool		was_on;

	ra = (uintptr_t)__builtin_return_address(0);

	/*
	 * Interrupts off first, for the whole hold, so an interrupt handler
	 * on this CPU can never spin on a lock its own mainline holds.  Saved
	 * and restored rather than cleared and set: nested sections keep them
	 * off until the outermost ends, and code already running with them
	 * off never has them turned on underneath it.
	 */
	was_on = intr_save_disable();

	/*
	 * Bump preempt-disable before the acquire, so a timer IRQ mid-acquire
	 * sees a critical section and does not preempt a lock holder.
	 */
	preempt_disable();

	if ((*spin_depth_slot())++ == 0)
		*spin_saved_if_slot() = was_on;

	if (sl->sl_state != 0 && sl->sl_holder_cpu == (int)cpu_id()) {
		/*
		 * Name both sites with ksym_print before panicking; panic's
		 * formatter cannot resolve symbols.
		 */
		kprintf("\n*** spin_lock(%s): recursive acquire\n",
		    sl->sl_name != NULL ? sl->sl_name : "?");
		kprintf("  attempted from ");
		ksym_print((uint64_t)ra);
		kprintf("\n  prior holder  ");
		ksym_print((uint64_t)sl->sl_holder_rip);
		kprintf("\n");
		panic("spin_lock(%s): recursive acquire",
		    sl->sl_name != NULL ? sl->sl_name : "?");
	}

	/*
	 * While waiting, answer TLB shootdowns.  With interrupts off this CPU
	 * cannot take the IPI, and the sender may be waiting for the answer
	 * while holding the lock wanted here.  pmap_tlb_poll needs no lock
	 * and is two loads and a compare when nothing is outstanding.
	 */
	spins = 0;
	while (__atomic_exchange_n(&sl->sl_state, 1u, __ATOMIC_ACQUIRE) != 0) {
		pmap_tlb_poll();
		__asm__ __volatile__ ("pause");
		if (++spins == SPIN_WATCHDOG_SPINS)
			spin_watchdog(sl, ra);
	}

	sl->sl_holder_rip = ra;
	sl->sl_holder_cpu = (int)cpu_id();

	witness_acquired(sl, ra);
}

void
spin_unlock(struct spinlock *sl)
{
	int	*depth;

	KASSERT(sl->sl_state == 1, "spin_unlock of unheld lock");
	KASSERT(sl->sl_holder_cpu == (int)cpu_id(),
	    "spin_unlock by non-owner CPU");

	witness_released(sl);

	sl->sl_holder_rip = 0;
	sl->sl_holder_cpu = -1;
	__atomic_store_n(&sl->sl_state, 0u, __ATOMIC_RELEASE);

	/*
	 * Interrupts back, if this was the outermost critical section and
	 * they were on when it began.  Before preempt_enable, which may
	 * yield, so the switch happens with interrupts as the caller had
	 * them.  Preemption is still disabled in between, so an interrupt
	 * there is serviced and returns rather than rescheduling from inside
	 * a lock release.
	 */
	depth = spin_depth_slot();
	KASSERT(*depth > 0, "spin_unlock with no critical section held");
	if (--(*depth) == 0)
		intr_restore(*spin_saved_if_slot());

	/*
	 * If this leaves the last critical section and a reschedule was
	 * requested meanwhile, preempt_enable yields here.
	 */
	preempt_enable();
}

bool
spin_held(const struct spinlock *sl)
{

	return (sl->sl_state == 1 && sl->sl_holder_cpu == (int)cpu_id());
}

bool
spin_trylock(struct spinlock *sl)
{
	uintptr_t	ra;
	bool		was_on;

	ra = (uintptr_t)__builtin_return_address(0);

	was_on = intr_save_disable();
	preempt_disable();
	if (__atomic_exchange_n(&sl->sl_state, 1u, __ATOMIC_ACQUIRE) != 0) {
		/*
		 * Nothing taken, nothing owed: restore interrupts as found, not
		 * unconditionally on, so a failed trylock inside another
		 * critical section leaves them off.
		 */
		intr_restore(was_on);
		preempt_enable();
		return (false);
	}

	if ((*spin_depth_slot())++ == 0)
		*spin_saved_if_slot() = was_on;

	sl->sl_holder_rip = ra;
	sl->sl_holder_cpu = (int)cpu_id();

	witness_acquired(sl, ra);
	return (true);
}
