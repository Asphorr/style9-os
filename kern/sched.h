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
 * Round-robin scheduler with timer-driven preemption.  Threads also
 * yield explicitly via thread_yield, or implicitly via thread_block
 * (e.g. mach_msg_recv_block on an empty port).  A timer interrupt debits
 * the current thread's PREEMPT_QUANTUM_TICKS slice and requests a
 * reschedule on expiry, honoured at the end of intr_dispatch or at the
 * next preempt_enable that drops the count to zero.
 *
 * The timer is this CPU's local APIC where there is one, the PIT where
 * there is not.  Both tick at the same rate, so the quantum stays a
 * number of ticks.
 *
 * Lock order: sched_lock is taken after any other lock; only th_lock
 * nests inside it.
 */

void		sched_init(void);

/*
 * Bring the calling application processor into the scheduler: the context
 * it is already running becomes its idle thread.  The boot CPU gets its
 * idle thread from sched_init instead, since its own context is the boot
 * thread.
 */
void		sched_cpu_attach(void);

/* Then run this CPU's idle loop: reap, yield if there is work, else hlt. */
void		sched_cpu_idle(void) __attribute__((noreturn));

/*
 * Eight threads of 100 ms each, each setting the bit of the CPU it runs
 * on: proof that a thread queued anywhere can run anywhere.
 */
void		sched_smp_selftest(void);
void		sched_enqueue(struct thread *);

/*
 * thread_yield: put current at the tail of the runqueue and switch to the
 * head; with the queue empty, a non-idle caller switches to this CPU's
 * idle thread.  Returns when the caller is rescheduled: true if it
 * switched, false if there was nothing to switch to (which, given the
 * idle fallback, only the idle thread sees).
 *
 * A yield is not a way of waiting on SMP: what a poll loop waits for is
 * usually running on another CPU, not queued behind the caller, so the
 * yield comes back almost at once.  Poll with sched_nap_ms (poll_turn in
 * lib/style9_sys.c from ring 3).
 */
bool		thread_yield(void);

/*
 * Give up the CPU for about `ms' of real time, to the resolution of a
 * timer tick (deadlines are checked per tick).  Every `while (not yet)'
 * poll loop in the kernel must use this rather than thread_yield: on SMP a
 * yield loop spins through sched_lock with interrupts off often enough to
 * miss timer ticks, which stalls the clock and every deadline with it.
 */
void		sched_nap_ms(uint64_t ms);

/*
 * thread_block: mark current BLOCKED, record reason and target, then
 * pick the next runnable thread.  The caller is expected to be
 * embedded inside a synchronisation primitive that will arrange for
 * thread_wake() to be called later.  Returns when woken, possibly
 * spuriously: callers re-test in a loop.  A thread whose task is killed
 * retires here instead of returning, unless it holds a mutex.
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
 * thread_wake: transition `th` from BLOCKED to READY and enqueue it at the
 * head of the queue, since a thread that blocked gave its slice up unused.
 * Also asks for a reschedule, honoured at the next safe point rather than
 * when whoever is running happens to stop.  On a READY or RUNNING thread
 * it leaves th_wake_pending instead, so a thread about to park does not
 * lose the wake; on any other state it does nothing.
 */
void		thread_wake(struct thread *);

/*
 * sched_wakeup: wake every thread parked on the channel `chan` -- that is,
 * every thread that passed `chan` as thread_block's target and has not been
 * woken since.  Returns how many there were.
 *
 * A channel is just an agreed address, conventionally a field of the object
 * being waited on: a pipe's byte count for readers waiting on data, its read
 * position for writers waiting on room -- two waits, two channels.
 *
 * The waiter must test the condition and park under one lock, handing that
 * lock to thread_block_release rather than dropping it first:
 *
 *	for (;;) {
 *		spin_lock(&obj->lock);
 *		if (ready(obj)) { ...; spin_unlock(&obj->lock); break; }
 *		thread_block_release(THREAD_BLOCK_SLEEP, &obj->field,
 *		    &obj->lock);
 *	}
 *
 * Unlock-then-block is the lost wakeup: an event in the gap wakes nobody.
 * A wake is also only a hint -- re-test in the loop, since the bytes may be
 * gone by the time this thread runs.
 */
uint32_t	sched_wakeup(void *chan);

/*
 * sched_wake_sleepers_of: wake every thread of `task` asleep on a channel,
 * leaving threads blocked on a port alone; returns how many.  This is how
 * a posted signal reaches a thread already inside a blocking syscall.
 */
struct task;
uint32_t	sched_wake_sleepers_of(struct task *task);

/*
 * IRQ-safe deferred wake.
 *
 * sched_post_irq_wake is callable from interrupt context: it pushes `th'
 * on a lock-free list and returns without touching sched_lock.  Drivers
 * use it to wake a thread from the IRQ that delivered its event.
 *
 * sched_drain_irq_wakes runs thread_wake on every entry.  It is called
 * at the tail of intr_dispatch (before the preempt point) and of
 * preempt_enable (when the count drops to zero), both with no spinlock
 * held, so thread_wake's sched_lock cannot recurse onto a lock the
 * interrupted thread held.
 */
void		sched_post_irq_wake(struct thread *);
void		sched_drain_irq_wakes(void);

/*
 * Timed-block plumbing.  A thread to be woken by an event or a deadline,
 * whichever comes first, sets th_wake_deadline_ms (absolute
 * clock_uptime_ms()) and calls sched_add_timed_waiter -- at most once --
 * before parking via thread_block_release.  On wake it must call
 * sched_remove_timed_waiter (idempotent), then read th_timed_out to tell
 * "event arrived" from "deadline expired".
 *
 * sched_check_timeouts runs from intr_dispatch after each 8259 interrupt
 * and posts IRQ wakes for expired entries.
 */
void		sched_add_timed_waiter(struct thread *);
void		sched_remove_timed_waiter(struct thread *);
void		sched_check_timeouts(void);

/*
 * sched_handoff_zombie: called from thread_exit().  Adds the thread to
 * the zombie list and switches away.  Never returns.
 */
void		sched_handoff_zombie(struct thread *)
		    __attribute__((noreturn));

/*
 * Free the struct and kstack of every thread that has exited.  Called by
 * the idle loop and after each thread_yield that switched; tests call it
 * directly for a deterministic cleanup point.
 */
void		sched_reap_zombies(void);

/*
 * CPU time (th_utime and th_stime in struct thread).  sched_cpu_to_sys
 * charges the running thread's time since its mark as user time and goes
 * into a syscall; sched_cpu_to_user charges it as system time and leaves
 * for ring 3.  sched_cpu_charge charges it where it stands, so that a
 * reader of the current thread's counters sees them up to now.
 */
void		sched_cpu_to_sys(void);
void		sched_cpu_to_user(void);
void		sched_cpu_charge(void);

/*
 * Diagnostics.
 */
void		sched_print(void);
size_t		sched_runq_len(void);
uint64_t	sched_context_switches(void);
uint64_t	sched_preempts(void);
void		sched_wake_latency_print(void);

/*
 * Tally one preempt event, from both the inline (intr_dispatch) and the
 * deferred (preempt_enable) path.
 */
void		sched_count_preempt(void);

/*
 * Preemption primitives.
 *
 * preempt_disable / preempt_enable bracket critical sections that must not
 * be preempted; spin_lock / spin_unlock call them.  The count is per-CPU,
 * not per-thread: sched_lock is held across a context switch, so the
 * thread that releases it is not the one that took it, but both are on
 * the same CPU.
 *
 * The resched request and the quantum tally are per-CPU too, reached only
 * through the five calls below: a CPU owes its own reschedule and no
 * other.  preempt_quantum_tick debits one timer tick against whatever is
 * running here and returns true when the slice is spent;
 * preempt_resched_request then asks.  The ask is honoured at the schedule
 * point -- the end of intr_dispatch, or the next preempt_enable that drops
 * the count to zero -- so a critical section that ends later still owes
 * the reschedule.  thread_yield clears both, so a thread that gives the
 * CPU up voluntarily hands its successor a fresh slice.
 */
bool		preempt_quantum_tick(void);
void		preempt_quantum_reset(void);
void		preempt_resched_request(void);
bool		preempt_resched_wanted(void);
void		preempt_resched_clear(void);

/*
 * How long a thread may hold the CPU before a timer takes it away.  The
 * slice is also the wait of whoever is queued behind, often a thread the
 * others wait for: a child that must reach _exit before its parent's wait4
 * returns, a writer before its reader sees a byte.  Measured, with wakes
 * going to the head of the queue:
 *
 *	quantum   pipe-then-reap   reap alone   total wake delay per boot
 *	5 ticks       48.4 ms         3.6 ms          100 ms
 *	2 ticks       19.9 ms         5.1 ms           50 ms
 *	1 tick        18.5 ms         8.9 ms           40 ms
 *
 * One tick gains almost nothing on pipe-then-reap and is worst at reaping:
 * a task teardown does not fit in the slice, so the dying child is
 * preempted and queues again to finish.
 *
 * Measured with the PIT debiting the slice.  The APIC timer is programmed
 * at the PIT's rate so the constant keeps its meaning; lapic_timer_report
 * prints the slice the delivered ticks imply.
 */
#define	PREEMPT_QUANTUM_TICKS	2	/* ~20 ms at 100 Hz */

void		preempt_disable(void);
void		preempt_enable(void);
bool		preempt_is_enabled(void);

#endif /* !_SYS_SCHED_H_ */
