/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "darwin.h"
#include "fpu.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "port_internal.h"
#include "sched.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

/* Defined in sched.c; released by trampoline on first dispatch. */
extern void	sched_post_switch_unlock(void);

static struct spinlock	threads_lock = SPINLOCK_INIT("threads-global");
static uint64_t		next_thread_id;

static void	thread_trampoline(void);

/*
 * Give the context already running a thread structure and make it this
 * CPU's current thread (boot CPU and APs alike; see thread.h).  The stack
 * is the caller's, so th_kstack_base stays NULL and the reaper never frees
 * it.  th_rsp_save is filled in by the first switch away.
 */
struct thread *
thread_adopt_current(struct task *t, const char *name)
{
	struct thread	*th;

	if (t == NULL)
		return (NULL);

	th = kmalloc(sizeof(*th));
	if (th == NULL)
		return (NULL);

	spin_init(&th->th_lock, "thread");
	spin_lock(&threads_lock);
	th->th_id = next_thread_id++;
	spin_unlock(&threads_lock);

	th->th_name            = name != NULL ? name : "(anon)";
	th->th_task            = t;
	th->th_state           = THREAD_RUNNING;
	th->th_block_reason    = THREAD_NOT_BLOCKED;
	th->th_block_target    = NULL;
	th->th_rsp_save        = 0;
	th->th_kstack_base     = NULL;	/* the caller's stack -- not ours   */
	th->th_kstack_size     = 0;
	th->th_kstack_owned    = false;
	th->th_entry           = NULL;
	th->th_arg             = NULL;
	/*
	 * Holds no lock, unlike a created thread, whose first act is to
	 * release one it never took (see thread_create).
	 */
	th->th_spin_depth      = 0;
	th->th_spin_saved_if   = false;
	th->th_mutex_depth     = 0;
	th->th_runq_link       = NULL;
	th->th_task_link       = NULL;
	th->th_wait_link       = NULL;
	th->th_wait_qhead      = NULL;
	th->th_wait_qtail      = NULL;
	th->th_wait_qlock      = NULL;
	th->th_wait_slot       = NULL;
	th->th_irq_link        = NULL;
	th->th_sleep_link      = NULL;
	th->th_wake_ms         = 0;
	th->th_wake_pending    = 0;
	th->th_wake_hog        = NULL;
	th->th_wake_qlen       = 0;
	th->th_trusted_send    = false;
	th->th_wake_deadline_ms = 0;
	th->th_timed_out       = 0;
	th->th_timed_link      = NULL;
	th->th_wake_hold       = 0;
	th->th_irq_queued      = 0;
	th->th_held_count      = 0;
	{
		unsigned	exi;

		for (exi = 0; exi < EXC_TYPE_COUNT; exi++)
			th->th_exc_ports[exi] = NULL;
	}

	fpu_clean_state(th->th_fpu);

	current_thread = th;
	task_attach_thread(t, th);

	return (th);
}

void
thread_subsystem_init(void)
{
	struct thread	*boot;

	next_thread_id = 1;

	if (kernel_task == NULL)
		panic("thread_subsystem_init: kernel_task not created");

	boot = thread_adopt_current(kernel_task, "boot");
	if (boot == NULL)
		panic("thread_subsystem_init: kmalloc(boot thread) failed");

	kprintf("thread: boot thread id=%llu attached to task %s\n",
	    (unsigned long long)boot->th_id, kernel_task->t_name);
}

/*
 * Create a thread in THREAD_INIT with its own kstack, holding a fake
 * switch frame (below) so the first switch into it "returns" into
 * thread_trampoline with zeroed callee-saved registers.  The trampoline
 * finds itself through current_thread, which the scheduler sets before
 * the switch, and calls the entry function.  thread_start makes it
 * runnable.
 */
struct thread *
thread_create(struct task *t, void (*entry)(void *), void *arg,
    const char *name)
{
	struct thread	*th;
	uint8_t		*kstack;
	uint64_t	*sp;

	if (t == NULL || entry == NULL)
		return (NULL);

	th = kmalloc(sizeof(*th));
	if (th == NULL)
		return (NULL);

	kstack = kmalloc(THREAD_DEFAULT_KSTACK);
	if (kstack == NULL) {
		kfree(th);
		return (NULL);
	}

	spin_init(&th->th_lock, "thread");
	spin_lock(&threads_lock);
	th->th_id = next_thread_id++;
	spin_unlock(&threads_lock);

	th->th_name              = name != NULL ? name : "(anon)";
	th->th_task              = t;
	th->th_state             = THREAD_INIT;
	th->th_block_reason      = THREAD_NOT_BLOCKED;
	th->th_block_target      = NULL;
	th->th_kstack_base       = kstack;
	th->th_kstack_size       = THREAD_DEFAULT_KSTACK;
	th->th_kstack_owned      = true;
	th->th_entry             = entry;
	th->th_arg               = arg;

	/*
	 * Pre-loaded as though holding one lock: the switch into a new thread
	 * happens with sched_lock held by the thread switching away, and
	 * thread_trampoline's first act is to release it.  A count starting
	 * at zero would go negative there.  The release restores interrupts
	 * on, so the thread is preemptible from its first instruction.
	 */
	th->th_spin_depth        = 1;
	th->th_spin_saved_if     = true;
	th->th_mutex_depth       = 0;
	th->th_runq_link         = NULL;
	th->th_task_link         = NULL;
	th->th_wait_link         = NULL;
	th->th_wait_qhead        = NULL;
	th->th_wait_qtail        = NULL;
	th->th_wait_qlock        = NULL;
	th->th_wait_slot         = NULL;
	th->th_irq_link          = NULL;
	th->th_sleep_link        = NULL;
	th->th_wake_ms           = 0;
	th->th_wake_pending      = 0;
	th->th_wake_hog          = NULL;
	th->th_wake_qlen         = 0;
	th->th_trusted_send      = false;
	th->th_wake_deadline_ms  = 0;
	th->th_timed_out         = 0;
	th->th_timed_link        = NULL;
	th->th_wake_hold         = 0;
	th->th_irq_queued        = 0;
	th->th_held_count        = 0;
	{
		unsigned	exi;

		for (exi = 0; exi < EXC_TYPE_COUNT; exi++)
			th->th_exc_ports[exi] = NULL;
	}

	fpu_clean_state(th->th_fpu);

	/*
	 * Fake-call frame at the high end of the kstack.  switch.S
	 * pops r15..rbp + popfq then rets, so the frame is:
	 *	[trampoline RIP] [rflags] [rbp] [rbx] [r12] [r13] [r14] [r15]
	 * with r15 at the lowest address (popped first).  rflags is
	 * 0x202 -- bit 1 (always-set reserved) plus IF=1, so the
	 * thread starts with interrupts enabled.
	 */
	sp = (uint64_t *)(kstack + THREAD_DEFAULT_KSTACK);
	*--sp = (uint64_t)(uintptr_t)thread_trampoline;	/* RIP for ret  */
	*--sp = 0x202;	/* rflags: IF=1, bit 1 reserved=1               */
	*--sp = 0;	/* rbp                                          */
	*--sp = 0;	/* rbx                                          */
	*--sp = 0;	/* r12                                          */
	*--sp = 0;	/* r13                                          */
	*--sp = 0;	/* r14                                          */
	*--sp = 0;	/* r15                                          */
	th->th_rsp_save = (uint64_t)(uintptr_t)sp;

	task_attach_thread(t, th);

	return (th);
}

/*
 * Place a freshly-created thread onto the runqueue so the next
 * scheduler dispatch picks it up.  Separate from thread_create so
 * the caller can fully initialise the thread (including any external
 * bookkeeping) before it can actually run.
 */
void
thread_start(struct thread *th)
{

	spin_lock(&th->th_lock);
	KASSERT(th->th_state == THREAD_INIT,
	    "thread_start: thread already started");
	th->th_state = THREAD_READY;
	spin_unlock(&th->th_lock);

	sched_enqueue(th);
}

void
thread_wait_note(struct thread *th, struct thread **qhead,
    struct thread **qtail, struct spinlock *qlock)
{

	/*
	 * A second note before the first is cleared means a thread linked
	 * into two lists through one field.  Catching it here names the
	 * enqueuer that forgot its detach, instead of wedging whoever parks
	 * next.
	 */
	KASSERT(th->th_wait_qlock == NULL,
	    "thread_wait_note: still noted on another list");
	th->th_wait_qhead = qhead;
	th->th_wait_qtail = qtail;
	th->th_wait_qlock = qlock;
}

void
thread_wait_forget(struct thread *th)
{

	th->th_wait_qhead = NULL;
	th->th_wait_qtail = NULL;
	th->th_wait_qlock = NULL;
}

/*
 * The slot note (th_wait_slot in thread.h), for drivers whose waiter list
 * is one cell an ISR exchanges.  note: around the whole wait loop, not per
 * install, because the cell may name this thread at any point inside it.
 * forget: before every return from the loop, by which time the loop has
 * emptied the cell.
 */
void
thread_slot_note(struct thread *th, struct thread *volatile *slot)
{

	KASSERT(th->th_wait_slot == NULL,
	    "thread_slot_note: still noted on another slot");
	th->th_wait_slot = slot;
}

void
thread_slot_forget(struct thread *th)
{

	th->th_wait_slot = NULL;
}

/*
 * The waker's claim against reap (th_wake_hold in thread.h says when it
 * may be taken).  Atomic because claims come from anywhere: thread context
 * under an object's lock, the deadline walk under timed_lock, a device ISR
 * that just exchanged its waiter slot.
 */
void
thread_hold(struct thread *th)
{

	__atomic_add_fetch(&th->th_wake_hold, 1, __ATOMIC_ACQ_REL);
}

void
thread_unhold(struct thread *th)
{
	uint32_t	was;

	was = __atomic_fetch_sub(&th->th_wake_hold, 1, __ATOMIC_ACQ_REL);
	KASSERT(was != 0, "thread_unhold: releasing a hold never taken");
}

void
thread_wait_unbind_locked(struct thread *th)
{
	struct thread	**pp;
	struct thread	 *prev;

	KASSERT(th->th_wait_qlock != NULL,
	    "thread_wait_unbind_locked: nothing noted");
	prev = NULL;
	for (pp = th->th_wait_qhead; *pp != NULL;
	    pp = &(*pp)->th_wait_link) {
		if (*pp == th) {
			*pp = th->th_wait_link;
			if (*th->th_wait_qtail == th)
				*th->th_wait_qtail = prev;
			th->th_wait_link = NULL;
			break;
		}
		prev = *pp;
	}
	thread_wait_forget(th);
}

/*
 * Settle the note from an exiting thread's own context: take the noted
 * lock, take the thread off the noted list if it is still there, forget.
 *
 * Finding it already gone is legitimate: an extractor popped it, and its
 * wake is what brought the thread here.  An extractor that has popped it
 * but not yet woken it is covered by the hold it took under the same lock
 * (th_wake_hold); the reaper leaves a held body alone.
 */
static void
thread_wait_unbind(struct thread *th)
{
	struct spinlock	*ql;

	ql = th->th_wait_qlock;
	if (ql == NULL)
		return;
	spin_lock(ql);
	thread_wait_unbind_locked(th);
	spin_unlock(ql);
}

void
thread_exit(void)
{
	struct thread	*me;

	me = current_thread;
	KASSERT(me != NULL, "thread_exit: no current thread");

	/*
	 * No exit may carry a mutex out: there is no unlock by proxy, so it
	 * would stay held for good.  The kill checks decline while
	 * th_mutex_depth is up and the syscall boundary asserts zero; this
	 * is the last tripwire, on every exit path.
	 */
	KASSERT(me->th_mutex_depth == 0,
	    "thread_exit: dying with a mutex still held");

	/*
	 * A Darwin task's files are closed by its own last thread, here, not
	 * by the reaper: closing can block (fs_lock is a mutex, and an
	 * orphan's blocks are freed on last close), and the reaper may run on
	 * the idle thread, which cannot.  Unix closes files in exit(2) for the
	 * same reason.  The reaper's call remains as an idempotent fallback.
	 * Darwin tasks are single-threaded; the count checks it.
	 */
	if (me->th_task != NULL &&
	    me->th_task->t_personality == TASK_PERSONALITY_DARWIN &&
	    me->th_task->t_nthreads == 1)
		darwin_files_teardown(me->th_task);

	/*
	 * Off every list that may still name this thread, before it becomes
	 * reapable.  A thread killed mid-park is retired from inside
	 * thread_block_release, so the caller that linked it in never runs its
	 * own removal.
	 *
	 * First the deadline list, which the timer walks every tick.
	 */
	sched_remove_timed_waiter(me);

	/* Then the object's waiter list, found through the note. */
	thread_wait_unbind(me);

	/*
	 * Then the driver's slot.  The ISR takes the cell with a bare
	 * exchange, so this is one CAS of our own name for NULL, and exactly
	 * one side gets the pointer: if the exit wins the interrupt finds the
	 * cell empty; if the interrupt wins, its wake went through
	 * sched_post_irq_wake, whose hold keeps this body unreaped until the
	 * wake has landed.
	 */
	if (me->th_wait_slot != NULL) {
		struct thread	*expect;

		expect = me;
		(void)__atomic_compare_exchange_n(me->th_wait_slot, &expect,
		    NULL, false, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED);
		me->th_wait_slot = NULL;
	}

	spin_lock(&me->th_lock);
	me->th_state = THREAD_ZOMBIE;
	spin_unlock(&me->th_lock);

	sched_handoff_zombie(me);
	/* NOTREACHED */
	panic("thread_exit: returned from sched_handoff_zombie");
}

int
thread_set_exception_ports(struct thread *th, uint32_t types_mask,
    struct port *port)
{
	struct port	*prev[EXC_TYPE_COUNT];
	unsigned	 i;

	if (th == NULL)
		return (MACH_E_INVAL);
	if ((types_mask & ~EXC_MASK_ALL) != 0)
		return (MACH_E_INVAL);
	if (types_mask == 0)
		return (MACH_MSG_OK);

	/*
	 * Mirror task_set_exception_ports' ref ordering: take
	 * popcount(types_mask) refs on the new port before the swap, so a
	 * concurrent user_fault_die snapshot always reads a live ref.
	 */
	if (port != NULL) {
		for (i = 0; i < EXC_TYPE_COUNT; i++) {
			if (types_mask & (1u << i))
				port_ref(port, MACH_PORT_RIGHT_SEND);
		}
	}

	for (i = 0; i < EXC_TYPE_COUNT; i++)
		prev[i] = NULL;

	spin_lock(&th->th_lock);
	for (i = 0; i < EXC_TYPE_COUNT; i++) {
		if ((types_mask & (1u << i)) == 0)
			continue;
		prev[i] = th->th_exc_ports[i];
		th->th_exc_ports[i] = port;
	}
	spin_unlock(&th->th_lock);

	for (i = 0; i < EXC_TYPE_COUNT; i++) {
		if (prev[i] != NULL)
			port_deref(prev[i], MACH_PORT_RIGHT_SEND);
	}
	return (MACH_MSG_OK);
}

const char *
thread_state_name(enum thread_state s)
{

	switch (s) {
	case THREAD_INIT:	return ("init");
	case THREAD_READY:	return ("ready");
	case THREAD_RUNNING:	return ("running");
	case THREAD_BLOCKED:	return ("blocked");
	case THREAD_ZOMBIE:	return ("zombie");
	default:		return ("?");
	}
}

const char *
thread_block_reason_name(enum thread_block_reason r)
{

	switch (r) {
	case THREAD_NOT_BLOCKED:	return ("nothing");
	case THREAD_BLOCK_PORT:		return ("port");
	case THREAD_BLOCK_SLEEP:	return ("channel");
	case THREAD_BLOCK_JOIN:		return ("join");
	default:			return ("?");
	}
}

void
thread_print(struct thread *th)
{

	kprintf("thread id=%llu name=%-12s state=%-7s task=%s\n",
	    (unsigned long long)th->th_id, th->th_name,
	    thread_state_name(th->th_state),
	    th->th_task != NULL ? th->th_task->t_name : "?");
}

/*
 * Where every new thread starts, via the fake frame's return address.
 * Calls the entry function; if it returns, thread_exit() rather than
 * running off the end of the stack.
 */
static void
thread_trampoline(void)
{
	struct thread	*me;

	/*
	 * The switch into us happened with sched_lock held by the previous
	 * thread; release it before anything else, or the next thread_yield
	 * would acquire it recursively and panic.
	 */
	sched_post_switch_unlock();

	me = current_thread;
	KASSERT(me != NULL,
	    "thread_trampoline: no current_thread set");
	KASSERT(me->th_entry != NULL,
	    "thread_trampoline: no entry function");

	me->th_entry(me->th_arg);
	thread_exit();
	/* NOTREACHED */
}
