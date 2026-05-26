/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_SCHED_H_
#define	_SYS_SCHED_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

struct thread;

/*
 * Cooperative round-robin scheduler.
 *
 * No preemption yet -- threads yield explicitly via thread_yield or
 * implicitly via thread_block (e.g. mach_msg_recv_block on an empty
 * port).  The PIT IRQ keeps bumping the tick counter but does NOT
 * call into the scheduler; that would require an "ask for resched on
 * return-to-thread" flag and the IRQ trampoline to honour it.  Easy
 * to add later without changing the API below.
 *
 * Lock order: anywhere that takes sched_lock, take it last.
 */

void		sched_init(void);
void		sched_enqueue(struct thread *);

/*
 * thread_yield: put current at the tail of the runqueue and switch
 * to whoever's at the head.  If the runqueue is empty the idle thread
 * runs.  Returns when the caller is rescheduled.
 */
void		thread_yield(void);

/*
 * thread_block: mark current BLOCKED, record reason and target, then
 * pick the next runnable thread.  The caller is expected to be
 * embedded inside a synchronisation primitive that will arrange for
 * thread_wake() to be called later.  Returns when woken.
 */
void		thread_block(int reason, void *target);

/*
 * thread_block_release: like thread_block, but drops `external` with
 * sched_lock held -- so any thread_wake fired between this drop and
 * the actual switch is forced to spin on sched_lock and cannot lose
 * the wake.  Use when blocking from inside a synchronisation
 * primitive: pass that primitive's lock so it is released atomically
 * with the state transition.
 */
struct spinlock;
void		thread_block_release(int reason, void *target,
		    struct spinlock *external);

/*
 * thread_wake: transition `th` from BLOCKED to READY and enqueue it.
 * Safe to call on a not-blocked thread (no-op).
 */
void		thread_wake(struct thread *);

/*
 * IRQ-safe deferred wake.
 *
 * `sched_post_irq_wake` is callable from interrupt context: it appends
 * `th` to a lock-free atomic list and returns at once, without touching
 * sched_lock.  It is the primitive a hardware driver uses to signal a
 * thread blocked on kbd / serial / etc. from the IRQ that delivered the
 * event.
 *
 * `sched_drain_irq_wakes` pops the list and runs thread_wake on every
 * entry.  It is called from two safe spots in normal kernel context:
 * the tail of intr_dispatch (right before the preempt point) and the
 * tail of preempt_enable (just after the count drops to zero).  Both
 * are guaranteed to be running with no spinlock held, so taking
 * sched_lock from inside thread_wake cannot recurse onto a lock the
 * interrupted thread was holding.
 */
void		sched_post_irq_wake(struct thread *);
void		sched_drain_irq_wakes(void);

/*
 * Timed-block plumbing.  A thread that wants to be woken either by a
 * pending event OR by a deadline records its deadline in
 * th_wake_deadline_ms (absolute clock_uptime_ms()) and then calls
 * sched_add_timed_waiter before parking via thread_block_release.  On
 * wake the thread must call sched_remove_timed_waiter (idempotent) to
 * detach itself, then inspect th_timed_out to discriminate "event
 * arrived" from "deadline expired".
 *
 * sched_check_timeouts is the PIT-IRQ tail that scans the list and
 * posts IRQ wakes for expired entries.
 */
void		sched_add_timed_waiter(struct thread *);
void		sched_remove_timed_waiter(struct thread *);
void		sched_check_timeouts(void);

/*
 * sched_handoff_zombie: called from thread_exit().  Adds the thread
 * to the zombie list (idle thread reaps them) and switches away.
 * Never returns.
 */
void		sched_handoff_zombie(struct thread *)
		    __attribute__((noreturn));

/*
 * Reap any thread that called thread_exit() but whose struct + kstack
 * are still around.  The idle thread calls this on every iteration;
 * tests and other long-running code can call it explicitly so they
 * don't have to bounce through idle just to get a deterministic
 * cleanup point.
 */
void		sched_reap_zombies(void);

/*
 * Diagnostics.
 */
void		sched_print(void);
size_t		sched_runq_len(void);
uint64_t	sched_context_switches(void);
uint64_t	sched_preempts(void);

/*
 * Tally one preempt event.  Called from both the inline path
 * (intr_dispatch) and the deferred path (preempt_enable) so the
 * counter reflects every IRQ-driven schedule, not just one flavour.
 */
void		sched_count_preempt(void);

/*
 * Preemption primitives.
 *
 * preempt_disable / preempt_enable bracket critical sections that
 * must not be preempted -- spin_lock / spin_unlock call them
 * automatically.  Tracking is per-thread (the count lives in
 * struct thread), so a thread switched in mid-critical-section
 * resumes with its own count intact.
 *
 * need_resched is set by PIT when the current thread's quantum is
 * exhausted.  It is honoured either inline at the end of
 * intr_dispatch, or deferred until the next preempt_enable that
 * drops the count to zero.  thread_yield clears both need_resched
 * and quantum_used so the new thread gets a fresh slice.
 */
extern volatile int		preempt_need_resched;	/* (a) */
extern volatile unsigned int	preempt_quantum_used;	/* (a) */

#define	PREEMPT_QUANTUM_TICKS	5	/* ~50 ms at 100 Hz */

void		preempt_disable(void);
void		preempt_enable(void);
bool		preempt_is_enabled(void);

#endif /* !_SYS_SCHED_H_ */
