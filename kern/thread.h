/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_THREAD_H_
#define	_SYS_THREAD_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "port.h"
#include "queue.h"
#include "spinlock.h"

struct task;

/*
 * Thread states.
 *
 *	INIT	     just created, not yet in any queue
 *	READY	     in the runqueue, runnable
 *	RUNNING	     currently executing on the CPU
 *	BLOCKED	     waiting on something (port, semaphore, ...)
 *	ZOMBIE	     exited; kstack still allocated until reaped
 *
 * Transitions are linear: INIT -> READY -> RUNNING -> {READY|BLOCKED}
 * with a final RUNNING -> ZOMBIE on exit.  The scheduler is the only
 * thing that touches RUNNING; everything else picks READY/BLOCKED.
 */
enum thread_state {
	THREAD_INIT = 0,
	THREAD_READY,
	THREAD_RUNNING,
	THREAD_BLOCKED,
	THREAD_ZOMBIE,
};

/*
 * Reason a thread is BLOCKED -- diagnostic only.  Real blockers
 * (struct port *, future struct sem *) live in th_block_target.
 */
enum thread_block_reason {
	THREAD_NOT_BLOCKED = 0,
	THREAD_BLOCK_PORT,
	THREAD_BLOCK_SLEEP,
	THREAD_BLOCK_JOIN,
};

struct thread {
	struct spinlock		 th_lock;	/* serialises state moves   */
	uint64_t		 th_id;
	const char		*th_name;
	struct task		*th_task;

	enum thread_state	 th_state;
	enum thread_block_reason th_block_reason;
	void			*th_block_target;

	/*
	 * Saved stack pointer at the point of the last context switch.
	 * The switch asm pops 6 callee-saved regs then RETs, so the
	 * stack laid out at thread_create time has 7 quads above this
	 * value (regs + entry RIP).
	 */
	uint64_t		 th_rsp_save;

	void			*th_kstack_base;	/* kmalloc'd or pmm */
	size_t			 th_kstack_size;
	bool			 th_kstack_owned;	/* free on reap?    */

	void			(*th_entry)(void *);
	void			*th_arg;

	/*
	 * Spinlocks this thread holds, and whether interrupts were enabled
	 * when it took the first of them.  PER-THREAD, and that is the
	 * interesting part: sched_lock is held ACROSS a context switch, so
	 * the lock object changes hands -- but each thread still performs
	 * exactly one acquire and one release of its own, and the interrupt
	 * state that has to be put back is the state THAT THREAD found.
	 *
	 * Per-CPU would balance too and would restore the wrong answer: a
	 * thread that yielded voluntarily with interrupts on can be resumed
	 * by a thread that entered the scheduler from an interrupt with them
	 * off, and would then run kernel code with interrupts disabled until
	 * something else happened to enable them.  Which is not a crash; it
	 * is preemption quietly stopping on that CPU.
	 */
	int			 th_spin_depth;		/* (i) locks held   */
	bool			 th_spin_saved_if;	/* (i) IF at depth 1 */

	/*
	 * Sleep mutexes this thread holds (kern/mutex.c moves it, nobody
	 * else reads another thread's copy -- so it needs no lock).
	 *
	 * ⚠ WHAT IT GATES: a kill may not retire a thread while this is
	 * nonzero.  A mutex dies with its owner -- there is no unlock by
	 * proxy, mutex_unlock asserts the caller IS the owner -- so a
	 * thread retired mid-park with a mutex held would take it to the
	 * grave locked.  And the mutex most often held across a park is
	 * `fs_lock`, which every disk-touching path sleeps under: one
	 * killed writer and the machine's next disk touch parks for ever
	 * behind a corpse.  So the kill checks in thread_block_release
	 * decline while this is up; the thread finishes what the lock was
	 * for and dies at the first check it reaches empty-handed -- the
	 * syscall boundary for a user thread (which asserts the count is
	 * zero there), the next bare park for a kernel one.
	 */
	int			 th_mutex_depth;	/* (self only)      */

	struct thread		*th_runq_link;		/* (sched_lock)     */
	struct thread		*th_task_link;		/* task->t_threads  */
	SLIST_ENTRY(thread)	 th_zombie_link;	/* zombie SLIST     */

	/*
	 * Parked on an OBJECT that keeps its own queue of who is waiting: a
	 * Mach port's receivers, its blocked senders, a port set's receivers,
	 * a mutex's waiters.  A thread is on at most one of those at a time,
	 * which is what makes one field enough for all of them.
	 *
	 * ⚠ AND WHY IT IS NOT ANOTHER TENANT OF th_runq_link, which is what it
	 * was.  Those lists are held by the object across the whole park, and
	 * a park can end WITHOUT THE OBJECT BEING TOLD: a deadline expires,
	 * and the thread is put on the runqueue -- through th_runq_link --
	 * while the port still names it.  From then on the two lists are one
	 * list.  A sender that pops the port's head writes NULL into the field
	 * and truncates the RUNQUEUE; the scheduler's next enqueue writes the
	 * runqueue's head into the port's list and hands the next message to a
	 * thread that was never waiting for it.  What it looks like from
	 * outside is a machine that goes quiet with three processors idle,
	 * because threads have stopped being on any list at all.
	 *
	 * The field costs eight bytes per thread.  Sharing it cost one boot in
	 * four.
	 */
	struct thread		*th_wait_link;		/* (the object's)   */

	/*
	 * ⚠ AND WHERE THAT LIST LIVES, because the link alone is a link with
	 * amnesia: it says this thread is on somebody's queue and not whose.
	 * The three fields below are that memory -- the queue's head, tail
	 * and lock, noted by the thread itself just before it parks on an
	 * object's list and forgotten by the same thread when it takes
	 * itself off.  Nobody else touches them: an extractor that pops this
	 * thread leaves the note alone, because the popped thread wakes,
	 * runs its caller's unconditional detach, finds nothing, and
	 * forgets on its own.
	 *
	 * What the note is FOR is the one exit that never comes back.
	 * thread_block_release retires a killed thread from inside the
	 * block, above the caller that enqueued it, so the caller's detach
	 * never runs and the object keeps a pointer to a thread about to be
	 * reaped.  The deadline list had this same defect and thread_exit
	 * could settle it by name, because there is exactly one deadline
	 * list; there is no walking every port in the system, so for the
	 * object lists the thread has to know where it is.
	 */
	struct thread		**th_wait_qhead;	/* (th_wait_qlock)  */
	struct thread		**th_wait_qtail;	/* (th_wait_qlock)  */
	struct spinlock		 *th_wait_qlock;	/* (self only)      */

	/*
	 * Queued for a wake an interrupt handler could not perform itself.
	 * Its own field for the third time and the same reason: the thread
	 * sitting on this list is BLOCKED and somebody else is entitled to
	 * wake it -- a deadline, a sender, a kill -- and the moment they do,
	 * the scheduler puts it on the runqueue.  Through th_runq_link, which
	 * this list used to be threaded on, that write truncated the LIFO and
	 * every wake queued behind it was simply never delivered.
	 */
	struct thread		*th_irq_link;		/* (a) irq_wake_head */

	/*
	 * Sleep-queue link: threads parked on a CHANNEL, i.e. those that
	 * called thread_block with a non-NULL target, so sched_wakeup can
	 * find them by what they are waiting for rather than by a pointer
	 * the waited-on object had to keep.  Its own field for the same
	 * reason as above, and it was the first one to need it.
	 */
	struct thread		*th_sleep_link;		/* (sched_lock)     */

	/*
	 * When this thread was last made READY out of BLOCKED, in
	 * clock_uptime_ms().  Read once by the thread itself when it resumes,
	 * to report how long a wake took to become a run -- the number that
	 * says whether parking a waiter made it hear news sooner or later.
	 */
	uint64_t		 th_wake_ms;		/* (sched_lock)     */

	/*
	 * ⚠ A WAKE THAT ARRIVED BEFORE THE SLEEP, WHICH ONLY A SECOND
	 * PROCESSOR CAN DO.
	 *
	 * thread_wake on a thread that is not BLOCKED used to return doing
	 * nothing, and that was right: the thread was READY or RUNNING, so
	 * the news it was being woken for could not be missed.  With one
	 * processor the third possibility -- that it is BETWEEN, having
	 * decided to sleep and not yet committed -- could not arise, because
	 * deciding and committing happen with no window in which anything
	 * else on that CPU could run.
	 *
	 * With four it arises constantly, and the ATA driver is where it
	 * showed: a thread installs itself as the channel's waiter, and the
	 * disk's interrupt lands on ANOTHER processor and posts the wake
	 * before this one has reached THREAD_BLOCKED.  The wake found a
	 * RUNNING thread, did nothing, and the thread then slept for ever --
	 * one boot in four, always inside a filesystem write, always with the
	 * machine otherwise healthy.
	 *
	 * So a wake that finds nobody asleep leaves a note, and the next
	 * attempt to sleep reads it and does not sleep.  The caller's loop
	 * re-tests its condition and finds what the wake was about, which is
	 * the contract every sleeper in this kernel already keeps -- waking is
	 * a hint, never a promise, and everyone parks inside for (;;).
	 */
	volatile int		 th_wake_pending;	/* (sched_lock)     */

	/*
	 * ...and the two facts that explain a slow one, recorded at the same
	 * moment: how many threads were already queued ahead of this one, and
	 * who held the CPU while it waited.  A duration alone says a wake was
	 * late; these say why, and the difference is a diagnosis in one line
	 * instead of a rebuild with a new print in it.  Read by the woken
	 * thread itself, next to th_wake_ms.
	 */
	const char		*th_wake_hog;		/* (sched_lock)     */
	uint32_t		 th_wake_qlen;		/* (sched_lock)     */

	/*
	 * Trusted-send flag.  Toggled by mach_msg_send_trusted around a
	 * send call that originates from kernel code shipping kernel-rodata
	 * bytes through an OOL descriptor (e.g. the "man" service replying
	 * with a man page).  When set, send_capture_ool skips its user-VA
	 * range validation on the assumption the kernel knows what address
	 * it is dereferencing.  Must NEVER be exposed to userspace.
	 */
	bool			 th_trusted_send;

	/*
	 * Timeout plumbing for mach_msg_recv_timed.  th_wake_deadline_ms
	 * holds the absolute clock_uptime_ms() value at which this thread
	 * wants to be woken; sched_check_timeouts (called from the PIT
	 * IRQ) walks the global timed_waiters list and posts an IRQ wake
	 * when the deadline has passed.  th_timed_out is the resulting
	 * signal back to the recv loop: "you woke because your timer
	 * expired, not because a message arrived".  th_timed_link threads
	 * the timed_waiters list itself.
	 */
	uint64_t		 th_wake_deadline_ms;
	volatile int		 th_timed_out;
	struct thread		*th_timed_link;

	/*
	 * WITNESS-lite per-thread held-locks stack.  Each entry records
	 * the lock's class name (identity-compared, not strcmp) and the
	 * RIP that acquired it.  Pushed by spin_lock, popped by
	 * spin_unlock.  Used both for lock-order cycle detection and for
	 * `s locks` in ddb / panic dumps.
	 */
#define	THREAD_HELD_LOCKS_MAX	8
	struct witness_held {
		const char	*wh_name;
		uintptr_t	 wh_ra;
	}			 th_held[THREAD_HELD_LOCKS_MAX];
	uint8_t			 th_held_count;

	/*
	 * Per-thread exception ports.  Identical shape to the task-level
	 * t_exc_ports array; user_fault_die checks this slot FIRST and
	 * falls through to the task-level slot only when this entry is
	 * NULL.  Thread-level wins because the canonical use is a
	 * debugger attached to one particular thread, watching its
	 * faults specifically while the rest of the task takes a
	 * different (or no) handler.  All under th_lock.
	 *
	 * Refs balance the same way the task-level slots do: one SEND
	 * ref per non-NULL slot, dropped on slot replace and on thread
	 * reap (sched_reap_zombies before kfree).
	 */
	struct port		*th_exc_ports[EXC_TYPE_COUNT];

	/*
	 * x87/SSE register file (FXSAVE area), saved/restored by
	 * thread_switch_asm on every context switch so two ring-3 tasks using
	 * SSE never clobber each other's XMM/x87 state.  Architecturally 512
	 * bytes; 16-byte aligned because FXSAVE/FXRSTOR require it (the thread
	 * is kmalloc'd 16-aligned and the attribute pins this field's offset).
	 * Seeded to a clean state by fpu_clean_state at create.
	 */
	uint8_t			 th_fpu[512] __attribute__((aligned(16)));
};

/*
 * `current_thread' is not a variable any more -- it is this CPU's
 * cp_curthread, reached through the GS base (machine/cpu.h), and it is
 * still an lvalue so the scheduler assigns to it exactly as before.  Every
 * file that uses it gets the definition from here, which is where it always
 * came from.
 */

#define	THREAD_DEFAULT_KSTACK	(16 * 1024)	/* 4 pages */

void		thread_subsystem_init(void);

/*
 * Give the context that is ALREADY RUNNING a thread structure and make it this
 * CPU's current thread.  A processor cannot switch away from a thread that
 * does not exist, so this is what a CPU does before it joins the scheduler --
 * the boot processor inside kmain, and every application processor standing on
 * the stack its trampoline handed it.
 *
 * The stack belongs to the caller and is never freed by the reaper.  Returns
 * NULL only if there is no memory for the structure.
 */
struct thread	*thread_adopt_current(struct task *, const char *name);

struct thread	*thread_create(struct task *,
		    void (*entry)(void *), void *arg, const char *name);
void		thread_start(struct thread *);
void		thread_exit(void) __attribute__((noreturn));

/*
 * The waiter-list note (see the th_wait_qhead comment in struct thread).
 * note: record the object list this thread is about to park on -- called
 * with the list's lock held, right after linking in.  forget: clear it --
 * called by the thread itself after its unconditional detach.
 * unbind_locked: take the thread off the noted list and forget, with the
 * noted lock already held -- the pre-park kill exit's case, where the lock
 * the caller parked under IS the list's lock and has not been dropped yet.
 */
void		thread_wait_note(struct thread *, struct thread **qhead,
		    struct thread **qtail, struct spinlock *qlock);
void		thread_wait_forget(struct thread *);
void		thread_wait_unbind_locked(struct thread *);

const char	*thread_state_name(enum thread_state);
const char	*thread_block_reason_name(enum thread_block_reason);
void		thread_print(struct thread *);

/*
 * Install `port` into every slot of `th->th_exc_ports` named by
 * `types_mask`; semantics + ref discipline mirror
 * task_set_exception_ports.  Passing port=NULL clears the named
 * slots.  Returns MACH_MSG_OK on success, MACH_E_INVAL on a bad
 * mask.  Caller typically passes current_thread.
 */
int		thread_set_exception_ports(struct thread *th,
		    uint32_t types_mask, struct port *port);

/* defined in switch.S */
void		thread_switch_asm(uint64_t *old_rsp_save, uint64_t new_rsp,
		    void *old_fpu, void *new_fpu);

#endif /* !_SYS_THREAD_H_ */
