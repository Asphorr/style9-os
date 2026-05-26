/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * Ring-3 helpers for the dev-NAME generic-driver protocol.
 *
 * The kernel-side reference for the wire formats is dev/dev_proto.h;
 * the matching kernel registration code is dev/dev_subsystem.c.  A
 * driver registers its control port under "dev/<short>" in the
 * bootstrap registry, so opening a device from ring 3 is:
 *
 *	1. ctl = bootstrap_lookup("dev/<short>");
 *	2. rpc ctl with msgh_id = DEV_OP_*;
 *	3. parse the typed reply, deallocate ctl, return whatever
 *	   handle the call promises (a stream SEND, an info struct).
 *
 * Both dev_open_stream and dev_info follow that pattern -- the only
 * variance is the reply shape.
 */

/*
 * Compose "dev/<short_name>" into `out`.  The composed string is what
 * gets handed to bootstrap_lookup; lookup itself caps at
 * BOOTSTRAP_NAME_MAX-1 anyway, so we mirror that cap here.  No error
 * on truncation: the lookup will simply miss and return MACH_PORT_NULL.
 */
static void
compose_devname(char *out, const char *short_name)
{
	size_t	i, j;

	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		out[i] = '\0';

	out[0] = 'd';
	out[1] = 'e';
	out[2] = 'v';
	out[3] = '/';
	i = 4;
	for (j = 0; short_name[j] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; j++)
		out[i++] = short_name[j];
}

mach_port_name_t
dev_open_stream(const char *short_name)
{
	struct mach_msg_header				req;
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply;
	char			full[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	ctl;
	mach_port_name_t	stream;
	int			rv;

	if (short_name == NULL || short_name[0] == '\0')
		return (MACH_PORT_NULL);

	compose_devname(full, short_name);

	ctl = bootstrap_lookup(full);
	if (ctl == MACH_PORT_NULL)
		return (MACH_PORT_NULL);

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = ctl;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = DEV_OP_OPEN_STREAM;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(ctl);
	if (rv != MACH_MSG_OK)
		return (MACH_PORT_NULL);
	if (!(reply.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		return (MACH_PORT_NULL);
	if (reply.body.msgh_descriptor_count != 1)
		return (MACH_PORT_NULL);

	stream = reply.pd.name;
	if (stream == MACH_PORT_NULL || stream == MACH_PORT_DEAD)
		return (MACH_PORT_NULL);
	return (stream);
}

int
dev_info(const char *short_name, struct dev_info_reply *out)
{
	struct mach_msg_header			req;
	struct {
		struct mach_msg_header		hdr;
		struct dev_info_reply		body;
	} reply;
	char			full[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	ctl;
	int			rv;

	if (short_name == NULL || out == NULL)
		return (MACH_E_INVAL);

	compose_devname(full, short_name);

	ctl = bootstrap_lookup(full);
	if (ctl == MACH_PORT_NULL)
		return (MACH_E_NAME);

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = ctl;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = DEV_OP_INFO;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(ctl);
	if (rv != MACH_MSG_OK)
		return (rv);

	*out = reply.body;
	return (MACH_MSG_OK);
}

ssize_t
dev_write(const char *short_name, const void *buf, size_t len)
{
	struct {
		struct mach_msg_header		hdr;
		struct dev_write_request	body;
	} req;
	struct {
		struct mach_msg_header		hdr;
		struct dev_write_reply		body;
	} reply;
	char			full[BOOTSTRAP_NAME_MAX];
	const uint8_t		*src;
	mach_port_name_t	ctl;
	size_t			i;
	int			rv;

	if (short_name == NULL || buf == NULL)
		return ((ssize_t)MACH_E_INVAL);
	if (len > DEV_WRITE_MAX)
		len = DEV_WRITE_MAX;

	compose_devname(full, short_name);

	ctl = bootstrap_lookup(full);
	if (ctl == MACH_PORT_NULL)
		return ((ssize_t)MACH_E_NAME);

	src = (const uint8_t *)buf;
	for (i = 0; i < DEV_WRITE_MAX; i++)
		req.body.dwr_data[i] = i < len ? src[i] : 0;
	req.body.dwr_len = (uint32_t)len;
	req.body.dwr_pad = 0;

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = ctl;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = DEV_OP_WRITE;

	rv = mach_msg_rpc(&req.hdr, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(ctl);
	if (rv != MACH_MSG_OK)
		return ((ssize_t)rv);
	if (reply.body.dwr_rv != MACH_MSG_OK)
		return ((ssize_t)reply.body.dwr_rv);
	return ((ssize_t)reply.body.dwr_written);
}
