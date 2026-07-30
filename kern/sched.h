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
 * Round-robin scheduler with PIT-driven preemption.  Threads also
 * yield explicitly via thread_yield, or implicitly via thread_block
 * (e.g. mach_msg_recv_block on an empty port).  The PIT IRQ debits
 * the current thread's PREEMPT_QUANTUM_TICKS slice and sets
 * preempt_need_resched on expiry; the reschedule fires either inline
 * at the end of intr_dispatch, or deferred until the next
 * preempt_enable that drops the count to zero.
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
 * thread_wake: transition `th` from BLOCKED to READY and enqueue it AHEAD of
 * the queue, since a thread that blocked gave its slice up unused.  Also asks
 * for a reschedule, so the wake reaches the CPU at the next tick rather than
 * when whoever is running happens to stop.  Safe to call on a not-blocked
 * thread (no-op).
 */
void		thread_wake(struct thread *);

/*
 * sched_wakeup: wake every thread parked on the channel `chan` -- that is,
 * every thread that passed `chan` as thread_block's target and has not been
 * woken since.  Returns how many there were.
 *
 * A channel is just an agreed address, conventionally a field of the object
 * being waited on: the pipe's byte count for readers waiting on data, its read
 * position for writers waiting on room.  Two waits on the same object want
 * different fields, so that they are separate channels is the point.
 *
 * The waiter's half of the contract is the whole difficulty.  Test the
 * condition and park under one lock, and hand that lock to
 * thread_block_release rather than dropping it first:
 *
 *	for (;;) {
 *		spin_lock(&obj->lock);
 *		if (ready(obj)) { ...; spin_unlock(&obj->lock); break; }
 *		thread_block_release(THREAD_BLOCK_SLEEP, &obj->field,
 *		    &obj->lock);
 *	}
 *
 * Unlock-then-block is the lost wakeup: an event in the gap wakes nobody and
 * the sleeper is never woken again.  A wake is also only a hint -- re-test in
 * the loop, since the bytes may be gone by the time this thread runs.
 */
uint32_t	sched_wakeup(void *chan);

/*
 * sched_wake_sleepers_of: wake every thread of `task` that is asleep on a
 * channel, leaving anything blocked on a port alone.  What makes a posted
 * signal reach a thread that is already inside a blocking syscall.  See the
 * definition for why the distinction is not optional.
 */
struct task;
uint32_t	sched_wake_sleepers_of(struct task *task);

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
void		sched_wake_latency_print(void);

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
 * automatically.  The count is kernel-wide rather than per-thread, and
 * deliberately: sched_lock is held ACROSS a context switch, so the
 * thread that releases it is not the one that took it and per-thread
 * accounting underflows.  What the counter answers is "is the kernel
 * inside any critical section", which is the question the PIT has.
 *
 * need_resched is set by PIT when the current thread's quantum is
 * exhausted.  It is honoured either inline at the end of
 * intr_dispatch, or deferred until the next preempt_enable that
 * drops the count to zero.  thread_yield clears both need_resched
 * and quantum_used so the new thread gets a fresh slice.
 */
extern volatile int		preempt_need_resched;	/* (a) */
extern volatile unsigned int	preempt_quantum_used;	/* (a) */

/*
 * How long a thread may hold the CPU before the PIT takes it away.
 *
 * THE SLICE IS ALSO SOMEBODY ELSE'S WAITING TIME.  Whoever is behind this
 * thread in the queue waits out the whole of it, and a thread near the front
 * of the queue is often one the others are waiting FOR -- a child that has to
 * reach _exit before its parent's wait4 can return, a writer that has to reach
 * write before a reader sees a byte.  At five ticks, measured: a parent that
 * read a pipe and then waited for the child that wrote it took 104 ms to get
 * both answers, of which one whole quantum was the child queueing for the CPU
 * it needed to finish dying.
 *
 * Two ticks rather than one because the curve has a knee there and going
 * further costs the other direction.  Same measurement, same boot, all with
 * wakes going to the front of the queue:
 *
 *	quantum   pipe-then-reap   reap alone   total wake delay per boot
 *	5 ticks       48.4 ms         3.6 ms          100 ms
 *	2 ticks       19.9 ms         5.1 ms           50 ms
 *	1 tick        18.5 ms         8.9 ms           40 ms
 *
 * One tick buys nothing on the number that motivated the change and is the
 * worst of the three at reaping, because a slice that short cannot hold a
 * task teardown: the dying child is preempted in the middle of it and has to
 * queue again to finish, so the parent waits for two turns instead of one.
 */
#define	PREEMPT_QUANTUM_TICKS	2	/* ~20 ms at 100 Hz */

void		preempt_disable(void);
void		preempt_enable(void);
bool		preempt_is_enabled(void);

#endif /* !_SYS_SCHED_H_ */
