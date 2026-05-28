/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Reply-protocol child.  Installs the parent-injected port at the
 * BAD_INSTRUCTION slot WITH the EXC_FLAG_RESUMABLE flag, opting into
 * the kernel's reply protocol.  Then executes UD2; user_fault_die
 * posts the exception to the watcher AND parks this thread waiting
 * for a verdict.  When the watcher replies EXC_VERDICT_RESUME with
 * rip_advance=2, the kernel skips past the UD2 and resumes execution
 * here.  We confirm the resume worked by sending a tagged "I survived"
 * message back to the watcher via MACH_PORT_PARENT.
 *
 * Without the RESUMABLE flag the kernel would retire the thread
 * after delivering the exception (A v1 / per-type / thread-level
 * behavior); the flag is the explicit opt-in.
 */

#include "style9.h"

#define	RESUME_SURVIVE_TAG	0xC0DEBA5Eu

int
main(void)
{
	struct mach_msg_header	ping;
	int			rv;

	rv = task_set_exception_ports(
	    EXC_MASK_BAD_INSTRUCTION | EXC_FLAG_RESUMABLE,
	    MACH_PORT_PARENT);
	if (rv != MACH_MSG_OK)
		return (1);

	/* #UD via UD2.  Kernel parks us; watcher's reply resumes us past it. */
	__asm __volatile("ud2");

	/*
	 * Post-resume: tell the watcher we made it.  Uses the same
	 * MACH_PORT_PARENT SEND right (still in our name table; the
	 * exception port took its own ref, didn't consume ours).
	 */
	ping.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	ping.msgh_size    = sizeof(ping);
	ping.msgh_remote  = MACH_PORT_PARENT;
	ping.msgh_local   = MACH_PORT_NULL;
	ping.msgh_voucher = 0;
	ping.msgh_id      = RESUME_SURVIVE_TAG;
	(void)mach_msg_send(&ping);
	return (0);
}
