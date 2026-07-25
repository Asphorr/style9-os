/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * oolchild -- the sending half of the out-of-line memory test, and the only
 * thing in the system that sends bulk data from one ring-3 task to another.
 *
 * That matters more than it sounds.  A message to a kernel service never
 * reaches the capture path at all: those destinations are special ports, and
 * their dispatchers read the sender's bytes straight out of its address
 * space.  A message from a kernel thread is captured but never shared --
 * a kernel address is in no task's map, so there are no page tables to
 * consult and nothing to write-protect.  User to user through a real port
 * queue is the one case where the kernel can hand the receiver the sender's
 * own frames, and before this program there was no such case, so the code
 * that does it ran zero times per boot.
 *
 * Spawned by hello.elf with a SEND right to the parent's work port injected
 * at MACH_PORT_PARENT.  Fills a page-aligned, whole-page buffer, sends it,
 * and then deliberately destroys it.
 */

#include "style9.h"

#define	OOLCHILD_PAGES	3
#define	OOLCHILD_BYTES	(OOLCHILD_PAGES * 4096u)
#define	OOLCHILD_TAG	0x00010CA1u

/*
 * The pattern both halves know.  The parent recomputes it from this formula
 * rather than believing a checksum the child sends along, so a child that
 * gets its own arithmetic wrong cannot make the two agree.
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
	 * Page-aligned and a whole number of pages, which is what makes the
	 * payload shareable at all: the message's page boundaries have to
	 * coincide with this task's, and a partial last page can never be
	 * handed over because the bytes past the end of the buffer are none
	 * of the receiver's business.
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
	 * THE POINT OF THIS PROGRAM.
	 *
	 * The message is posted and the parent has not read it yet.  Scribble
	 * over every byte of what was just sent.
	 *
	 * If the kernel handed the parent these frames and left this task
	 * able to write them, the loop below is editing a message already in
	 * the mail -- and the parent, checking against the pattern it knows,
	 * says so.  If it write-protected them, each store here faults, this
	 * task quietly gets a private copy, and the parent still receives
	 * what was true at the instant of the send.
	 *
	 * Doing it before the parent could plausibly have run is deliberate:
	 * a test that overwrites after the receive proves nothing, because by
	 * then there is nothing left to corrupt.
	 */
	for (i = 0; i < OOLCHILD_BYTES; i++)
		buf[i] = 0xA5u;

	return (0);
}
