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
#include "sched.h"
#include "spinlock.h"
#include "thread.h"
#include "witness.h"

/*
 * WHERE THE INTERRUPT STATE OF A CRITICAL SECTION LIVES.
 *
 * In the running thread, once there is one, and in the CPU before that.  The
 * choice matters and is argued in thread.h beside the fields: sched_lock is
 * held across a context switch, so a per-CPU answer would be restored by a
 * thread that never saved it.
 *
 * The two are never mixed within one acquire/release pair, because the only
 * thing that changes which one is in use is a thread coming into existence,
 * and that does not happen with a lock held.
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
	bool		was_on;

	ra = (uintptr_t)__builtin_return_address(0);

	/*
	 * ⚠ INTERRUPTS OFF FIRST, AND THIS IS WHAT MAKES THE LOCK AN SMP
	 * LOCK RATHER THAN A UNIPROCESSOR ONE.
	 *
	 * The acquire below has always been a real atomic exchange, so two
	 * processors could never both hold the lock.  What was missing is the
	 * case of ONE processor against ITSELF: take a lock, be interrupted,
	 * and have the handler ask for the same lock.  On one CPU that could
	 * not happen, because no interrupt handler here takes a lock the
	 * mainline can hold -- an argument about which handlers exist, not
	 * about the lock, and one that stops being available the moment a
	 * second processor runs kernel code with interrupts enabled.  So the
	 * lock stops relying on it.
	 *
	 * Saved and restored rather than cleared and set: nested critical
	 * sections must leave interrupts off until the OUTERMOST one ends,
	 * and code that was already running with them off must not have them
	 * turned on underneath it.
	 */
	was_on = intr_save_disable();

	/*
	 * Bump preempt-disable BEFORE attempting acquire, so a timer
	 * IRQ that fires mid-acquire sees us as in a critical section
	 * and refuses to preempt.  Otherwise lock-holder preemption
	 * happens and other waiters spin pointlessly.
	 */
	preempt_disable();

	if ((*spin_depth_slot())++ == 0)
		*spin_saved_if_slot() = was_on;

	if (sl->sl_state != 0 && sl->sl_holder_cpu == (int)cpu_id()) {
		/*
		 * Spell out both sites with ksym_print BEFORE calling panic,
		 * because panic's %lx formatter has no hook for symbol
		 * resolution.  Doing it here means the recursive-acquire
		 * report names the offending function on both sides:
		 *	"attempted from sched_drain_irq_wakes+0x42, prior
		 *	 holder thread_wake+0x10".
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

	while (__atomic_exchange_n(&sl->sl_state, 1u, __ATOMIC_ACQUIRE) != 0)
		__asm__ __volatile__ ("pause");

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
	 * they were on when it began.  Before preempt_enable, which may yield
	 * -- so the switch happens with interrupts in the state the caller
	 * had them, which is the state they were in before this rung too.
	 *
	 * Between the two, preemption is still disabled, so an interrupt
	 * arriving in that window is serviced and returns rather than
	 * rescheduling from inside a lock release.
	 */
	depth = spin_depth_slot();
	KASSERT(*depth > 0, "spin_unlock with no critical section held");
	if (--(*depth) == 0)
		intr_restore(*spin_saved_if_slot());

	/*
	 * Re-enable preemption.  If this drops us out of every
	 * critical section AND a timer tick has set need_resched while we
	 * were inside one, preempt_enable yields the CPU here so
	 * the deferred schedule actually happens before the caller
	 * runs another loop iteration.
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
		 * Nothing was taken, so nothing is owed: put interrupts back
		 * exactly as they were found.  Restoring from the saved value
		 * rather than unconditionally enabling is what makes a failed
		 * trylock inside another critical section harmless -- there
		 * they were already off, and this leaves them off.
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
