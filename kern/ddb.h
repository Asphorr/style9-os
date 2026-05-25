/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DDB_H_
#define	_SYS_DDB_H_

#include <stdint.h>

#include "intr.h"		/* struct trapframe */

/*
 * Tiny in-kernel debugger.
 *
 * ddb_enter() is called from intr_panic / panic after the initial dump
 * has been printed.  It opens a polled (IRQ-free) read loop on the
 * keyboard and dispatches single-line commands until the operator
 * types 'q'.  The function never returns; on 'q' it just halts.
 *
 *	entry_rbp	rbp at the moment the trigger fired (intr_panic
 *			passes tf->tf_rbp; panic captures its own rbp).
 *			Used by the 'bt' command so backtraces start at
 *			the relevant frame rather than inside the
 *			debugger.
 *	tf		may be NULL when entered from a non-trap panic;
 *			the 'r' command degrades gracefully in that case.
 */
void	ddb_enter(uint64_t entry_rbp, const struct trapframe *tf)
	    __attribute__((noreturn));

#endif /* !_SYS_DDB_H_ */
