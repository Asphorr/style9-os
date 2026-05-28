/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Ring-3 demo for style9-os, now linked against libstyle9.
 *
 * Steps (each demos one layer of the user/kernel IPC plumbing):
 *	1. printf banner via SYS_PRINT
 *	2. mach_port_allocate + self-send + recv round-trip
 *	3. mach_msg_recv_timed on an empty port (expect E_TIMEOUT)
 *	4. task_self GET_INFO RPC (sync dispatcher in the kernel)
 *	5. bootstrap_lookup("kernel_task") + GET_INFO chained call
 */

#include "style9.h"

#define	DEMO_TAG	0xCAFEBABEu

static int
demo_round_trip(void)
{
	struct mach_msg_header	tx;
	struct mach_msg_header	rx;
	mach_port_name_t	name;
	int			rv;

	name = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (name == MACH_PORT_NULL) {
		printf("  port_allocate failed\n");
		return (1);
	}
	printf("  allocated port = 0x%x\n", (uint32_t)name);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = name;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = DEMO_TAG;

	rv = mach_msg_send(&tx);
	if (rv != MACH_MSG_OK) {
		printf("  msg_send failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (2);
	}
	printf("  self-send queued\n");

	rv = mach_msg_recv(name, &rx, sizeof(rx));
	if (rv != MACH_MSG_OK) {
		printf("  msg_recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (3);
	}
	if (rx.msgh_id != DEMO_TAG) {
		printf("  TAG MISMATCH: got 0x%x expected 0x%x\n",
		    rx.msgh_id, DEMO_TAG);
		(void)mach_port_deallocate(name);
		return (4);
	}
	printf("  mach_msg round-trip via SYSCALL: OK\n");

	/* Empty-port timeout probe. */
	rv = mach_msg_recv_timed(name, &rx, sizeof(rx), 50);
	if (rv == MACH_MSG_OK) {
		printf("  recv_timed unexpectedly returned a message\n");
		(void)mach_port_deallocate(name);
		return (5);
	}
	if (rv != MACH_E_TIMEOUT)
		printf("  recv_timed odd rv = %d\n", rv);
	else
		printf("  recv_timed returned E_TIMEOUT after 50 ms: OK\n");

	(void)mach_port_deallocate(name);
	return (0);
}

static int
demo_task_self(void)
{
	struct mach_msg_header	tx;
	struct {
		struct mach_msg_header	hdr;
		struct task_info_reply	body;
	} reply;
	int	rv;

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = MACH_PORT_TASK_SELF;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = TASK_OP_GET_INFO;

	rv = mach_msg_rpc(&tx, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  task_self GET_INFO rpc failed (rv=%d)\n", rv);
		return (6);
	}
	printf("  task_self GET_INFO ok: name='%s' tir_task_id=%llu\n",
	    reply.body.tir_name,
	    (unsigned long long)reply.body.tir_task_id);
	return (0);
}

/*
 * OOL round-trip: build a user-VA buffer with a known pattern, send it
 * as the sole OOL descriptor in a complex message to svc/echool, expect
 * the kernel-side FNV-1a checksum to come back in msgh_id and match
 * what we computed locally.  Validates the wire format end-to-end from
 * ring 3: the body's descriptor-count, the OOL type tag at byte 0, the
 * size and address fields, and the kernel's special-port intercept all
 * have to line up for this to pass.
 */
static uint32_t
demo_ool_fnv1a(const uint8_t *buf, uint32_t size)
{
	uint32_t	h, i;

	h = 0x811C9DC5u;
	for (i = 0; i < size; i++) {
		h ^= (uint32_t)buf[i];
		h *= 0x01000193u;
	}
	return (h);
}

static int
demo_ool_roundtrip(void)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	} req;
	struct mach_msg_header	reply;
	uint8_t			buf[512];
	mach_port_name_t	svc;
	uint32_t		i, expected;
	int			rv;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
	expected = demo_ool_fnv1a(buf, sizeof(buf));

	svc = bootstrap_lookup(SVC_ECHOOL_NAME);
	if (svc == MACH_PORT_NULL) {
		printf("  bootstrap_lookup('echool') failed\n");
		return (9);
	}

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0)
	    | MACH_MSGH_BITS_COMPLEX;
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = ECHOOL_OP_CHECKSUM;

	req.body.msgh_descriptor_count = 1;

	req.ool.type       = MACH_MSG_OOL_DESCRIPTOR;
	req.ool.copy       = MACH_MSG_PHYSICAL_COPY;
	req.ool.deallocate = 0;
	req.ool.pad        = 0;
	req.ool.size       = (uint32_t)sizeof(buf);
	req.ool.address    = (uint64_t)(uintptr_t)buf;

	rv = mach_msg_rpc(&req.hdr, &reply, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("  echool rpc failed (rv=%d)\n", rv);
		return (10);
	}
	if (reply.msgh_id != expected) {
		printf("  OOL checksum MISMATCH: kernel=0x%x expected=0x%x\n",
		    (unsigned)reply.msgh_id, (unsigned)expected);
		return (11);
	}
	printf("  OOL round-trip %u bytes via echool: fnv1a=0x%x OK\n",
	    (unsigned)sizeof(buf), (unsigned)expected);
	return (0);
}

static int
demo_bootstrap_chain(void)
{
	struct mach_msg_header	tx;
	struct {
		struct mach_msg_header	hdr;
		struct task_info_reply	body;
	} info_reply;
	mach_port_name_t	svc;
	int			rv;

	svc = bootstrap_lookup("kernel_task");
	if (svc == MACH_PORT_NULL) {
		printf("  bootstrap_lookup('kernel_task') failed\n");
		return (7);
	}
	printf("  bootstrap_lookup('kernel_task') -> name=0x%x\n",
	    (uint32_t)svc);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = svc;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = TASK_OP_GET_INFO;

	rv = mach_msg_rpc(&tx, &info_reply.hdr, sizeof(info_reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("  GET_INFO via bootstrap name failed (rv=%d)\n", rv);
		return (8);
	}
	printf("  GET_INFO via bootstrap name: ok, tir_task_id=%llu\n",
	    (unsigned long long)info_reply.body.tir_task_id);
	return (0);
}

int
main(void)
{
	int	rv;

	printf("hello from hello.elf (libstyle9, ring 3)\n");

	rv = demo_round_trip();
	if (rv != 0)
		return (rv);

	rv = demo_task_self();
	if (rv != 0)
		return (rv);

	rv = demo_bootstrap_chain();
	if (rv != 0)
		return (rv);

	rv = demo_ool_roundtrip();
	if (rv != 0)
		return (rv);

	printf("hello.elf: all demos passed\n");
	return (0);
}
