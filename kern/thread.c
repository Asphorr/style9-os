/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "sched.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

/* Defined in sched.c; released by trampoline on first dispatch. */
extern void	sched_post_switch_unlock(void);

struct thread		*current_thread;

static struct spinlock	threads_lock = SPINLOCK_INIT("threads-global");
static uint64_t		next_thread_id;

static void	thread_trampoline(void);

void
thread_subsystem_init(void)
{
	struct thread	*boot;

	next_thread_id = 1;

	if (kernel_task == NULL)
		panic("thread_subsystem_init: kernel_task not created");

	/*
	 * Synthesise a thread structure for whoever is running kmain
	 * right now.  It re-uses the boot stack (no kmalloc here), and
	 * its rsp_save is meaningless until the first context switch
	 * AWAY from this thread -- at that point the switch asm fills
	 * it in.
	 */
	boot = (struct thread *)kmalloc(sizeof(*boot));
	if (boot == NULL)
		panic("thread_subsystem_init: kmalloc(boot thread) failed");

	spin_init(&boot->th_lock, "thread");
	boot->th_id              = next_thread_id++;
	boot->th_name            = "boot";
	boot->th_task            = kernel_task;
	boot->th_state           = THREAD_RUNNING;
	boot->th_block_reason    = THREAD_NOT_BLOCKED;
	boot->th_block_target    = NULL;
	boot->th_rsp_save        = 0;
	boot->th_kstack_base     = NULL;	/* boot stack -- not ours to free */
	boot->th_kstack_size     = 0;
	boot->th_kstack_owned    = false;
	boot->th_entry           = NULL;
	boot->th_arg             = NULL;
	boot->th_runq_link       = NULL;
	boot->th_task_link       = NULL;
	boot->th_zombie_next     = NULL;

	current_thread = boot;
	task_attach_thread(kernel_task, boot);

	kprintf("thread: boot thread id=%llu attached to task %s\n",
	    (unsigned long long)boot->th_id, kernel_task->t_name);
}

/*
 * Stack layout the first context switch into a brand-new thread will
 * encounter (lowest address at top, which is where th_rsp_save points):
 *
 *	th_rsp_save -> [ r15 = 0     ]   popped by switch.S
 *	               [ r14 = 0     ]
 *	               [ r13 = 0     ]
 *	               [ r12 = 0     ]
 *	               [ rbx = 0     ]
 *	               [ rbp = 0     ]
 *	               [ RIP = trampoline ]   ret target
 *	               [ ...stack grows down for the trampoline's use... ]
 *
 * After the asm's 6 pops and ret, the new thread enters
 * thread_trampoline() with a freshly-zeroed register file and RSP just
 * above the saved RIP slot.  The trampoline picks up current_thread
 * (set by sched_run_next before the switch) and invokes the entry fn.
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

	th = (struct thread *)kmalloc(sizeof(*th));
	if (th == NULL)
		return (NULL);

	kstack = (uint8_t *)kmalloc(THREAD_DEFAULT_KSTACK);
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
	th->th_runq_link         = NULL;
	th->th_task_link         = NULL;
	th->th_zombie_next       = NULL;

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
thread_exit(void)
{
	struct thread	*me;

	me = current_thread;
	KASSERT(me != NULL, "thread_exit: no current thread");

	spin_lock(&me->th_lock);
	me->th_state = THREAD_ZOMBIE;
	spin_unlock(&me->th_lock);

	sched_handoff_zombie(me);
	/* NOTREACHED */
	panic("thread_exit: returned from sched_handoff_zombie");
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

void
thread_print(struct thread *th)
{

	kprintf("thread id=%llu name=%-12s state=%-7s task=%s\n",
	    (unsigned long long)th->th_id, th->th_name,
	    thread_state_name(th->th_state),
	    th->th_task != NULL ? th->th_task->t_name : "?");
}

/*
 * First instructions every brand-new thread executes after the very
 * first switch into it.  Lives at the top of the fake call frame.
 * Picks up the entry function and argument from the thread struct;
 * if entry returns we call thread_exit() so a misbehaving worker
 * doesn't run off the end of its stack.
 */
static void
thread_trampoline(void)
{
	struct thread	*me;

	/*
	 * The switch into us happened with sched_lock held by the
	 * previous thread.  We are responsible for releasing it before
	 * running anything else; otherwise the next thread_yield would
	 * try to acquire it recursively and panic.
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
