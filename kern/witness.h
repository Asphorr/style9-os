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
 *	locks with the same literal address share a class).  Every time
 *	spin_lock is called, we record the edge "<every currently-held
 *	class> -> <new class>" in a global N x N bitmap.  If we are
 *	about to acquire C while holding H, and the bitmap already has
 *	the edge C -> H (i.e. somewhere earlier the kernel grabbed H
 *	while holding C), we are about to close a deadlock cycle --
 *	panic with the held stack so the offending pair is visible.
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
 * outer acquire's witness update.  The window is microscopic and we
 * are single-CPU, so spinlock + cli/sti is sufficient.
 *
 * Disabled-by-default knob is intentional: turn it off in production
 * builds by removing the spin_lock hook calls.  We compile it in
 * unconditionally for now (the hobby kernel is its own debug build).
 */

struct spinlock;
struct thread;

void	witness_acquired(struct spinlock *sl, uintptr_t ra);
void	witness_released(struct spinlock *sl);

/* Dump the calling thread's held-locks stack (for panic / ddb). */
void	witness_dump_held(struct thread *th);

#endif /* !_SYS_WITNESS_H_ */
