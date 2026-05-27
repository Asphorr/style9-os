/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "ksym.h"
#include "panic.h"
#include "spinlock.h"
#include "thread.h"
#include "witness.h"

/*
 * Storage:
 *
 *	witness_classes[]	first-seen ordering of lock-name pointers,
 *				so a class id is just the array index.
 *
 *	witness_edges[i][j]	1 byte == 1 if "class i acquired-before
 *				class j" has been observed somewhere in
 *				the running kernel.  A new acquire of j
 *				while holding i panics if edges[j][i]
 *				is already set (would close a cycle).
 *
 * Both are mutated only with IRQs disabled in the calling context, so
 * the racing-IRQ-handler-also-takes-a-lock case can't tear them.
 */
#define	WITNESS_MAX_CLASSES	48

static const char	*witness_classes[WITNESS_MAX_CLASSES];
static uint8_t		 witness_nclasses;
static uint8_t		 witness_edges[WITNESS_MAX_CLASSES][WITNESS_MAX_CLASSES];

static int		 class_of(const char *name);
static uint64_t		 irq_disable_save(void);
static void		 irq_restore(uint64_t flags);
static void		 witness_panic_cycle(const char *new_name,
			    uintptr_t new_ra, const char *held_name,
			    uintptr_t held_ra)
			    __attribute__((noreturn));

void
witness_acquired(struct spinlock *sl, uintptr_t ra)
{
	struct thread	*me;
	const char	*new_name;
	uint64_t	 flags;
	int		 new_class;
	uint8_t		 i;

	me = current_thread;
	if (me == NULL)
		return;	/* pre-scheduler boot path */

	new_name = sl->sl_name != NULL ? sl->sl_name : "(anon)";

	flags = irq_disable_save();

	new_class = class_of(new_name);
	if (new_class < 0) {
		/* Out of class slots; degrade silently. */
		irq_restore(flags);
		return;
	}

	for (i = 0; i < me->th_held_count; i++) {
		const char	*held_name = me->th_held[i].wh_name;
		int		 held_class;

		held_class = class_of(held_name);
		if (held_class < 0)
			continue;
		if (held_class == new_class)
			continue;	/* nested same-class is fine */

		/*
		 * Cycle check.  If the reverse edge (new -> held) is
		 * already in the matrix, this acquire would close a
		 * cycle.  Drop into the loud autopsy path.
		 */
		if (witness_edges[new_class][held_class]) {
			irq_restore(flags);
			witness_panic_cycle(new_name, ra,
			    held_name, me->th_held[i].wh_ra);
		}

		/* Forward edge: record this observed order. */
		witness_edges[held_class][new_class] = 1;
	}

	if (me->th_held_count < THREAD_HELD_LOCKS_MAX) {
		me->th_held[me->th_held_count].wh_name = new_name;
		me->th_held[me->th_held_count].wh_ra   = ra;
		me->th_held_count++;
	}
	/* Silently truncate if held-stack is full -- diagnostic, non-fatal. */

	irq_restore(flags);
}

void
witness_released(struct spinlock *sl)
{
	struct thread	*me;
	const char	*name;
	uint64_t	 flags;
	int		 i;

	me = current_thread;
	if (me == NULL)
		return;

	name = sl->sl_name != NULL ? sl->sl_name : "(anon)";

	flags = irq_disable_save();

	if (me->th_held_count == 0) {
		irq_restore(flags);
		return;
	}

	/*
	 * LIFO release is the common case; try the top first, fall
	 * through to a search for out-of-order releases (legal, just
	 * less common).
	 */
	if (me->th_held[me->th_held_count - 1].wh_name == name) {
		me->th_held_count--;
		irq_restore(flags);
		return;
	}

	for (i = (int)me->th_held_count - 1; i >= 0; i--) {
		if (me->th_held[i].wh_name == name) {
			int j;
			for (j = i; j < (int)me->th_held_count - 1; j++)
				me->th_held[j] = me->th_held[j + 1];
			me->th_held_count--;
			break;
		}
	}

	irq_restore(flags);
}

void
witness_dump_held(struct thread *th)
{
	uint8_t	i;

	if (th == NULL || th->th_held_count == 0) {
		kprintf("witness: thread holds no locks\n");
		return;
	}

	kprintf("witness: thread %llu (%s) holds %u lock(s):\n",
	    (unsigned long long)th->th_id,
	    th->th_name != NULL ? th->th_name : "?",
	    (unsigned)th->th_held_count);
	for (i = 0; i < th->th_held_count; i++) {
		kprintf("  [%u] %s acquired at ", (unsigned)i,
		    th->th_held[i].wh_name);
		ksym_print((uint64_t)th->th_held[i].wh_ra);
		kprintf("\n");
	}
}

/* ---- internals --------------------------------------------------- */

/*
 * Linear scan over witness_classes by pointer identity.  Class names
 * come from spin_init's `name` arg which is expected to be a string
 * literal -- same literal across the kernel => same pointer => same
 * class.  Auto-registers on first sight.
 *
 * Caller must hold the "IRQ disabled" invariant (the only mutator of
 * witness_nclasses + witness_classes is this routine, called only
 * from witness_acquired/released which both disable IRQ around it).
 */
static int
class_of(const char *name)
{
	uint8_t	i;

	for (i = 0; i < witness_nclasses; i++) {
		if (witness_classes[i] == name)
			return ((int)i);
	}
	if (witness_nclasses >= WITNESS_MAX_CLASSES)
		return (-1);
	witness_classes[witness_nclasses] = name;
	return ((int)witness_nclasses++);
}

static uint64_t
irq_disable_save(void)
{
	uint64_t	flags;

	__asm__ __volatile__ (
	    "pushfq\n\t"
	    "popq %0\n\t"
	    "cli"
	    : "=r"(flags)
	    :
	    : "memory");
	return (flags);
}

static void
irq_restore(uint64_t flags)
{

	if (flags & 0x200u)
		__asm__ __volatile__ ("sti" : : : "memory");
}

static void
witness_panic_cycle(const char *new_name, uintptr_t new_ra,
    const char *held_name, uintptr_t held_ra)
{
	struct thread	*me;

	me = current_thread;

	kprintf("\n*** witness: lock-order cycle detected\n");
	kprintf("  attempted to acquire '%s' at ", new_name);
	ksym_print((uint64_t)new_ra);
	kprintf("\n");
	kprintf("  while holding   '%s' at ", held_name);
	ksym_print((uint64_t)held_ra);
	kprintf("\n");
	kprintf("  reverse edge ('%s' -> '%s') was already observed elsewhere;\n",
	    new_name, held_name);
	kprintf("  acquiring this lock here would close a deadlock cycle.\n");

	witness_dump_held(me);

	panic("witness: lock-order cycle '%s' <-> '%s'",
	    new_name, held_name);
}
