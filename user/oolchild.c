/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * oolchild -- the sending half of the out-of-line memory test, and the only
 * program that sends bulk data from one ring-3 task to another.
 *
 * Only that case reaches the page-sharing path: a message to a kernel
 * service is read straight out of the sender's address space, and one from
 * a kernel thread is copied, since a kernel address has no task page tables
 * to write-protect.  User to user through a port queue is where the kernel
 * hands the receiver the sender's own frames.
 *
 * Spawned by hello.elf with a send right to the parent's work port at
 * MACH_PORT_PARENT.  Fills a page-aligned, whole-page buffer, sends it,
 * then overwrites it.
 */

#include "style9.h"

#define	OOLCHILD_PAGES	3
#define	OOLCHILD_BYTES	(OOLCHILD_PAGES * 4096u)
#define	OOLCHILD_TAG	0x00010CA1u

/*
 * The pattern both halves know.  The parent recomputes it from the formula
 * instead of trusting a checksum from the child.
 */
static uint8_t
pattern(unsigned i)
{

	return ((uint8_t)((i * 31u + 7u) & 0xFFu));
}

int
main(void)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	}		 msg;
	uint8_t		*buf;
	unsigned	 i;
	int		 rv;

	/*
	 * Page-aligned and whole pages, or the payload cannot be shared: a
	 * partial last page would expose bytes past the buffer's end to the
	 * receiver.
	 */
	buf = (uint8_t *)vm_allocate(OOLCHILD_BYTES,
	    VM_PROT_READ | VM_PROT_WRITE);
	if (buf == NULL) {
		printf("oolchild: vm_allocate(%u) failed\n", OOLCHILD_BYTES);
		return (1);
	}
	for (i = 0; i < OOLCHILD_BYTES; i++)
		buf[i] = pattern(i);

	msg.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	msg.hdr.msgh_size    = sizeof(msg);
	msg.hdr.msgh_remote  = MACH_PORT_PARENT;
	msg.hdr.msgh_local   = MACH_PORT_NULL;
	msg.hdr.msgh_voucher = 0;
	msg.hdr.msgh_id      = OOLCHILD_TAG;
	msg.body.msgh_descriptor_count = 1;
	msg.ool.type       = MACH_MSG_OOL_DESCRIPTOR;
	msg.ool.copy       = MACH_MSG_VIRTUAL_COPY;
	msg.ool.deallocate = 0;
	msg.ool.pad        = 0;
	msg.ool.size       = OOLCHILD_BYTES;
	msg.ool.address    = (uint64_t)(uintptr_t)buf;

	rv = mach_msg_send(&msg.hdr);
	if (rv != MACH_MSG_OK) {
		printf("oolchild: mach_msg_send rv=%d\n", rv);
		return (2);
	}

	/*
	 * The point of this program: overwrite what was just sent, before
	 * the parent has received it.  If the kernel shared these frames
	 * without write-protecting them, this edits the queued message and
	 * the parent's pattern check fails.  If they are protected, each
	 * store faults into a private copy and the parent sees the data as
	 * of the send.  Overwriting after the receive would prove nothing.
	 */
	for (i = 0; i < OOLCHILD_BYTES; i++)
		buf[i] = 0xA5u;

	return (0);
}
