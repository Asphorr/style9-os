/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "cpu.h"
#include "gdt.h"
#include "kmem.h"
#include "kprintf.h"
#include "mp.h"
#include "panic.h"
#include "pmap.h"
#include "port_internal.h"
#include "queue.h"
#include "sched.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "tsc.h"

/*
 * Scheduler locking discipline.
 *
 * sched_lock is held across every context switch.  The outgoing thread
 * acquires it (thread_yield / thread_block_release / sched_handoff_zombie)
 * and the incoming thread releases it: at the post-switch unlock of the
 * same function for a resumed thread, in thread_trampoline for a new one.
 *
 * So leaving RUNNING and leaving our own stack are one step as far as
 * thread_wake and the other CPUs can tell.  A yielding thread is back on
 * the runqueue before it has switched off its stack, and no other CPU can
 * pick it in that window, because the window is exactly the span
 * sched_lock is held for.  The same release/acquire pair publishes
 * th_rsp_save, which the switch writes inside it.
 */

static struct spinlock	sched_lock = SPINLOCK_INIT("sched");

static struct thread	*runq_head;	/* (s) READY list, FIFO            */
static struct thread	*runq_tail;	/* (s)                              */
static size_t		 runq_len;	/* (s)                              */

SLIST_HEAD(zombie_list, thread);
static struct zombie_list zombie_head;	/* (s) waiting for idle to reap    */
static uint64_t		 ctx_switches;	/* (s) printable counter            */
static uint64_t		 preempts;	/* (s) IRQ-driven yields            */

/*
 * The idle thread and the preemption state are per-CPU (machine/cpu.h),
 * aliased here.  Idle is per-CPU because a CPU with nothing to run still
 * needs a stack of its own to run nothing on.  The preempt count is
 * per-CPU rather than per-thread because sched_lock is held across a
 * switch: the thread that releases it is not the one that took it, but
 * the two are always on the same CPU.
 */
#define	idle_thread		(curcpu()->cp_idle_thread)
#define	preempt_need_resched	(curcpu()->cp_need_resched)
#define	preempt_quantum_used	(curcpu()->cp_quantum_used)
#define	preempt_count		(curcpu()->cp_preempt_count)

/*
 * Lock-free LIFO of threads an interrupt handler wants woken: pushed from
 * any context, drained in preempt_enable and at the tail of intr_dispatch.
 * Chained through th_irq_link, not th_runq_link -- a thread on this list
 * can be woken by someone else before the drain reaches it, and that wake
 * puts it on the runqueue.
 */
static struct thread	*irq_wake_head;		/* (a) */

/*
 * Threads parked with a deadline (timed Mach receives, sched_nap_ms, the
 * ATA and Darwin waits), threaded through th_timed_link under timed_lock.
 * sched_check_timeouts walks it from interrupt context with a trylock, so
 * a contended walk is skipped and a deadline can be one PIT period (10 ms)
 * late.
 */
static struct spinlock	timed_lock = SPINLOCK_INIT("sched-timed");
static struct thread	*timed_head;		/* (timed_lock)            */

/*
 * Sleep queue: every thread BLOCKED on a channel, threaded through
 * th_sleep_link.  A channel is any agreed address, conventionally a field
 * of the object waited on; sched_wakeup wakes whoever is parked on one.
 *
 * The scheduler owns the list, rather than each object keeping a waiter
 * pointer, because a sleeper can be woken or retired by parties that know
 * nothing of the object -- a kill fan-out, a driver IRQ, task teardown.
 * Only committing to BLOCKED and leaving it touch the list, and both
 * belong to the scheduler.
 *
 * One list, scanned per wakeup: few threads are ever parked at once.  If
 * that changes, hash on the channel; nothing outside the sleepq functions
 * would notice.
 */
static struct thread	*sleepq_head;		/* (sched_lock)            */

/*
 * Wake latency: time from being made READY to running, per wake (taken in
 * thread_block_release).  Where a woken thread lands in the queue decides
 * how long it waits; these say what that costs.
 */
static uint64_t		wake_lat_n;		/* (a) wakes measured      */
static uint64_t		wake_lat_sum_ms;	/* (a) total delay         */
static uint64_t		wake_lat_max_ms;	/* (a) worst one           */

/*
 * Slow wakes, named: each printed line says how many threads were queued
 * ahead and who held the CPU, which a mean cannot show.  Printing is
 * capped; counting is not.
 */
#define	WAKE_SLOW_MS		20	/* two quanta: not queueing, standing */
#define	WAKE_SLOW_MAX_LINES	20
static uint64_t		wake_slow_n;		/* (a) wakes over the bar  */
static uint64_t		wake_slow_lines;	/* (a) ...of them, printed */

static void	idle_loop(void *) __attribute__((noreturn));
static struct thread *pick_next_locked(struct thread *self);
static bool	thread_is_idle(const struct thread *th);
static void	poke_an_idle_cpu_locked(void);
static void	enqueue_locked(struct thread *th);
static void	switch_pmap_if_needed(struct thread *self, struct thread *next);
static void	switch_user_kstack(struct thread *next);

/*
 * Load the incoming task's CR3 if it is not the outgoing one's.  Same-task
 * switches -- kernel-to-kernel included, since every kernel_task thread
 * (idle too) shares kernel_pmap -- skip the reload and the TLB flush that
 * comes with it.  Done before thread_switch_asm so that the active pmap is
 * the running thread's at every observable point.
 */
static void
switch_pmap_if_needed(struct thread *self, struct thread *next)
{
	struct pmap	*old_pm;
	struct pmap	*new_pm;

	if (self == NULL || next == NULL)
		return;
	if (self->th_task == next->th_task)
		return;
	old_pm = self->th_task != NULL ? self->th_task->t_pmap : NULL;
	new_pm = next->th_task != NULL ? next->th_task->t_pmap : NULL;
	if (new_pm == NULL || new_pm == old_pm)
		return;
	pmap_activate(new_pm);
}

/*
 * Point this CPU's tss.rsp0 (interrupt from ring 3) and cp_kernel_rsp
 * (SYSCALL) at the incoming thread's kstack.  Both are read at the next
 * ring 3 -> 0 transition, and there is one pair per CPU, so they are stale
 * the moment another user thread is picked: a syscall would land on the
 * previous thread's, possibly freed, stack.  A thread with no kstack of its
 * own (the synthesised boot thread) never returns to ring 3 and is skipped.
 */
static void
switch_user_kstack(struct thread *next)
{
	uint64_t	ksp;

	if (next == NULL || next->th_kstack_base == NULL)
		return;
	ksp = (uint64_t)(uintptr_t)next->th_kstack_base +
	    next->th_kstack_size;
	tss_set_rsp0(ksp);
	cpu_set_kernel_rsp(ksp);
}

/* thread_trampoline releases sched_lock for a new thread through this. */
void	sched_post_switch_unlock(void);

void
sched_init(void)
{

	runq_head    = runq_tail = NULL;
	runq_len     = 0;
	SLIST_INIT(&zombie_head);
	ctx_switches = 0;
	preempts     = 0;
	preempt_resched_clear();
	preempt_quantum_reset();

	idle_thread = thread_create(kernel_task, idle_loop, NULL, "idle");
	if (idle_thread == NULL)
		panic("sched_init: idle thread creation failed");

	/*
	 * Idle is never on the runqueue -- pick_next_locked falls
	 * through to it whenever there's nothing else.  Mark it READY
	 * so the first dispatch's state-transition KASSERTs pass.
	 */
	spin_lock(&idle_thread->th_lock);
	idle_thread->th_state = THREAD_READY;
	spin_unlock(&idle_thread->th_lock);

	kprintf("sched: preemptive round-robin, %u-tick quantum, idle id=%llu\n",
	    (unsigned)PREEMPT_QUANTUM_TICKS,
	    (unsigned long long)idle_thread->th_id);
}

/*
 * An application processor joins the scheduler.  It needs a thread of its
 * own to switch away from and an idle thread to fall back to, and here
 * they are one: the context it is already running on (the trampoline's
 * per-CPU stack) is adopted and becomes this CPU's idler -- never on the
 * runqueue, as on the boot CPU.  The boot CPU creates its idle thread
 * instead, because its own context is the boot thread.  Either way, every
 * CPU owns exactly one idle thread.
 */
void
sched_cpu_attach(void)
{
	static const char *const	idle_names[MAXCPU] = {
		"idle", "idle1", "idle2", "idle3",
		"idle4", "idle5", "idle6", "idle7",
	};
	struct cpu			*cp;
	struct thread			*idle;

	cp = curcpu();

	idle = thread_adopt_current(kernel_task, idle_names[cp->cp_id]);
	if (idle == NULL)
		panic("sched_cpu_attach: cpu %u has no idle thread",
		    (unsigned int)cp->cp_id);

	cp->cp_idle_thread = idle;

	/* A fresh slice, nothing owed.  Already so in a zeroed block. */
	preempt_resched_clear();
	preempt_quantum_reset();
}

void
sched_cpu_idle(void)
{

	idle_loop(NULL);
	/* NOTREACHED */
}

/*
 * Is this any CPU's idle thread?  `th == idle_thread' only knows this
 * CPU's, and another CPU's idler let onto the runqueue could end up run by
 * two processors at once.  A scan rather than a flag: at most MAXCPU
 * entries, and nothing to keep in sync.
 */
static bool
thread_is_idle(const struct thread *th)
{
	unsigned int	i;

	for (i = 0; i < cpu_present_count(); i++) {
		if (cpus[i].cp_idle_thread == th)
			return (true);
	}
	return (false);
}

/*
 * Tell one idle CPU there is work.  A CPU halted in idle does not look at
 * the queue until its own next tick, so work could pile up in front of a
 * busy CPU while another slept.  One CPU, and only a truly idle one: a busy
 * CPU reaches the queue at its next schedule point anyway.
 *
 * And only with more than one thread waiting.  The caller has just set
 * need_resched on itself and runs a single woken thread at the spin_unlock
 * that follows; an IPI for that one is four APIC register accesses (each a
 * VM exit under virtualisation) under the scheduler's lock, spent to lose a
 * race.
 *
 * Called with sched_lock held; the ICR wait is bounded (lapic_icr_idle).
 * Sending after the unlock would act on a queue that had moved on.
 */
static void
poke_an_idle_cpu_locked(void)
{
	struct cpu	*me;
	unsigned int	 i;

	if (cpu_online_count() < 2)
		return;
	if (runq_len < 2)
		return;

	me = curcpu();
	for (i = 0; i < cpu_present_count(); i++) {
		if (&cpus[i] == me || cpus[i].cp_online == 0)
			continue;
		/*
		 * No idle thread means that processor has not joined the
		 * scheduler at all -- it is parked, and a reschedule there
		 * would mean nothing.
		 */
		if (cpus[i].cp_idle_thread == NULL)
			continue;
		if (cpus[i].cp_curthread != cpus[i].cp_idle_thread)
			continue;
		mp_resched(&cpus[i]);
		return;
	}
}

void
sched_enqueue(struct thread *th)
{

	if (th == NULL)
		return;
	if (thread_is_idle(th))
		return;	/* idle dispatched only by pick_next_locked */

	spin_lock(&sched_lock);

	spin_lock(&th->th_lock);
	/*
	 * Allow enqueue from INIT (first run), READY (wake racing
	 * with another wake -- harmless), or BLOCKED (transitioning).
	 * Anything else is the caller's bug.
	 */
	th->th_state = THREAD_READY;
	spin_unlock(&th->th_lock);

	enqueue_locked(th);
	poke_an_idle_cpu_locked();
	spin_unlock(&sched_lock);
}

/*
 * Put `th` on the sleep queue / take it off again.  Both require sched_lock.
 * The unlink is idempotent -- a thread that was never on the list, or has
 * already been taken off it by sched_wakeup, is left alone -- because the
 * callers cannot all tell which case they are in.
 */
static void
sleepq_link_locked(struct thread *th)
{

	th->th_sleep_link = sleepq_head;
	sleepq_head = th;
}

static void
sleepq_unlink_locked(struct thread *th)
{
	struct thread	**pp;

	for (pp = &sleepq_head; *pp != NULL; pp = &(*pp)->th_sleep_link) {
		if (*pp == th) {
			*pp = th->th_sleep_link;
			th->th_sleep_link = NULL;
			return;
		}
	}
}

static void
enqueue_locked(struct thread *th)
{

	th->th_runq_link = NULL;
	if (runq_tail == NULL) {
		runq_head = th;
		runq_tail = th;
	} else {
		runq_tail->th_runq_link = th;
		runq_tail = th;
	}
	runq_len++;
}

/*
 * Ready, and at the head of the queue.  A woken thread gave the CPU up
 * before its slice was over; at the tail, the news it was woken for would
 * wait out a quantum for every runnable thread ahead of it -- measured, that
 * was nearly all of wake latency.
 *
 * It does not starve the queue: the boost is one turn, spent by taking it.
 * Preempted or yielding, the thread goes to the tail like any other; only
 * the end of a new wait puts it first again, so nothing accumulates.
 */
static void
enqueue_first_locked(struct thread *th)
{

	th->th_runq_link = runq_head;
	runq_head = th;
	if (runq_tail == NULL)
		runq_tail = th;
	runq_len++;
}

static struct thread *
pick_next_locked(struct thread *self)
{
	struct thread	*next;

	next = runq_head;
	if (next != NULL) {
		runq_head = next->th_runq_link;
		if (runq_head == NULL)
			runq_tail = NULL;
		next->th_runq_link = NULL;
		runq_len--;
		return (next);
	}

	if (self != idle_thread)
		return (idle_thread);

	/* idle is yielding with nothing else to run -- stay on idle. */
	return (NULL);
}

bool
thread_yield(void)
{
	struct thread	*self, *next;

	self = current_thread;
	KASSERT(self != NULL, "thread_yield: no current");

	spin_lock(&sched_lock);

	/*
	 * Whether or not we actually switch, the act of calling
	 * thread_yield discharges any pending resched request and
	 * earns the caller a fresh quantum.
	 */
	preempt_resched_clear();
	preempt_quantum_reset();

	next = pick_next_locked(self);
	if (next == NULL || next == self) {
		/*
		 * Nothing else to run.  Say so: a poll loop needs to know that
		 * no time passed on this CPU.
		 */
		spin_unlock(&sched_lock);
		return (false);
	}

	if (self != idle_thread) {
		spin_lock(&self->th_lock);
		self->th_state = THREAD_READY;
		spin_unlock(&self->th_lock);
		enqueue_locked(self);
	}

	spin_lock(&next->th_lock);
	next->th_state = THREAD_RUNNING;
	spin_unlock(&next->th_lock);

	current_thread = next;
	ctx_switches++;
	curcpu()->cp_switches++;

	switch_pmap_if_needed(self, next);
	switch_user_kstack(next);
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save,
	    self->th_fpu, next->th_fpu);

	/* Resumed: release the lock the OTHER thread acquired before switching. */
	spin_unlock(&sched_lock);

	/*
	 * Reap here as well as in idle: two or more threads yield-spinning
	 * keep the runqueue non-empty, idle never runs, and zombies would
	 * pile up.  With none waiting this costs one lock round trip.
	 */
	sched_reap_zombies();

	return (true);
}

void
thread_block(int reason, void *target)
{

	thread_block_release(reason, target, NULL);
}

/*
 * Sleep for about `ms', rounded up to a timer tick (deadlines are checked
 * per tick).  For poll loops.  `while (busy) thread_yield()' only waits on
 * one CPU; with several, the thread waited for runs elsewhere and the loop
 * spins through the scheduler, taking sched_lock with interrupts off so
 * often that the CPU misses its timer ticks -- the clock stalls, and every
 * deadline in the system with it.  A poll waits in time instead.
 */
void
sched_nap_ms(uint64_t ms)
{
	struct thread	*self;

	self = current_thread;
	if (self == NULL || ms == 0)
		return;

	self->th_wake_deadline_ms = clock_uptime_ms() + ms;
	self->th_timed_out        = 0;
	sched_add_timed_waiter(self);

	/*
	 * The channel is the thread itself, which nothing else names: only
	 * the deadline or a kill can wake this.
	 */
	thread_block(THREAD_BLOCK_SLEEP, self);

	sched_remove_timed_waiter(self);
	self->th_timed_out = 0;
}

void
thread_block_release(int reason, void *target, struct spinlock *external)
{
	struct thread	*self, *next;

	self = current_thread;
	KASSERT(self != NULL, "thread_block_release: no current");
	KASSERT(self != idle_thread,
	    "thread_block_release: idle cannot block");

	spin_lock(&sched_lock);

	/*
	 * Pre-park kill check.  task_request_terminate sets t_killed before
	 * it takes sched_lock for the wake fan-out, so a thread committing
	 * to BLOCKED after the kill sees the flag here; otherwise it would
	 * miss the wake (thread_wake ignores non-BLOCKED threads) and stay
	 * parked.  kernel_task threads cannot be killed and skip the check.
	 * `external' is dropped first: it has no business surviving into
	 * sched_handoff_zombie.
	 *
	 * Not while a mutex is held.  A thread that dies owning one leaves
	 * it locked for good (mutex_unlock is owner-only), and the mutex
	 * most often slept under is fs_lock, held across the ATA waits.  So
	 * with th_mutex_depth up the kill is declined: the thread parks,
	 * finishes the operation, and retires at the next check it reaches
	 * holding nothing -- the syscall boundary, or the next bare park.
	 * That park is finite: the ATA waits have deadlines, and a mutex
	 * being acquired is released by a live owner.
	 */
	if (self->th_task != kernel_task &&
	    self->th_mutex_depth == 0 &&
	    task_kill_pending(self->th_task)) {
		/*
		 * Off the object's waiter list, if the caller linked us onto
		 * one: this path never returns, so the caller's own detach
		 * never runs.  Done here, not in thread_exit, because
		 * `external' -- the lock that list lives under -- is still
		 * held, so no extractor can be mid-pop against us.
		 */
		if (self->th_wait_qlock != NULL) {
			KASSERT(self->th_wait_qlock == external,
			    "thread_block_release: noted on one lock's "
			    "list, parking under another");
			thread_wait_unbind_locked(self);
		}
		spin_unlock(&sched_lock);
		if (external != NULL)
			spin_unlock(external);
		thread_exit();
		/* NOTREACHED */
	}

	/*
	 * A wake that arrived before this sleep: thread_wake found us still
	 * running and left th_wake_pending.  Read under sched_lock, the lock
	 * the waker wrote it under, and before committing to BLOCKED.
	 * Declining to sleep is a spurious wakeup; every caller re-tests in
	 * a loop.
	 */
	if (self->th_wake_pending != 0) {
		self->th_wake_pending = 0;
		if (external != NULL)
			spin_unlock(external);
		spin_unlock(&sched_lock);
		return;
	}

	spin_lock(&self->th_lock);
	self->th_state        = THREAD_BLOCKED;
	self->th_block_reason = reason;
	self->th_block_target = target;
	spin_unlock(&self->th_lock);

	/*
	 * Linked only now, after the kill check: that is the one exit that
	 * bypasses thread_wake, and it would leave a soon-freed thread on the
	 * list.  A NULL target names no channel and stays off it.
	 */
	if (target != NULL)
		sleepq_link_locked(self);

	/*
	 * Drop the caller's lock with sched_lock held.  Any thread_wake
	 * fired now must first acquire sched_lock; by that time we have
	 * already committed to BLOCKED so the wake observes a real
	 * BLOCKED -> READY transition rather than losing the signal.
	 */
	if (external != NULL)
		spin_unlock(external);

	next = pick_next_locked(self);
	KASSERT(next != NULL,
	    "thread_block_release: pick_next returned NULL "
	    "(idle missing)");

	spin_lock(&next->th_lock);
	next->th_state = THREAD_RUNNING;
	spin_unlock(&next->th_lock);

	current_thread = next;
	ctx_switches++;
	curcpu()->cp_switches++;

	switch_pmap_if_needed(self, next);
	switch_user_kstack(next);
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save,
	    self->th_fpu, next->th_fpu);

	/* Woken by thread_wake; release sched_lock and return. */
	spin_unlock(&sched_lock);

	/* How long that wake took to reach the CPU.  See wake_lat_*. */
	if (self->th_wake_ms != 0) {
		uint64_t	delay;

		delay = clock_uptime_ms() - self->th_wake_ms;
		self->th_wake_ms = 0;
		if (delay >= WAKE_SLOW_MS) {
			wake_slow_n++;
			if (wake_slow_lines < WAKE_SLOW_MAX_LINES) {
				wake_slow_lines++;
				kprintf("sched: slow wake -- %s waited %llu ms "
				    "behind %u thread(s), %s had the CPU\n",
				    self->th_name != NULL ? self->th_name : "?",
				    (unsigned long long)delay,
				    (unsigned int)self->th_wake_qlen,
				    self->th_wake_hog != NULL ?
				    self->th_wake_hog : "?");
			}
		}
		wake_lat_n++;
		wake_lat_sum_ms += delay;
		if (delay > wake_lat_max_ms)
			wake_lat_max_ms = delay;
	}

	/*
	 * Post-wake kill check: a kill while we were parked is what woke
	 * us, so retire rather than return to a caller whose result nobody
	 * will read.  Same mutex clause as before parking -- a holder
	 * returns, releases in the ordinary course, and dies at the
	 * boundary; the kill's wake was then merely spurious.
	 */
	if (current_thread->th_task != kernel_task &&
	    current_thread->th_mutex_depth == 0 &&
	    task_kill_pending(current_thread->th_task))
		thread_exit();
	/* NOTREACHED if killed */
}

void
thread_wake(struct thread *th)
{

	if (th == NULL)
		return;

	spin_lock(&sched_lock);
	spin_lock(&th->th_lock);
	if (th->th_state != THREAD_BLOCKED) {
		/*
		 * Not blocked is not the same as not listening.  A READY or
		 * RUNNING thread may be between deciding to sleep and
		 * committing to it -- a window with no bound on SMP -- and a
		 * wake that did nothing there would be lost for good.  So
		 * leave a note: thread_block_release reads it under
		 * sched_lock and declines to sleep, and the caller re-tests
		 * its condition.
		 */
		if (th->th_state == THREAD_READY ||
		    th->th_state == THREAD_RUNNING)
			th->th_wake_pending = 1;
		spin_unlock(&th->th_lock);
		spin_unlock(&sched_lock);
		return;
	}
	th->th_wake_pending = 0;
	th->th_state        = THREAD_READY;
	th->th_block_reason = THREAD_NOT_BLOCKED;
	th->th_block_target = NULL;
	th->th_wake_ms      = clock_uptime_ms();
	th->th_wake_hog     = current_thread != NULL ?
	    current_thread->th_name : "?";
	th->th_wake_qlen    = runq_len;
	spin_unlock(&th->th_lock);

	sleepq_unlink_locked(th);
	enqueue_first_locked(th);
	/*
	 * Ask for a reschedule.  Without it the woken thread waits until the
	 * running one yields or uses up its quantum; with it, measured over a
	 * boot, mean wake latency went from 6 ms to 0.  It is also what lets
	 * sleeping beat polling: a sleeper gets one look and needs the CPU to
	 * take it.  Honoured at the next safe point -- the tail of
	 * intr_dispatch, or the preempt_enable the spin_unlock below reaches
	 * at once.
	 */
	preempt_need_resched = 1;
	poke_an_idle_cpu_locked();
	spin_unlock(&sched_lock);
}

/*
 * Wake every thread of `task' asleep on a channel, and no others; returns
 * how many.  For posting a signal: a thread parked inside a syscall would
 * otherwise never see the pending bit, and read(2) on a pipe must be
 * interruptible.  Threads blocked for any other reason (a Mach receive)
 * are left alone -- a receive is not interruptible here, and would only
 * re-check an empty queue and park again.
 */
uint32_t
sched_wake_sleepers_of(struct task *task)
{
	struct thread	*th;
	struct thread	*next;
	uint32_t	 n;

	if (task == NULL)
		return (0);

	n = 0;
	spin_lock(&sched_lock);
	for (th = sleepq_head; th != NULL; th = next) {
		next = th->th_sleep_link;
		if (th->th_task != task)
			continue;
		if (th->th_block_reason != THREAD_BLOCK_SLEEP)
			continue;
		spin_lock(&th->th_lock);
		if (th->th_state != THREAD_BLOCKED) {
			spin_unlock(&th->th_lock);
			continue;
		}
		th->th_state        = THREAD_READY;
		th->th_block_reason = THREAD_NOT_BLOCKED;
		th->th_block_target = NULL;
		th->th_wake_ms      = clock_uptime_ms();
		th->th_wake_hog     = current_thread != NULL ?
		    current_thread->th_name : "?";
		th->th_wake_qlen    = runq_len;
		spin_unlock(&th->th_lock);

		sleepq_unlink_locked(th);
		enqueue_first_locked(th);
		n++;
	}
	if (n != 0) {
		preempt_need_resched = 1;
		poke_an_idle_cpu_locked();
	}
	spin_unlock(&sched_lock);
	return (n);
}

/*
 * Make ready every thread parked on `chan'; returns how many.
 *
 * The waiter tests its condition and parks under one lock, handing that
 * lock to thread_block_release; the waker changes the condition and then
 * calls this, before or after dropping its own lock.  A wake is a hint,
 * never a promise: every caller parks inside for (;;) and re-tests.
 */
uint32_t
sched_wakeup(void *chan)
{
	struct thread	*th;
	struct thread	*next;
	uint32_t	 n;

	KASSERT(chan != NULL, "sched_wakeup: NULL channel would wake nobody");

	n = 0;
	spin_lock(&sched_lock);
	/* Read the link before waking: the unlink clears it. */
	for (th = sleepq_head; th != NULL; th = next) {
		next = th->th_sleep_link;
		if (th->th_block_target != chan)
			continue;
		spin_lock(&th->th_lock);
		if (th->th_state != THREAD_BLOCKED) {
			spin_unlock(&th->th_lock);
			continue;
		}
		th->th_state        = THREAD_READY;
		th->th_block_reason = THREAD_NOT_BLOCKED;
		th->th_block_target = NULL;
		th->th_wake_ms      = clock_uptime_ms();
		th->th_wake_hog     = current_thread != NULL ?
		    current_thread->th_name : "?";
		th->th_wake_qlen    = runq_len;
		spin_unlock(&th->th_lock);

		sleepq_unlink_locked(th);
		enqueue_first_locked(th);
		n++;
	}
	/* Same reasoning as thread_wake: a wake without one is a wake late. */
	if (n != 0) {
		preempt_need_resched = 1;
		poke_an_idle_cpu_locked();
	}
	spin_unlock(&sched_lock);
	return (n);
}

void
sched_post_irq_wake(struct thread *th)
{
	struct thread	*old;

	if (th == NULL)
		return;

	/*
	 * Queued at most once.  Two parties can post the same thread at
	 * once from different CPUs (a deadline expiring an ATA wait while
	 * the drive's interrupt completes it), and a second push would
	 * overwrite th_irq_link while the list runs through it: a self-loop
	 * the drain spins on forever, or every wake behind it lost.  The
	 * loser just leaves -- one queued wake carries any news, since every
	 * sleeper re-tests.
	 */
	if (__atomic_exchange_n(&th->th_irq_queued, 1, __ATOMIC_ACQ_REL) != 0)
		return;

	/*
	 * Held from here until the drain's thread_wake: in between, the
	 * thread can be woken by someone else, exit and be reaped.  The hold
	 * keeps the reaper off the body until the wake is delivered.
	 */
	thread_hold(th);

	old = __atomic_load_n(&irq_wake_head, __ATOMIC_RELAXED);
	do {
		th->th_irq_link = old;
	} while (!__atomic_compare_exchange_n(&irq_wake_head, &old, th,
	    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

void
sched_drain_irq_wakes(void)
{
	struct thread	*list, *next;

	list = __atomic_exchange_n(&irq_wake_head, NULL, __ATOMIC_ACQUIRE);
	while (list != NULL) {
		next = list->th_irq_link;
		list->th_irq_link = NULL;
		/*
		 * Mark off before the wake, so news arriving mid-wake queues
		 * the thread again instead of being dropped; hold off after,
		 * so it cannot be freed in between.  A wake delivered twice is
		 * spurious and harmless; a wake dropped is a hang.
		 */
		__atomic_store_n(&list->th_irq_queued, 0, __ATOMIC_RELEASE);
		thread_wake(list);
		thread_unhold(list);
		list = next;
	}
}

/* Is `th` already on the deadline list?  Caller holds timed_lock. */
static bool
timed_present_locked(const struct thread *th)
{
	const struct thread	*cur;

	for (cur = timed_head; cur != NULL; cur = cur->th_timed_link)
		if (cur == th)
			return (true);
	return (false);
}

/*
 * Add `th' (th_wake_deadline_ms already set) to the timed waiters.
 * Insert at the head, remove by scan: the list is a handful long, and a
 * sorted insert would cost more than it saves.
 */
void
sched_add_timed_waiter(struct thread *th)
{

	if (th == NULL || th->th_wake_deadline_ms == 0)
		return;

	spin_lock(&timed_lock);
	/*
	 * Twice on this list cuts it: the push overwrites the link the
	 * thread already holds, and everyone behind it loses their deadline
	 * -- silently, since a waiter never woken looks like one still
	 * waiting.  The scan turns that into a panic naming the thread.
	 */
	KASSERT(!timed_present_locked(th),
	    "sched_add_timed_waiter: already on the deadline list");
	th->th_timed_out  = 0;
	th->th_timed_link = timed_head;
	timed_head        = th;
	spin_unlock(&timed_lock);
}

void
sched_remove_timed_waiter(struct thread *th)
{
	struct thread	**pp;

	if (th == NULL)
		return;

	spin_lock(&timed_lock);
	pp = &timed_head;
	while (*pp != NULL) {
		if (*pp == th) {
			*pp = th->th_timed_link;
			th->th_timed_link        = NULL;
			th->th_wake_deadline_ms  = 0;
			break;
		}
		pp = &(*pp)->th_timed_link;
	}
	spin_unlock(&timed_lock);
}

/*
 * Post an IRQ wake for every timed waiter whose deadline has passed.
 * Called from intr_dispatch after each 8259 interrupt, so at least at the
 * PIT's rate.  Trylock only: if another CPU holds timed_lock, this tick
 * is skipped rather than spent spinning in interrupt context.
 */
void
sched_check_timeouts(void)
{
	struct thread	**pp;
	struct thread	 *th;
	uint64_t	  now;

	if (!spin_trylock(&timed_lock))
		return;

	now = clock_uptime_ms();
	pp  = &timed_head;
	while (*pp != NULL) {
		th = *pp;
		if (th->th_wake_deadline_ms != 0 &&
		    th->th_wake_deadline_ms <= now) {
			*pp                      = th->th_timed_link;
			th->th_timed_link        = NULL;
			th->th_wake_deadline_ms  = 0;
			th->th_timed_out         = 1;
			sched_post_irq_wake(th);
		} else {
			pp = &(*pp)->th_timed_link;
		}
	}
	spin_unlock(&timed_lock);
}

void
sched_handoff_zombie(struct thread *self)
{
	struct thread	*next;

	spin_lock(&sched_lock);

	SLIST_INSERT_HEAD(&zombie_head, self, th_zombie_link);

	next = pick_next_locked(self);
	KASSERT(next != NULL,
	    "sched_handoff_zombie: no runnable thread");

	spin_lock(&next->th_lock);
	next->th_state = THREAD_RUNNING;
	spin_unlock(&next->th_lock);

	current_thread = next;
	ctx_switches++;
	curcpu()->cp_switches++;

	switch_pmap_if_needed(self, next);
	switch_user_kstack(next);
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save,
	    self->th_fpu, next->th_fpu);

	/* NOTREACHED -- self is zombie. */
	panic("sched_handoff_zombie: returned from switch");
}

void
sched_post_switch_unlock(void)
{

	spin_unlock(&sched_lock);
}

void
sched_reap_zombies(void)
{
	struct zombie_list	 drain;
	struct thread		*next;
	struct task		*t;
	struct thread		*z;

	SLIST_INIT(&drain);

	spin_lock(&sched_lock);
	SLIST_SWAP(&drain, &zombie_head, thread);
	spin_unlock(&sched_lock);

	SLIST_FOREACH_SAFE(z, &drain, th_zombie_link, next) {
		unsigned	exi;

		/*
		 * Still held: a waker has popped this thread and not yet
		 * delivered its thread_wake (th_wake_hold, kern/thread.h).
		 * Freeing it now is the use-after-free the hold prevents, so
		 * requeue it -- a hold only spans a pop and a wake.
		 */
		if (__atomic_load_n(&z->th_wake_hold, __ATOMIC_ACQUIRE) != 0) {
			spin_lock(&sched_lock);
			SLIST_INSERT_HEAD(&zombie_head, z, th_zombie_link);
			spin_unlock(&sched_lock);
			continue;
		}

		t = z->th_task;

		/*
		 * Release SEND refs the thread held on its per-thread
		 * exception ports.  No t_lock needed: a zombie has no
		 * other reachers (thread_exit already detached us from
		 * the runqueue and any waitq).
		 */
		for (exi = 0; exi < EXC_TYPE_COUNT; exi++) {
			if (z->th_exc_ports[exi] != NULL) {
				port_deref(z->th_exc_ports[exi],
				    MACH_PORT_RIGHT_SEND);
				z->th_exc_ports[exi] = NULL;
			}
		}

		/* Detached from any waiter list: checked, not assumed. */
		KASSERT(z->th_wait_qlock == NULL,
		    "reap: thread still noted on an object's waiter list");

		if (z->th_kstack_owned && z->th_kstack_base != NULL)
			kfree(z->th_kstack_base);

		task_detach_thread(t, z);
		kfree(z);
	}
}

/*
 * Idle thread body.  On every spin: reap any zombies, then either
 * yield (if there is real work waiting) or sti/hlt until the next
 * interrupt nudges the system.  cli on the way out so the next iter
 * of the loop reaps zombies without an IRQ landing on a half-set-up
 * iteration.
 */
static void
idle_loop(void *arg)
{

	(void)arg;
	for (;;) {
		sched_reap_zombies();

		spin_lock(&sched_lock);
		if (runq_head != NULL) {
			spin_unlock(&sched_lock);
			thread_yield();
			continue;
		}
		spin_unlock(&sched_lock);

		__asm__ __volatile__ ("sti; hlt; cli");
	}
}

/* ---- proving the work goes to more than one processor ---------------- */

/*
 * Did work run anywhere but here?  Eight probes each set the bit of the
 * CPU they are running on, read from that CPU's own block -- only code on
 * CPU 3 can set bit 3 -- so a scheduler that kept everything on the boot
 * CPU, or a CPU that joined and wedged, cannot pass.  Each probe lives
 * 100 ms of TSC (wall) time, so all eight end together however many CPUs
 * share them.
 */
#define	SMP_TEST_THREADS	8
#define	SMP_TEST_US		100000
#define	SMP_TEST_WAIT_MS	5000

static volatile uint32_t	smp_seen;	/* (a) cpu ids, as bits    */
static volatile uint32_t	smp_done;	/* (a) probes that ended   */

static void
smp_probe(void *arg)
{
	uint64_t	t0;

	(void)arg;

	t0 = tsc_read();
	while (tsc_to_us(tsc_read() - t0) < SMP_TEST_US) {
		__atomic_fetch_or(&smp_seen, 1u << cpu_id(), __ATOMIC_RELAXED);
		__asm__ __volatile__ ("pause");
	}
	__atomic_fetch_add(&smp_done, 1, __ATOMIC_RELAXED);
}

void
sched_smp_selftest(void)
{
	struct thread	*th;
	uint64_t	 deadline;
	uint32_t	 seen;
	unsigned int	 nbits;
	unsigned int	 done;
	unsigned int	 i;

	if (cpu_online_count() < 2) {
		kprintf("sched-smp: one processor is running -- there is "
		    "nowhere else for the work to go\n");
		return;
	}

	__atomic_store_n(&smp_seen, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&smp_done, 0, __ATOMIC_RELAXED);

	for (i = 0; i < SMP_TEST_THREADS; i++) {
		th = thread_create(kernel_task, smp_probe, NULL, "smp-probe");
		if (th == NULL) {
			kprintf("sched-smp: FAIL no memory for probe %u\n", i);
			return;
		}
		thread_start(th);
	}

	deadline = clock_uptime_ms() + SMP_TEST_WAIT_MS;
	for (;;) {
		done = __atomic_load_n(&smp_done, __ATOMIC_RELAXED);
		if (done >= SMP_TEST_THREADS)
			break;
		if (clock_uptime_ms() > deadline) {
			kprintf("sched-smp: FAIL only %u of %u probes finished "
			    "in %u ms\n", done, (unsigned int)SMP_TEST_THREADS,
			    (unsigned int)SMP_TEST_WAIT_MS);
			return;
		}
		thread_yield();
	}

	seen  = __atomic_load_n(&smp_seen, __ATOMIC_RELAXED);
	nbits = 0;
	for (i = 0; i < MAXCPU; i++) {
		if ((seen & (1u << i)) != 0)
			nbits++;
	}

	if (nbits < 2) {
		kprintf("sched-smp: FAIL %u thread(s) all ran on one "
		    "processor (mask 0x%x)\n", (unsigned int)SMP_TEST_THREADS,
		    (unsigned int)seen);
		return;
	}

	kprintf("sched-smp: PASS -- %u thread(s) ran on %u of the %u "
	    "processor(s) that are up (mask 0x%x), and each of them said so "
	    "out of its own CPU's block\n", (unsigned int)SMP_TEST_THREADS,
	    nbits, cpu_online_count(), (unsigned int)seen);
}

/* ---- introspection --------------------------------------------------- */

void
sched_print(void)
{
	struct thread	*cur;

	spin_lock(&sched_lock);
	kprintf("sched: %zu in runq, %llu context switches\n",
	    runq_len, (unsigned long long)ctx_switches);
	for (cur = runq_head; cur != NULL; cur = cur->th_runq_link) {
		spin_unlock(&sched_lock);
		thread_print(cur);
		spin_lock(&sched_lock);
	}
	if (current_thread != NULL) {
		spin_unlock(&sched_lock);
		kprintf("current: ");
		thread_print(current_thread);
		spin_lock(&sched_lock);
	}
	spin_unlock(&sched_lock);
}

size_t
sched_runq_len(void)
{
	size_t	n;

	spin_lock(&sched_lock);
	n = runq_len;
	spin_unlock(&sched_lock);
	return (n);
}

void
sched_wake_latency_print(void)
{

	if (wake_lat_n == 0)
		return;
	kprintf("sched: %llu wake(s) reached the CPU in %llu ms total, "
	    "mean %llu ms, worst %llu ms\n",
	    (unsigned long long)wake_lat_n,
	    (unsigned long long)wake_lat_sum_ms,
	    (unsigned long long)(wake_lat_sum_ms / wake_lat_n),
	    (unsigned long long)wake_lat_max_ms);
	kprintf("sched: %llu of them waited %d ms or more%s\n",
	    (unsigned long long)wake_slow_n, WAKE_SLOW_MS,
	    wake_slow_n > wake_slow_lines ? " (not all named above)" : "");
}

uint64_t
sched_context_switches(void)
{
	uint64_t	v;

	spin_lock(&sched_lock);
	v = ctx_switches;
	spin_unlock(&sched_lock);
	return (v);
}

uint64_t
sched_preempts(void)
{

	return (__atomic_load_n(&preempts, __ATOMIC_RELAXED));
}

void
sched_count_preempt(void)
{

	__atomic_fetch_add(&preempts, 1, __ATOMIC_RELAXED);
}

/*
 * This CPU's preempt count: every spin_lock bumps it before spinning and
 * every spin_unlock drops it.  A reschedule is honoured only at zero.
 */
void
preempt_disable(void)
{

	__atomic_fetch_add(&preempt_count, 1, __ATOMIC_SEQ_CST);
}

void
preempt_enable(void)
{
	int	n;

	n = __atomic_sub_fetch(&preempt_count, 1, __ATOMIC_SEQ_CST);
	KASSERT(n >= 0, "preempt_enable: count underflow");

	if (n != 0)
		return;

	/*
	 * Just dropped to zero: settle what was deferred meanwhile.  IRQ-
	 * posted wakes first, so a thread they ready takes part in the
	 * yield; then any owed reschedule.  sched_count_preempt is a plain
	 * atomic because a spin_unlock here would re-enter this function
	 * with need_resched still set.  thread_wake in the drain does
	 * re-enter through its spin_unlock; that recursion ends, because
	 * thread_yield clears the request.
	 */
	sched_drain_irq_wakes();

	/*
	 * Only with a thread to switch away from.  An AP released from
	 * parking takes locks (kmalloc, for its idle thread) before it has a
	 * current thread, and the release IPI left need_resched set on it.
	 * intr_dispatch's tail makes the same test for the same window.
	 */
	if (preempt_resched_wanted() && current_thread != NULL) {
		sched_count_preempt();
		thread_yield();
	}
}

bool
preempt_is_enabled(void)
{

	return (__atomic_load_n(&preempt_count, __ATOMIC_RELAXED) == 0);
}

/*
 * The quantum and the resched request: per-CPU, and still atomic, because
 * the timer interrupt and the thread it interrupted are two writers on one
 * CPU.
 */
bool
preempt_quantum_tick(void)
{
	unsigned int	q;

	q = __atomic_add_fetch(&preempt_quantum_used, 1, __ATOMIC_RELAXED);
	return (q >= PREEMPT_QUANTUM_TICKS);
}

void
preempt_quantum_reset(void)
{

	__atomic_store_n(&preempt_quantum_used, 0, __ATOMIC_RELAXED);
}

void
preempt_resched_request(void)
{

	__atomic_store_n(&preempt_need_resched, 1, __ATOMIC_RELAXED);
}

bool
preempt_resched_wanted(void)
{

	return (__atomic_load_n(&preempt_need_resched, __ATOMIC_RELAXED) != 0);
}

void
preempt_resched_clear(void)
{

	__atomic_store_n(&preempt_need_resched, 0, __ATOMIC_RELAXED);
}
