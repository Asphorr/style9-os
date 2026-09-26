/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Reply-protocol child.  Installs the parent-injected port at the
 * BAD_INSTRUCTION slot with EXC_FLAG_RESUMABLE, then executes UD2:
 * user_fault_die posts the exception and parks this thread for a verdict.
 * The watcher replies EXC_VERDICT_RESUME with rip_advance=2, the kernel
 * skips the UD2, and we send a tagged "survived" message back through
 * MACH_PORT_PARENT.  Without the flag the thread would be retired after
 * delivery.
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
	 * Resumed: tell the watcher.  MACH_PORT_PARENT is still ours; the
	 * exception port took its own reference.
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
