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

	struct thread		*th_runq_link;		/* runqueue / waitq */
	struct thread		*th_task_link;		/* task->t_threads  */
	struct thread		*th_zombie_next;	/* reaper list      */

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
};

extern struct thread		*current_thread;	/* per-CPU later    */

#define	THREAD_DEFAULT_KSTACK	(16 * 1024)	/* 4 pages */

void		thread_subsystem_init(void);
struct thread	*thread_create(struct task *,
		    void (*entry)(void *), void *arg, const char *name);
void		thread_start(struct thread *);
void		thread_exit(void) __attribute__((noreturn));

const char	*thread_state_name(enum thread_state);
void		thread_print(struct thread *);

/* defined in switch.S */
void		thread_switch_asm(uint64_t *old_rsp_save, uint64_t new_rsp);

#endif /* !_SYS_THREAD_H_ */
