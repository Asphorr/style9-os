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
 *	panic(fmt, ...)		hard stop: emits formatted message, walks
 *				the frame-pointer chain to produce a
 *				backtrace, then halts.  Never returns.
 *
 *	backtrace_print(rbp, n)	walk up to n frames starting from the
 *				supplied RBP.  Caller provides RBP so the
 *				trap dispatcher can backtrace from the
 *				faulting context rather than from itself.
 *
 *	KASSERT(cond, msg)	if cond is false, calls kassert_fail()
 *				which prints the source location and
 *				message and panics.  Cheap to leave on
 *				(one CMP + JNE per call); BSD kernels
 *				ship thousands of these on the theory
 *				that one missed invariant is worth more
 *				than every assertion's cycle cost.
 *
 *	panic_in_progress	non-zero once panic() has started running.
 *				Modules that may be called from the panic
 *				path (e.g. tty, dbgcon) consult this to
 *				bypass any synchronisation that would
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
