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
#include "gdt.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "port_internal.h"
#include "queue.h"
#include "sched.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"

/*
 * Scheduler locking discipline.
 *
 * sched_lock is held across every context switch.  The OUTgoing
 * thread acquires it (in thread_yield / thread_block / handoff_zombie)
 * and the INcoming thread is responsible for releasing it -- either
 * by falling through to the post-switch unlock at the bottom of the
 * same function (for resumed threads), or by calling spin_unlock in
 * thread_trampoline (for brand-new threads).
 *
 * This guarantees that the transition out of RUNNING and the moment
 * we leave our own stack are atomic with respect to thread_wake: a
 * waker spinning on sched_lock cannot observe us as both not-RUNNING
 * (so eligible to be woken) and still executing past our save point.
 *
 * Cooperative only -- the PIT IRQ touches no scheduler state.
 */

static struct spinlock	sched_lock = SPINLOCK_INIT("sched");

static struct thread	*runq_head;	/* (s) READY list, FIFO            */
static struct thread	*runq_tail;	/* (s)                              */
static size_t		 runq_len;	/* (s)                              */

SLIST_HEAD(zombie_list, thread);
static struct zombie_list zombie_head;	/* (s) waiting for idle to reap    */
static struct thread	*idle_thread;	/* (c)                              */
static uint64_t		 ctx_switches;	/* (s) printable counter            */
static uint64_t		 preempts;	/* (s) IRQ-driven yields            */

volatile int		 preempt_need_resched;	/* (a) */
volatile unsigned int	 preempt_quantum_used;	/* (a) */

/*
 * Preempt-disable nesting count.  Global, not per-thread: when
 * sched_lock is held across a context switch the unlock happens in a
 * different thread, so per-thread accounting underflows.  What the
 * counter conceptually represents is "is the kernel currently inside
 * any critical section" -- which is exactly the kernel-wide invariant
 * we want the PIT to consult.
 *
 * Per-CPU when we go SMP; until then a single int is fine.
 */
static volatile int	preempt_count;		/* (a) */

/*
 * Lock-free LIFO of threads queued for thread_wake by an interrupt
 * handler.  Chained through th_runq_link, which is otherwise unused
 * while the thread is BLOCKED.  Pushed atomically from any context;
 * drained in the safe windows of preempt_enable / intr_dispatch.
 */
static struct thread	*irq_wake_head;		/* (a) */

/*
 * Timed-waiters list: threads parked in mach_msg_recv_timed with a
 * non-FOREVER deadline.  Threaded through th_timed_link.  Maintained
 * under timed_lock from non-IRQ context; sched_check_timeouts runs
 * from the PIT IRQ and uses spin_trylock so a contended tick is just
 * skipped (deadline accuracy bound is one PIT period, ~10 ms).
 */
static struct spinlock	timed_lock = SPINLOCK_INIT("sched-timed");
static struct thread	*timed_head;		/* (timed_lock)            */

static void	idle_loop(void *) __attribute__((noreturn));
static struct thread *pick_next_locked(struct thread *self);
static void	enqueue_locked(struct thread *th);
static void	switch_pmap_if_needed(struct thread *self, struct thread *next);
static void	switch_user_kstack(struct thread *next);

/*
 * If the incoming thread belongs to a different task than the outgoing,
 * load the new task's CR3.  Same-task switches (e.g. between kernel
 * threads, or between two threads of the same user task) skip the
 * reload; that saves the full TLB flush x86 does on every CR3 write.
 *
 * kernel_task threads (including idle) share kernel_pmap, so kernel-
 * to-kernel switches never reload either.  Called from C BEFORE
 * thread_switch_asm because the asm swap leaves us on the incoming
 * thread's stack; either order works in practice (both pmaps share
 * the boot identity map covering all kmalloc'd kstacks), but doing it
 * here keeps "active pmap == executing thread's task pmap" true at
 * every observable point.
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
 * Re-arm the ring-transition kstack pointers for the incoming thread.
 *
 * Both tss.rsp0 (used by IRQ-from-ring-3 entry) and syscall_kernel_rsp
 * (used by the SYSCALL stub) are read by hardware at the next ring-3
 * -> ring-0 transition, so they must always point at the CURRENT
 * thread's own kstack -- otherwise a syscall taken from sh.elf after
 * we just dispatched away from clock.elf would land on clock's freed
 * kstack, scribble the syscall_frame onto recycled memory, and SYSRET
 * to a garbage RIP.
 *
 * usermode_elf_launcher sets these once when a new user task starts,
 * but the value is global -- without this re-arm on every context
 * switch the MSRs are stale the moment the scheduler picks a
 * different user thread.
 *
 * Threads without a kstack of their own (only the synthesised boot
 * thread) are skipped; they don't return to ring 3, so the MSRs are
 * never read from their context.
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
	syscall_kernel_rsp = ksp;
}

/*
 * sched_lock unlock helper invoked by the trampoline that starts every
 * brand-new thread.  Lives here so trampoline doesn't need to know
 * the lock's name.
 */
void	sched_post_switch_unlock(void);

void
sched_init(void)
{

	runq_head    = runq_tail = NULL;
	runq_len     = 0;
	SLIST_INIT(&zombie_head);
	ctx_switches = 0;
	preempts     = 0;
	preempt_need_resched = 0;
	preempt_quantum_used = 0;

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

	kprintf("sched: cooperative round-robin, idle id=%llu\n",
	    (unsigned long long)idle_thread->th_id);
}

void
sched_enqueue(struct thread *th)
{

	if (th == NULL)
		return;
	if (th == idle_thread)
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
	spin_unlock(&sched_lock);
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

void
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
	__atomic_store_n(&preempt_need_resched, 0, __ATOMIC_RELAXED);
	__atomic_store_n(&preempt_quantum_used, 0, __ATOMIC_RELAXED);

	next = pick_next_locked(self);
	if (next == NULL || next == self) {
		spin_unlock(&sched_lock);
		return;
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

	switch_pmap_if_needed(self, next);
	switch_user_kstack(next);
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save);

	/* Resumed: release the lock the OTHER thread acquired before switching. */
	spin_unlock(&sched_lock);

	/*
	 * Drain any zombies that have accumulated.  Historically reap was
	 * idle-only, on the theory that "nothing else to do" was the safest
	 * point to free thread structs.  That works when at most one user
	 * thread is yield-spinning (idle gets the empty-runq case + reaps),
	 * but breaks down with two or more concurrent yield-spinners: their
	 * FIFO round-robin keeps the runq non-empty forever, idle never
	 * runs, and zombies (e.g. an async-killed task waiting to drop its
	 * last ref) pile up indefinitely.  Reaping from every voluntary
	 * yield closes that hole at a one-spinlock-trylock cost in the
	 * common (zombie-list-empty) case.
	 */
	sched_reap_zombies();
}

void
thread_block(int reason, void *target)
{

	thread_block_release(reason, target, NULL);
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
	 * Pre-park kill check.  task_request_terminate sets t_killed
	 * BEFORE taking sched_lock for the wake fan-out, so any thread
	 * that acquires sched_lock to commit to BLOCKED after the kill
	 * was issued observes the flag here.  Without this, a thread
	 * mid-transition from RUNNING to BLOCKED could miss the wake
	 * (thread_wake bails on non-BLOCKED) and stay parked indefinitely.
	 *
	 * Drop external before thread_exit -- the caller's lock has no
	 * business surviving into sched_handoff_zombie's reacquisition
	 * of sched_lock.  kernel_task threads cannot be killed so skip
	 * the check for them entirely; t_killed is permanently false
	 * there.
	 */
	if (self->th_task != kernel_task &&
	    task_kill_pending(self->th_task)) {
		spin_unlock(&sched_lock);
		if (external != NULL)
			spin_unlock(external);
		thread_exit();
		/* NOTREACHED */
	}

	spin_lock(&self->th_lock);
	self->th_state        = THREAD_BLOCKED;
	self->th_block_reason = reason;
	self->th_block_target = target;
	spin_unlock(&self->th_lock);

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

	switch_pmap_if_needed(self, next);
	switch_user_kstack(next);
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save);

	/* Woken by thread_wake; release sched_lock and return. */
	spin_unlock(&sched_lock);

	/*
	 * Post-wake kill check.  If task_request_terminate ran while we
	 * were parked, the wake it fired broke us out of the BLOCKED
	 * state and we resumed here; t_killed is set, so retire instead
	 * of returning to the caller (whose RPC the user will never read).
	 */
	if (current_thread->th_task != kernel_task &&
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
		spin_unlock(&th->th_lock);
		spin_unlock(&sched_lock);
		return;
	}
	th->th_state        = THREAD_READY;
	th->th_block_reason = THREAD_NOT_BLOCKED;
	th->th_block_target = NULL;
	spin_unlock(&th->th_lock);

	enqueue_locked(th);
	spin_unlock(&sched_lock);
}

void
sched_post_irq_wake(struct thread *th)
{
	struct thread	*old;

	if (th == NULL)
		return;

	old = __atomic_load_n(&irq_wake_head, __ATOMIC_RELAXED);
	do {
		th->th_runq_link = old;
	} while (!__atomic_compare_exchange_n(&irq_wake_head, &old, th,
	    false, __ATOMIC_RELEASE, __ATOMIC_RELAXED));
}

void
sched_drain_irq_wakes(void)
{
	struct thread	*list, *next;

	list = __atomic_exchange_n(&irq_wake_head, NULL, __ATOMIC_ACQUIRE);
	while (list != NULL) {
		next = list->th_runq_link;
		list->th_runq_link = NULL;
		thread_wake(list);
		list = next;
	}
}

/*
 * Add `th` to the list of timed waiters.  Caller has already filled in
 * th_wake_deadline_ms; this function inserts into a small list at the
 * head so insert is O(1).  Removal scans linearly; with only a handful
 * of timed waiters at a time (typically one or two RPC clients) the
 * extra cost is negligible compared to alternatives like a sorted
 * insert that pessimises the common case.
 */
void
sched_add_timed_waiter(struct thread *th)
{

	if (th == NULL || th->th_wake_deadline_ms == 0)
		return;

	spin_lock(&timed_lock);
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
 * Walk the timed-waiters list and post an IRQ wake for any thread
 * whose deadline has elapsed.  Called from the PIT IRQ tail; uses
 * spin_trylock so a non-IRQ holder of timed_lock never deadlocks us
 * (we'll catch the deadline on the next tick at the cost of <= one
 * PIT period of latency).
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

	switch_pmap_if_needed(self, next);
	switch_user_kstack(next);
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save);

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
 * Bump the kernel-wide preempt counter.  Every spin_lock calls this
 * before the acquire spin; every spin_unlock matches it.  The counter
 * tracks how many active critical sections the kernel is currently
 * inside, summed across all threads; the PIT looks at it (not at any
 * per-thread state) when deciding whether to honour need_resched.
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
	 * Just dropped to zero.  Two pieces of deferred work may have
	 * accumulated while a critical section was held: IRQ-context
	 * wake requests queued on the lock-free list, and a PIT
	 * need_resched.  Drain the wakes first (so any newly readied
	 * thread participates in the immediate yield), then honour
	 * the resched.
	 *
	 * The preempts counter itself is bumped via a plain atomic
	 * rather than spin_lock+sched_lock so this path involves no
	 * further spin_unlock -- otherwise that unlock would re-enter
	 * preempt_enable, see need_resched still set, and recurse
	 * indefinitely.  thread_wake inside the drain does take
	 * sched_lock and re-enters via spin_unlock; bounded recursion
	 * since the second pass finds an empty wake list and a
	 * cleared need_resched (thread_yield clears it).
	 */
	sched_drain_irq_wakes();

	if (__atomic_load_n(&preempt_need_resched, __ATOMIC_RELAXED)) {
		sched_count_preempt();
		thread_yield();
	}
}

bool
preempt_is_enabled(void)
{

	return (__atomic_load_n(&preempt_count, __ATOMIC_RELAXED) == 0);
}
