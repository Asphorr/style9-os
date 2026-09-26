/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * Mach IPC wrappers: one per syscall, plus RPC helpers for the bootstrap,
 * host and task ports.
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
 * bootstrap_lookup: return the SEND right the bootstrap port hands back
 * for `service', or MACH_PORT_NULL on any error (RPC failure, not found,
 * malformed reply).  The caller deallocates it.
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
 * bootstrap_register_service: BOOTSTRAP_OP_REGISTER is a COMPLEX message
 * carrying one port descriptor (COPY_SEND of `port') followed by the
 * service name; the reply carries bsr_status.
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

/* bootstrap_deregister_service: a plain request carrying the name. */
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

/*
 * mach_host_self: native tasks reach the host port through the bootstrap
 * registry like any other service; Darwin binaries use host_self_trap.
 */
mach_port_name_t
mach_host_self(void)
{

	return (bootstrap_lookup(SVC_HOST_NAME));
}

int
host_page_size(mach_port_name_t host, uint32_t *page_size_out)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header		hdr;
		struct svc_host_page_size_reply	body;
	} reply;
	int	rv;

	if (page_size_out == NULL)
		return (MACH_E_INVAL);

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = host;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = HOST_OP_PAGE_SIZE;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	*page_size_out = reply.body.hps_page_size;
	return (MACH_MSG_OK);
}

int
host_info(mach_port_name_t host, struct svc_host_info_reply *out)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header		hdr;
		struct svc_host_info_reply	body;
	} reply;
	int	rv;

	if (out == NULL)
		return (MACH_E_INVAL);

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = host;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = HOST_OP_INFO;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	*out = reply.body;
	return (MACH_MSG_OK);
}

/*
 * task_vm_allocate: returns MACH_MSG_OK, the reply's in-band status if the
 * allocation failed, or the transport's MACH_E_* code.
 */
int
task_vm_allocate(mach_port_name_t task, uint64_t size, uint32_t prot,
    uint64_t *addr_out)
{
	struct {
		struct mach_msg_header			hdr;
		struct task_vm_allocate_request		body;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct task_vm_allocate_reply		body;
	} reply;
	int	rv;

	if (addr_out == NULL)
		return (MACH_E_INVAL);

	req.body.tva_size = size;
	req.body.tva_prot = prot;
	req.body.tva_pad  = 0;

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = task;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = TASK_OP_VM_ALLOCATE;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	if (reply.body.tvar_status != MACH_MSG_OK)
		return ((int)reply.body.tvar_status);
	*addr_out = reply.body.tvar_address;
	return (MACH_MSG_OK);
}

int
task_vm_deallocate(mach_port_name_t task, uint64_t addr, uint64_t size)
{
	struct {
		struct mach_msg_header			hdr;
		struct task_vm_deallocate_request	body;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct task_vm_deallocate_reply		body;
	} reply;
	int	rv;

	req.body.tvd_address = addr;
	req.body.tvd_size    = size;

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = task;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = TASK_OP_VM_DEALLOCATE;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	return ((int)reply.body.tvdr_status);
}

/*
 * task_get_special_port: success is a COMPLEX reply, the port riding in a
 * descriptor as for a bootstrap lookup; anything else is MACH_E_INVAL.
 * The caller deallocates the returned name.
 */
int
task_get_special_port(mach_port_name_t task, uint32_t which,
    mach_port_name_t *port_out)
{
	struct {
		struct mach_msg_header			hdr;
		struct task_special_port_request	body;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply;
	int	rv;

	if (port_out == NULL)
		return (MACH_E_INVAL);

	req.body.tsp_which = which;
	req.body.tsp_pad   = 0;

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = task;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = TASK_OP_GET_SPECIAL_PORT;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (rv);
	if (!(reply.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		return (MACH_E_INVAL);
	*port_out = reply.pd.name;
	return (MACH_MSG_OK);
}
