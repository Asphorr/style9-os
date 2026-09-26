/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_WITNESS_H_
#define	_SYS_WITNESS_H_

#include <stdint.h>

/*
 * WITNESS-lite: lock-order cycle detector for spinlocks, after FreeBSD's
 * WITNESS in miniature.
 *
 *	Each spinlock's class is its name pointer (spin_init's `name',
 *	expected to be a string literal: same literal, same class).
 *	Every spin_lock records the edge "<each held class> -> <new
 *	class>" in a global N x N matrix.  Acquiring C while holding H
 *	when the edge C -> H has already been seen closes a cycle, and
 *	panics with the held stack.
 *
 *	Nested same-class acquires (two threads' th_locks, both named
 *	"thread") are allowed: the edge work is skipped.
 *
 *	The recursive-acquire check is spinlock.c's, not duplicated here.
 *
 * Every update runs with interrupts disabled on the calling CPU, so an
 * interrupt handler taking a lock cannot tear it.  Across CPUs, class
 * registration is serialised and edges are atomic (witness.c).
 *
 * Always compiled in; removing the two hook calls in spinlock.c turns it
 * off.
 */

struct spinlock;
struct thread;

void	witness_acquired(struct spinlock *sl, uintptr_t ra);
void	witness_released(struct spinlock *sl);

/* Dump the held-locks stack of `th' (for panic / ddb). */
void	witness_dump_held(struct thread *th);

#endif /* !_SYS_WITNESS_H_ */
