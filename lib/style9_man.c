/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * Ring-3 helper for the kernel "man" service.
 *
 * Wire protocol mirrors mach/services.c svc_man_dispatch: send a complex
 * request whose body is the NUL-terminated short page name (no extension);
 * the service replies either with a complex message carrying one OOL
 * descriptor that names the rendered text now mapped into the caller's
 * vm_map, or with a bare header whose msgh_id is MAN_NOT_FOUND when the
 * page is not registered.
 */

int
man_fetch(const char *name, const char **out_text, size_t *out_len)
{
	struct {
		struct mach_msg_header	hdr;
		char			body[MAN_NAME_MAX];
	}				req;
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	}				reply;
	mach_port_name_t		port;
	size_t				i, name_len;
	int				rv;

	if (name == NULL || out_text == NULL || out_len == NULL)
		return (MACH_E_INVAL);

	for (name_len = 0;
	    name_len < MAN_NAME_MAX - 1 && name[name_len] != '\0';
	    name_len++)
		;
	if (name_len == 0)
		return (MACH_E_INVAL);

	port = bootstrap_lookup(SVC_MAN_NAME);
	if (port == MACH_PORT_NULL)
		return (MACH_E_NAME);

	for (i = 0; i < MAN_NAME_MAX; i++)
		req.body[i] = i <= name_len ? name[i] : '\0';

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(struct mach_msg_header) +
	    (uint32_t)(name_len + 1);
	req.hdr.msgh_remote  = port;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = MAN_OP_GET;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(port);
	if (rv != MACH_MSG_OK)
		return (rv);

	if ((reply.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0) {
		if (reply.hdr.msgh_id == MAN_NOT_FOUND)
			return (MACH_E_NAME);
		return (MACH_E_INVAL);
	}
	if (reply.body.msgh_descriptor_count != 1 ||
	    reply.ool.type != MACH_MSG_OOL_DESCRIPTOR)
		return (MACH_E_INVAL);
	if (reply.ool.address == 0 || reply.ool.size == 0)
		return (MACH_E_INVAL);

	*out_text = (const char *)(uintptr_t)reply.ool.address;
	*out_len  = (size_t)reply.ool.size;
	return (MACH_MSG_OK);
}

int
man_release(const char *text, size_t len)
{
	size_t	aligned;

	if (text == NULL || len == 0)
		return (SYS_E_INVAL);

	aligned = (len + 0xFFFu) & ~(size_t)0xFFFu;
	return (vm_deallocate((void *)(uintptr_t)text, aligned));
}
