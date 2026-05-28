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
 * WITNESS-lite: lock-order cycle detector for spinlocks.
 *
 * Idea (mirrors FreeBSD's WITNESS, in miniature):
 *
 *	Each spinlock has a "class" identified by its name pointer
 *	(spin_init's `name` arg, expected to be a static string -- two
 *	locks with the same literal address share a class).  Every
 *	spin_lock records the edge "<every currently-held class> ->
 *	<new class>" in a global N x N bitmap.  An acquire of C while
 *	holding H, with the bitmap already carrying the edge C -> H
 *	(i.e. some earlier acquire took H while holding C), closes a
 *	deadlock cycle and panics with the held stack so the offending
 *	pair is visible.
 *
 *	Nested same-class acquires (e.g. walking two different `struct
 *	thread`s, each with its own th_lock named "thread") are
 *	explicitly allowed: same name pointer => skip the edge work.
 *
 *	The recursive-acquire check stays in spin_lock.c -- WITNESS
 *	does not duplicate it.
 *
 * IRQ safety: every state mutation happens with IRQs disabled.  An
 * IRQ handler that itself takes a spinlock would otherwise race the
 * outer acquire's witness update.  The window is microscopic and the
 * kernel is single-CPU, so spinlock + cli/sti suffices.
 *
 * Disabled-by-default knob is intentional: production builds can turn
 * it off by removing the spin_lock hook calls.  The hobby kernel
 * compiles it in unconditionally (the kernel is its own debug build).
 */

struct spinlock;
struct thread;

void	witness_acquired(struct spinlock *sl, uintptr_t ra);
void	witness_released(struct spinlock *sl);

/* Dump the calling thread's held-locks stack (for panic / ddb). */
void	witness_dump_held(struct thread *th);

#endif /* !_SYS_WITNESS_H_ */
