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

/*
 * Sleep queue: every thread currently BLOCKED on a CHANNEL, threaded through
 * th_sleep_link.  A channel is any address two pieces of code agree to name --
 * classically a field of the object being waited on -- and sched_wakeup wakes
 * whoever is parked on one.
 *
 * WHY THE SCHEDULER OWNS THIS LIST rather than each object keeping its own
 * waiter pointer: a waiter can be woken by things that know nothing about what
 * it was waiting for.  A kill fan-out wakes every thread of a task; a driver
 * IRQ wakes one by name; a thread whose task is killed while it is parked
 * retires without the object ever hearing about it.  An object holding a
 * thread pointer has to be told about all three, and the console's single
 * waiter slot -- which needed an explicit hook in task teardown to avoid
 * calling thread_wake on freed memory -- is what that costs.  Here the only
 * two moments a thread enters or leaves the list are the two the scheduler
 * already owns: committing to BLOCKED, and leaving it.  Nothing else can get
 * it wrong, because nothing else touches the list.
 *
 * A single list, walked per wakeup, rather than a hash of queues.  The whole
 * system has a handful of threads and at most a couple of them are parked at
 * once, so a scan is cheaper than the buckets it would take to avoid it; when
 * that stops being true the fix is a hash keyed on the channel, and nothing
 * outside these three functions would have to change.
 */
static struct thread	*sleepq_head;		/* (sched_lock)            */

/*
 * How long a wake takes to become a RUN.  Making a thread READY is not making
 * it run: something else holds the CPU, and where in the queue the woken thread
 * lands decides how long it waits.  A waiter that polls is always in that queue
 * already and so is never far from its turn; one that sleeps rejoins from
 * wherever it is put, and whether that is cheaper depends on numbers rather
 * than on argument.
 *
 * Measured here rather than reasoned about because the reasoning was wrong
 * once: parking three waits made the news arrive TEN TIMES LATER, and nothing
 * short of this counter said so.
 */
static uint64_t		wake_lat_n;		/* (a) wakes measured      */
static uint64_t		wake_lat_sum_ms;	/* (a) total delay         */
static uint64_t		wake_lat_max_ms;	/* (a) worst one           */

/*
 * ...and WHY, for the ones that were slow, which the totals above cannot say.
 *
 * A mean of zero over twenty thousand wakes hides the handful that took fifty
 * milliseconds, and the handful is the interesting part: what a slow wake needs
 * naming is not its duration but what stood in front of it.  Each of these
 * lines carries the two facts that identify the cause -- how many threads were
 * queued ahead of the woken one, and who held the CPU at that moment -- and
 * printing them is what turned a whole afternoon of theories into one
 * measurement: 122 slow wakes in a boot, every single one with two to four
 * threads queued ahead, delayed by a whole quantum each.  It reads zero now.
 *
 * Bounded so a boot that goes wrong floods nothing; the counter keeps counting
 * after the printing stops, so the tally stays honest.
 */
#define	WAKE_SLOW_MS		20	/* two quanta: not queueing, standing */
#define	WAKE_SLOW_MAX_LINES	20
static uint64_t		wake_slow_n;		/* (a) wakes over the bar  */
static uint64_t		wake_slow_lines;	/* (a) ...of them, printed */

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
 * Ready, and ahead of the queue.
 *
 * WHAT THE TAIL COSTS A WAKE.  A thread that has just been woken is, by
 * construction, a thread that gave the CPU up before its slice was over: it
 * asked for something, it was not there, it slept.  Putting it behind the
 * threads that have been running means the news it was woken for waits for
 * their slices, and a slice here is five ticks -- so wake latency is not one
 * switch but "however many runnable threads are ahead of me" times fifty
 * milliseconds.  That was measured rather than reasoned about: in one boot,
 * 122 wakes waited 20 ms or longer, every one of them with two to four threads
 * queued ahead of it, and the delay was a whole quantum almost every time
 * (a hundred milliseconds when two of the queue wanted the CPU, not one).
 *
 * WHY THIS DOES NOT STARVE THE QUEUE.  The boost is worth exactly one slice
 * and is spent by taking it: a thread put here runs, and the moment it is
 * preempted -- or yields, or blocks and is woken having ALREADY had its turn --
 * it goes back through enqueue_locked like everybody else.  Nothing accumulates
 * priority by sleeping; sleeping only buys the CPU for the first look, which is
 * the whole point of having been woken.  A thread that wants the machine cannot
 * get it this way, because wanting the machine is not what puts a thread here.
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
	thread_switch_asm(&self->th_rsp_save, next->th_rsp_save,
	    self->th_fpu, next->th_fpu);

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
	 * On the sleep queue now, and only now: after the pre-park kill check
	 * above, which is the one path out of here that does NOT go through
	 * thread_wake and so would leave a linked thread behind to be woken
	 * after it had been freed.  A NULL target names no channel and cannot
	 * be woken by one, so it stays off the list.
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
	th->th_wake_ms      = clock_uptime_ms();
	th->th_wake_hog     = current_thread != NULL ?
	    current_thread->th_name : "?";
	th->th_wake_qlen    = runq_len;
	spin_unlock(&th->th_lock);

	sleepq_unlink_locked(th);
	enqueue_first_locked(th);
	/*
	 * ASK FOR A RESCHEDULE.  Making a thread READY is not making it run:
	 * without this it waits in the queue until whoever holds the CPU gives
	 * it up voluntarily or the quantum expires -- and the quantum is five
	 * ticks, so news can arrive fifty milliseconds after it happened.
	 *
	 * This line is here because that was MEASURED, twice over.  Across a
	 * whole boot, 24074 wakes spent 154 seconds between being made ready
	 * and running, a mean of 6 ms each; with this, 530 ms, a mean of 0.
	 * And it is what makes sleeping better than polling rather than worse:
	 * parking three waits that used to poll made them ten times SLOWER to
	 * notice anything, because a poller gets a fresh look every time it is
	 * scheduled while a sleeper gets one look and must be given the CPU to
	 * take it.
	 *
	 * Honoured at the next point it is safe to honour -- the tail of
	 * intr_dispatch, or the preempt_enable that drops the count to zero,
	 * which the spin_unlock below reaches at once.
	 */
	preempt_need_resched = 1;
	spin_unlock(&sched_lock);
}

/*
 * sched_wakeup: make ready every thread parked on `chan`.
 *
 * The caller's contract is the one every sleep/wakeup pair has ever had, and
 * getting it wrong is the classic lost wakeup: the waiter must test the
 * condition and park under ONE lock, handing that lock to
 * thread_block_release so the drop happens with sched_lock held.  The waker
 * changes the condition, then calls this -- in either order with respect to
 * dropping its own lock, since a waiter is already on the queue by the time it
 * has let go of the lock the waker needs.
 *
 * Waking is a HINT and never a promise that the condition holds: a woken
 * thread must re-test in a loop.  Another thread may take the bytes first, a
 * kill may have arrived, or the channel may be shared by waiters wanting
 * different things.  Every caller in this kernel parks inside `for (;;)`.
 *
 * Returns the number woken, which is what lets a caller count its own
 * fruitless trips honestly rather than guessing.
 */
/*
 * Wake every thread of `task` that is asleep on a CHANNEL, and no others.
 * Returns how many there were.
 *
 * For posting a signal.  A signal in this kernel sets a bit in the target and
 * returns; a thread already asleep inside a syscall never sees it, and before
 * anything here parked, that did not matter -- the waits polled, so the bit was
 * noticed on the next trip round.  Parking them made read(2) on a pipe stop
 * being interruptible, which POSIX requires it to be, and which a scene in
 * pipefork tests and duly failed.
 *
 * WHY ONLY CHANNEL SLEEPERS.  A thread waiting on a Mach port is linked into
 * that port's waiter list through th_runq_link -- the same field the runqueue
 * uses -- so making it READY while it is still on the port's list overwrites
 * the link and truncates the list.  Waking one "just in case" is therefore not
 * a harmless spurious wake but corruption, and the block reason is what tells
 * the two apart: a channel sleeper is on the scheduler's own list and can be
 * woken by anyone at any time, which is the whole reason that list exists.
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
	if (n != 0)
		preempt_need_resched = 1;
	spin_unlock(&sched_lock);
	return (n);
}

uint32_t
sched_wakeup(void *chan)
{
	struct thread	*th;
	struct thread	*next;
	uint32_t	 n;

	KASSERT(chan != NULL, "sched_wakeup: NULL channel would wake nobody");

	n = 0;
	spin_lock(&sched_lock);
	/*
	 * Read the link BEFORE waking: making a thread ready takes it off
	 * this list, so walking through the entry after the fact would follow
	 * a pointer that the unlink has already cleared.
	 */
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
	if (n != 0)
		preempt_need_resched = 1;
	spin_unlock(&sched_lock);
	return (n);
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
