/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_PANIC_H_
#define	_SYS_PANIC_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Kernel autopsy primitives.
 *
 *	panic(fmt, ...)		hard stop: prints the message and a
 *				frame-pointer backtrace, then enters ddb.
 *				Never returns; a recursive panic halts.
 *
 *	backtrace_print(rbp, n)	walk up to n frames from the supplied
 *				RBP, so the trap dispatcher can start
 *				from the faulting context.
 *
 *	KASSERT(cond, msg)	if cond is false, kassert_fail() panics
 *				with the source location and message.
 *				Always on: one compare and branch.
 *
 *	panic_in_progress	true once panic() has started.  Code the
 *				panic path calls (the tty) checks it to
 *				bypass synchronisation that would
 *				otherwise self-deadlock.
 */

extern volatile bool	panic_in_progress;

void	panic(const char *fmt, ...)
	    __attribute__((noreturn, format(printf, 1, 2)));
void	backtrace_print(uintptr_t rbp, int max_frames);
void	kassert_fail(const char *cond, const char *file, int line,
	    const char *msg) __attribute__((noreturn));

#define	KASSERT(cond, msg)						\
	do {								\
		if (__builtin_expect(!(cond), 0))			\
			kassert_fail(#cond, __FILE__, __LINE__, (msg));	\
	} while (0)

#endif /* !_SYS_PANIC_H_ */
