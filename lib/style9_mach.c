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
mach_port_mod_refs(mach_port_name_t name, uint8_t right)
{

	return ((int)syscall2(SYS_PORT_MOD_REFS, (long)name, (long)right));
}

mach_port_name_t
mach_port_set_allocate(void)
{
	long	rv;

	rv = syscall0(SYS_PORT_SET_ALLOC);
	if (rv <= 0)
		return (MACH_PORT_NULL);
	return ((mach_port_name_t)rv);
}

int
mach_port_set_insert(mach_port_name_t set_name, mach_port_name_t port_name)
{

	return ((int)syscall2(SYS_PORT_SET_INSERT,
	    (long)set_name, (long)port_name));
}

int
mach_port_set_remove(mach_port_name_t set_name, mach_port_name_t port_name)
{

	return ((int)syscall2(SYS_PORT_SET_REMOVE,
	    (long)set_name, (long)port_name));
}

mach_port_name_t
mach_port_set_extract(mach_port_name_t port_name)
{
	long	rv;

	rv = syscall1(SYS_PORT_SET_EXTRACT, (long)port_name);
	if (rv <= 0)
		return (MACH_PORT_NULL);
	return ((mach_port_name_t)rv);
}

int
mach_port_request_notification(mach_port_name_t name, uint32_t notify_type,
    mach_port_name_t notify_port, uint32_t notify_msgid)
{

	return ((int)syscall4(SYS_PORT_REQUEST_NOTIFICATION,
	    (long)name, (long)notify_type, (long)notify_port,
	    (long)notify_msgid));
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

/*
 * bootstrap_register_service: publish `port` under `service` in the
 * registry.  Wraps the BOOTSTRAP_OP_REGISTER request -- COMPLEX
 * message carrying one port_descriptor (COPY_SEND of `port`) plus the
 * service name in the trailing inline payload -- and decodes the
 * bsr_status word in the status reply.
 */
int
bootstrap_register_service(const char *service, mach_port_name_t port)
{
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
		struct bootstrap_lookup_request		name;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct bootstrap_status_reply		body;
	} reply;
	size_t	i;
	int	rv;

	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		req.name.blr_name[i] = '\0';
	for (i = 0; service[i] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; i++)
		req.name.blr_name[i] = service[i];

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = MACH_PORT_BOOTSTRAP;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = BOOTSTRAP_OP_REGISTER;

	req.body.msgh_descriptor_count = 1;

	req.pd.type        = MACH_MSG_PORT_DESCRIPTOR;
	req.pd.disposition = MACH_MSG_TYPE_COPY_SEND;
	req.pd.pad1        = 0;
	req.pd.pad2        = 0;
	req.pd.name        = port;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	return ((int)reply.body.bsr_status);
}

/*
 * bootstrap_deregister_service: remove `service` from the registry.
 * Plain (non-complex) request with the service name in the inline
 * payload; decode status from the reply.
 */
int
bootstrap_deregister_service(const char *service)
{
	struct {
		struct mach_msg_header			hdr;
		struct bootstrap_lookup_request		name;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct bootstrap_status_reply		body;
	} reply;
	size_t	i;
	int	rv;

	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		req.name.blr_name[i] = '\0';
	for (i = 0; service[i] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; i++)
		req.name.blr_name[i] = service[i];

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = MACH_PORT_BOOTSTRAP;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = BOOTSTRAP_OP_DEREGISTER;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	return ((int)reply.body.bsr_status);
}
