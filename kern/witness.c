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
 *	witness_edges[i][j]	1 if "class i acquired before class j"
 *				has been observed.  Acquiring j while
 *				holding i panics if edges[j][i] is
 *				already set (it would close a cycle).
 *
 * A class slot, once published, never changes: registration fills
 * witness_classes[n] under witness_reg_lock and then stores n + 1 into
 * witness_nclasses with release, and lookups load the count with acquire
 * and need no lock.  Edges only ever go from 0 to 1 and are relaxed
 * atomics; an edge another CPU is setting at the same moment may be seen
 * one acquire late.
 */
#define	WITNESS_MAX_CLASSES	48

static const char	*witness_classes[WITNESS_MAX_CLASSES];
static uint8_t		 witness_nclasses;
static uint8_t		 witness_edges[WITNESS_MAX_CLASSES][WITNESS_MAX_CLASSES];
/*
 * A bare test-and-set lock, not a struct spinlock: taking one of those
 * would call back into witness.
 */
static unsigned int	 witness_reg_lock;

static int		 class_find(const char *name, unsigned int n);
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

		/* The reverse edge (new -> held) already seen: a cycle. */
		if (__atomic_load_n(&witness_edges[new_class][held_class],
		    __ATOMIC_RELAXED) != 0) {
			irq_restore(flags);
			witness_panic_cycle(new_name, ra,
			    held_name, me->th_held[i].wh_ra);
		}

		/* Forward edge: record this observed order. */
		__atomic_store_n(&witness_edges[held_class][new_class], 1,
		    __ATOMIC_RELAXED);
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

	/* LIFO release is the common case; out of order is legal too. */
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

/* Index of `name' among the first `n' published classes, or -1. */
static int
class_find(const char *name, unsigned int n)
{
	unsigned int	i;

	for (i = 0; i < n; i++) {
		if (witness_classes[i] == name)
			return ((int)i);
	}
	return (-1);
}

/*
 * Class id of `name', by pointer identity, registering it on first
 * sight; -1 when the table is full.  Called with interrupts disabled.
 * Two CPUs meeting a new class at once both reach the lock; the second
 * finds the first one's slot on its rescan instead of taking another.
 */
static int
class_of(const char *name)
{
	unsigned int	n;
	int		id;

	id = class_find(name,
	    __atomic_load_n(&witness_nclasses, __ATOMIC_ACQUIRE));
	if (id >= 0)
		return (id);

	while (__atomic_exchange_n(&witness_reg_lock, 1u,
	    __ATOMIC_ACQUIRE) != 0)
		__asm__ __volatile__ ("pause");
	n  = witness_nclasses;
	id = class_find(name, n);
	if (id < 0 && n < WITNESS_MAX_CLASSES) {
		witness_classes[n] = name;
		__atomic_store_n(&witness_nclasses, (uint8_t)(n + 1),
		    __ATOMIC_RELEASE);
		id = (int)n;
	}
	__atomic_store_n(&witness_reg_lock, 0u, __ATOMIC_RELEASE);
	return (id);
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
