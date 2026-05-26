/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * Mach IPC wrappers.
 *
 * One per syscall, plus a `bootstrap_lookup` convenience that bundles
 * the well-known sequence "build BOOTSTRAP_OP_LOOKUP request -> rpc
 * to MACH_PORT_BOOTSTRAP -> decode the port_descriptor in the reply"
 * since every program that talks to a kernel service needs it.
 */

mach_port_name_t
mach_port_allocate(uint8_t rights)
{
	long	rv;

	rv = syscall1(SYS_PORT_ALLOC, (long)rights);
	if (rv <= 0)
		return (MACH_PORT_NULL);
	return ((mach_port_name_t)rv);
}

int
mach_port_deallocate(mach_port_name_t name)
{

	return ((int)syscall1(SYS_PORT_DEALLOC, (long)name));
}

int
mach_msg_send(const struct mach_msg_header *msg)
{

	return ((int)syscall1(SYS_MSG_SEND, (long)msg));
}

int
mach_msg_recv(mach_port_name_t name, struct mach_msg_header *buf,
    size_t buf_size)
{

	return ((int)syscall3(SYS_MSG_RECV,
	    (long)name, (long)buf, (long)buf_size));
}

int
mach_msg_recv_timed(mach_port_name_t name, struct mach_msg_header *buf,
    size_t buf_size, uint64_t timeout_ms)
{

	return ((int)syscall4(SYS_MSG_RECV_TIMED,
	    (long)name, (long)buf, (long)buf_size, (long)timeout_ms));
}

int
mach_msg_rpc(struct mach_msg_header *req, struct mach_msg_header *reply,
    size_t reply_size, uint64_t timeout_ms)
{

	return ((int)syscall4(SYS_MSG_RPC,
	    (long)req, (long)reply, (long)reply_size, (long)timeout_ms));
}

/*
 * bootstrap_lookup: ask the well-known bootstrap port for a service
 * name and return the SEND right it hands back.  Returns
 * MACH_PORT_NULL on any error (RPC failure, service-not-found, or a
 * malformed reply).  Caller is responsible for mach_port_deallocate
 * on the returned name.
 */
mach_port_name_t
bootstrap_lookup(const char *service)
{
	struct {
		struct mach_msg_header			hdr;
		struct bootstrap_lookup_request		body;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply;
	size_t	i;
	int	rv;

	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		req.body.blr_name[i] = '\0';
	for (i = 0; service[i] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; i++)
		req.body.blr_name[i] = service[i];

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = MACH_PORT_BOOTSTRAP;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = BOOTSTRAP_OP_LOOKUP;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (MACH_PORT_NULL);
	if (reply.hdr.msgh_id == BOOTSTRAP_REPLY_NOT_FOUND)
		return (MACH_PORT_NULL);
	if (!(reply.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		return (MACH_PORT_NULL);
	return (reply.pd.name);
}
