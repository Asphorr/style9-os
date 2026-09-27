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
 *	BLOCKED	     waiting on something (port, channel, ...)
 *	ZOMBIE	     exited; kstack still allocated until reaped
 *
 * INIT -> READY (thread_start), READY <-> RUNNING, RUNNING -> BLOCKED ->
 * READY, and finally RUNNING -> ZOMBIE in thread_exit.
 */
enum thread_state {
	THREAD_INIT = 0,
	THREAD_READY,
	THREAD_RUNNING,
	THREAD_BLOCKED,
	THREAD_ZOMBIE,
};

/*
 * Why a thread is BLOCKED.  Mostly diagnostic, but sched_wake_sleepers_of
 * wakes only THREAD_BLOCK_SLEEP.  What it waits on (a struct port *, a
 * sleep channel) is th_block_target.
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
	 * The switch asm pops 6 callee-saved regs and RFLAGS, then RETs, so
	 * the stack laid out at thread_create time has 8 quads above this
	 * value (regs + RFLAGS + entry RIP).
	 */
	uint64_t		 th_rsp_save;

	void			*th_kstack_base;	/* kmalloc'd or pmm */
	size_t			 th_kstack_size;
	bool			 th_kstack_owned;	/* free on reap?    */

	void			(*th_entry)(void *);
	void			*th_arg;

	/*
	 * Spinlocks this thread holds, and whether interrupts were enabled
	 * when it took the first.  Per-thread, although sched_lock changes
	 * hands across a switch: each thread still does one acquire and one
	 * release of its own, and must get back the interrupt state it found.
	 * A per-CPU copy would balance but restore the wrong state -- a
	 * thread that yielded with interrupts on, resumed by one that entered
	 * the scheduler from an interrupt, would run on with them off, and
	 * preemption would silently stop on that CPU.
	 */
	int			 th_spin_depth;		/* (i) locks held   */
	bool			 th_spin_saved_if;	/* (i) IF at depth 1 */

	/*
	 * Sleep mutexes this thread holds.  Only kern/mutex.c changes it and
	 * only the thread itself reads it, so it needs no lock.
	 *
	 * A kill may not retire a thread while this is nonzero: there is no
	 * unlock by proxy (mutex_unlock is owner-only), so the mutex would
	 * stay locked for good -- and the one most often held across a park
	 * is fs_lock.  The kill checks in thread_block_release decline; the
	 * thread finishes and dies at the next check it reaches holding
	 * nothing -- the syscall boundary (which asserts zero) for a user
	 * thread, the next bare park for a kernel one.
	 */
	int			 th_mutex_depth;	/* (self only)      */

	struct thread		*th_runq_link;		/* (sched_lock)     */
	struct thread		*th_task_link;		/* task->t_threads  */
	SLIST_ENTRY(thread)	 th_zombie_link;	/* zombie SLIST     */

	/*
	 * Parked on an object that keeps its own queue of waiters: a Mach
	 * port's receivers or blocked senders, a port set's receivers, a
	 * mutex's waiters.  A thread is on at most one such queue at a time,
	 * so one field serves them all.
	 *
	 * It must not share th_runq_link: the object holds the thread across
	 * the whole park, and a park can end without the object being told
	 * (a deadline expires, the thread goes on the runqueue), after which
	 * a shared field would splice the two lists into one.
	 */
	struct thread		*th_wait_link;		/* (the object's)   */

	/*
	 * Which queue th_wait_link is on: its head, tail and lock, noted by
	 * the thread just before it parks and forgotten by the thread when
	 * it takes itself off.  Nobody else touches them; an extractor that
	 * pops the thread leaves the note, and the woken thread's own
	 * unconditional detach finds nothing and forgets.
	 *
	 * The note exists for the exit that never returns:
	 * thread_block_release retires a killed thread from inside the block,
	 * so the caller's detach never runs, and thread_exit needs to know
	 * which object still names it -- there is no walking every port.
	 */
	struct thread		**th_wait_qhead;	/* (th_wait_qlock)  */
	struct thread		**th_wait_qtail;	/* (th_wait_qlock)  */
	struct spinlock		 *th_wait_qlock;	/* (self only)      */

	/*
	 * The same note for drivers whose waiter "queue" is a single cell
	 * (ata's ch_waiter, kbd_waiter, mouse_waiter, uart_waiter): the
	 * consumer installs itself and parks, and the ISR exchanges the cell
	 * and wakes whoever it got.  thread_slot_note records which cell may
	 * name the thread, around the whole wait loop; thread_exit CASes its
	 * own name out.  Against the ISR's exchange exactly one side gets the
	 * pointer: either the interrupt finds the cell empty, or its wake is
	 * pinned by sched_post_irq_wake's hold until delivered.  Strictly the
	 * thread's own; the cell keeps its own discipline.
	 */
	struct thread	*volatile *th_wait_slot;	/* (self only)      */

	/*
	 * Link on the IRQ wake LIFO.  Its own field for the same reason as
	 * th_wait_link: a queued thread can be woken by someone else and put
	 * on the runqueue before the drain reaches it.
	 */
	struct thread		*th_irq_link;		/* (a) irq_wake_head */

	/*
	 * A wake is a pointer that can outlive its thread.  A waker pops a
	 * thread from a list or slot and calls thread_wake a few lines later,
	 * with the lock dropped; meanwhile the thread can be woken by someone
	 * else (a kill fan-out), exit and be reaped.  th_wake_hold closes the
	 * window: the waker takes it (thread_hold) while the thread is
	 * provably alive -- under the lock its exit path must take, or in the
	 * interrupt that just owned the slot naming it -- and drops it
	 * (thread_unhold) after the wake.  sched_reap_zombies leaves a held
	 * body for a later pass: the pin means "do not free yet", not "do not
	 * die".
	 *
	 * th_irq_queued marks membership of the IRQ wake LIFO, set by
	 * sched_post_irq_wake and cleared by the drain.  Two parties may post
	 * the same thread at once (the deadline walk and a device ISR, on
	 * different CPUs), and a second push would overwrite th_irq_link
	 * while the list still runs through it.  Queued once is enough:
	 * every sleeper re-tests.
	 */
	uint32_t		 th_wake_hold;		/* (a) pins vs reap  */
	volatile int		 th_irq_queued;		/* (a) on the LIFO   */

	/*
	 * Sleep-queue link: threads parked on a channel (thread_block with a
	 * non-NULL target), so sched_wakeup finds them by what they wait for.
	 */
	struct thread		*th_sleep_link;		/* (sched_lock)     */

	/*
	 * When this thread was last made READY out of BLOCKED, in
	 * clock_uptime_ms().  Read by the thread itself when it resumes, to
	 * measure wake latency.
	 */
	uint64_t		 th_wake_ms;		/* (sched_lock)     */

	/*
	 * A wake that arrived before the sleep.  On SMP a thread can be
	 * between deciding to sleep and committing to BLOCKED when another
	 * CPU's wake arrives; thread_wake then finds it READY or RUNNING and
	 * sets this instead of doing nothing, and the next
	 * thread_block_release reads it and does not sleep.  The caller
	 * re-tests its condition, as every sleeper here does.
	 */
	volatile int		 th_wake_pending;	/* (sched_lock)     */

	/*
	 * Why a wake was slow, recorded with th_wake_ms: how many threads
	 * were queued ahead, and who held the CPU.
	 */
	const char		*th_wake_hog;		/* (sched_lock)     */
	uint32_t		 th_wake_qlen;		/* (sched_lock)     */

	/*
	 * Trusted-send flag.  Set by mach_msg_send_trusted around a kernel
	 * send that ships kernel-rodata bytes through an OOL descriptor (e.g.
	 * the "man" service's replies); send_capture_ool then skips its
	 * user-VA range check.  Must never be exposed to userspace.
	 */
	bool			 th_trusted_send;

	/*
	 * Set while a kernel object's dispatcher runs on this thread, in the
	 * sender's context (mach_msg_send): the sends it makes, its replies,
	 * are the kernel's and not held to the destination's queue limit.
	 */
	bool			 th_kernel_send;

	/*
	 * Deadline for a timed park (mach_msg_recv_timed, sched_nap_ms, ...):
	 * th_wake_deadline_ms is absolute clock_uptime_ms(), and
	 * sched_check_timeouts posts an IRQ wake once it has passed.
	 * th_timed_out tells the woken thread its timer expired rather than
	 * its event arriving.  th_timed_link threads the deadline list.
	 */
	uint64_t		 th_wake_deadline_ms;
	volatile int		 th_timed_out;
	struct thread		*th_timed_link;

	/*
	 * CPU time, in TSC cycles.  The running thread's cycles since
	 * th_cpu_mark are charged at every switch and at both ends of a
	 * syscall: to th_stime while th_in_sys, else to th_utime, so a user
	 * thread's interrupts count as its user time.  Written by the thread
	 * itself with preemption off, and th_cpu_mark by the CPU switching to
	 * it; others only read.
	 */
	uint64_t		 th_cpu_mark;
	uint64_t		 th_utime;
	uint64_t		 th_stime;
	bool			 th_in_sys;

	/*
	 * WITNESS-lite per-thread held-locks stack: each entry is the lock's
	 * class name (compared by identity, not strcmp) and the RIP that
	 * acquired it.  Pushed by spin_lock, popped by spin_unlock; used for
	 * lock-order checking and by `s locks' in ddb and panic dumps.
	 */
#define	THREAD_HELD_LOCKS_MAX	8
	struct witness_held {
		const char	*wh_name;
		uintptr_t	 wh_ra;
	}			 th_held[THREAD_HELD_LOCKS_MAX];
	uint8_t			 th_held_count;

	/*
	 * Per-thread exception ports, shaped like the task's t_exc_ports.
	 * user_fault_die tries this slot first and falls back to the task's
	 * only when it is NULL, as in Mach (e.g. a debugger watching one
	 * thread).  Written under th_lock, and only by the thread itself;
	 * user_fault_die, running on that thread, reads without it.
	 *
	 * One SEND ref per non-NULL slot, dropped on replace and at reap
	 * (sched_reap_zombies, before kfree).
	 */
	struct port		*th_exc_ports[EXC_TYPE_COUNT];

	/*
	 * x87/SSE register file (FXSAVE area), saved and restored by
	 * thread_switch_asm on every switch.  16-byte aligned for
	 * FXSAVE/FXRSTOR (the thread is kmalloc'd 16-aligned; the attribute
	 * pins the offset).  Seeded by fpu_clean_state at create.
	 */
	uint8_t			 th_fpu[512] __attribute__((aligned(16)));
};

/*
 * `current_thread' is this CPU's cp_curthread, reached through the GS base
 * and defined in machine/cpu.h (included above).  It is an lvalue; the
 * scheduler assigns to it.
 */

#define	THREAD_DEFAULT_KSTACK	(16 * 1024)	/* 4 pages */

void		thread_subsystem_init(void);

/*
 * Give the context already running a thread structure and make it this
 * CPU's current thread -- what a CPU needs before it can switch away and
 * join the scheduler: the boot CPU in kmain, each AP on its trampoline
 * stack.  The stack stays the caller's and is never freed by the reaper.
 * Returns NULL only if there is no memory for the structure.
 */
struct thread	*thread_adopt_current(struct task *, const char *name);

struct thread	*thread_create(struct task *,
		    void (*entry)(void *), void *arg, const char *name);
void		thread_start(struct thread *);
void		thread_exit(void) __attribute__((noreturn));

/*
 * The waiter-list note (th_wait_qhead in struct thread).  note: record the
 * object list this thread is about to park on, with the list's lock held,
 * right after linking in.  forget: clear it, by the thread itself after its
 * unconditional detach.  unbind_locked: take the thread off the noted list
 * and forget, with the noted lock held -- the pre-park kill exit, where the
 * lock the caller parked under is the list's and is still held.
 */
void		thread_wait_note(struct thread *, struct thread **qhead,
		    struct thread **qtail, struct spinlock *qlock);
void		thread_wait_forget(struct thread *);
void		thread_wait_unbind_locked(struct thread *);

/*
 * The waker's claim against reap (th_wake_hold).  hold: pin the thread's
 * memory -- legal only while it is provably alive: under the lock its exit
 * path must take to leave the list it was popped from, or in the IRQ that
 * just exchanged the slot naming it.  unhold: release the pin after the
 * thread_wake it was taken for.
 */
void		thread_hold(struct thread *);
void		thread_unhold(struct thread *);

/*
 * The slot note (see th_wait_slot above): note around a driver wait loop
 * whose ISR wakes by exchanging a single waiter cell, forget before every
 * return from it.  thread_exit settles what the note still owes by CASing
 * the thread's own name out of the cell.
 */
void		thread_slot_note(struct thread *, struct thread *volatile *);
void		thread_slot_forget(struct thread *);

const char	*thread_state_name(enum thread_state);
const char	*thread_block_reason_name(enum thread_block_reason);
void		thread_print(struct thread *);

/*
 * Install `port' into every slot of th->th_exc_ports named by
 * `types_mask', with task_set_exception_ports' semantics and ref
 * discipline; port == NULL clears the slots.  Returns MACH_MSG_OK, or
 * MACH_E_INVAL for a NULL thread or a bad mask.  `th' must be
 * current_thread: user_fault_die reads the slots without th_lock.
 */
int		thread_set_exception_ports(struct thread *th,
		    uint32_t types_mask, struct port *port);

/* defined in switch.S */
void		thread_switch_asm(uint64_t *old_rsp_save, uint64_t new_rsp,
		    void *old_fpu, void *new_fpu);

#endif /* !_SYS_THREAD_H_ */
