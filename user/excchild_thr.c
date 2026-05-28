/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Thread-level vs task-level exception precedence test.  This child
 * installs the parent-injected port at BOTH the task-level
 * BAD_INSTRUCTION slot AND the thread-level BAD_INSTRUCTION slot, then
 * deliberately executes UD2.  user_fault_die checks thread-level
 * first; if precedence is correct the message is posted there and
 * the task-level slot is left alone -- exactly one MACH_EXC_FAULT
 * lands in the parent's queue.  The parent recv_timed for a second
 * message with a short timeout and asserts MACH_E_TIMEOUT; the
 * timeout proves the task-level slot did NOT also fire.
 */

#include "style9.h"

int
main(void)
{
	int	rv;

	rv = task_set_exception_ports(EXC_MASK_BAD_INSTRUCTION,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK)
		return (1);

	rv = thread_set_exception_ports(EXC_MASK_BAD_INSTRUCTION,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK)
		return (2);

	/* #UD via UD2.  Thread-level slot wins. */
	__asm __volatile("ud2");

	/* Unreachable. */
	return (0);
}
