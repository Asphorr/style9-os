/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bootstrap.h"
#include "dev_subsystem.h"
#include "kprintf.h"
#include "port.h"

extern struct port_space	*kernel_space;
extern int			 port_install_send_in_kernel(struct port *,
				    mach_port_name_t *name_out);

int
dev_register(const char *short_name, struct port *control_port)
{
	char			full[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	kn;
	size_t			i, j;
	int			rv;

	if (short_name == NULL || control_port == NULL)
		return (MACH_E_INVAL);

	rv = port_install_send_in_kernel(control_port, &kn);
	if (rv != MACH_MSG_OK)
		return (rv);

	/*
	 * Compose "dev/<short_name>" in `full`.  Cap at BOOTSTRAP_NAME_MAX-1
	 * so we always have room for the terminating NUL; long names get
	 * silently truncated rather than rejected so a future verbose name
	 * doesn't take a driver out of the registry.
	 */
	for (i = 0; i < DEV_PREFIX_LEN; i++)
		full[i] = DEV_PREFIX[i];
	for (j = 0; short_name[j] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; j++)
		full[i++] = short_name[j];
	while (i < BOOTSTRAP_NAME_MAX)
		full[i++] = '\0';

	rv = bootstrap_register(full, kn);
	if (rv != MACH_MSG_OK)
		return (rv);

	kprintf("dev: registered %s -> kernel name %u\n",
	    full, (unsigned)kn);
	return (MACH_MSG_OK);
}

size_t
dev_list_names(char (*out)[DEV_NAME_MAX], size_t max)
{
	char			names[BOOTSTRAP_MAX_SERVICES][BOOTSTRAP_NAME_MAX];
	mach_port_name_t	knames[BOOTSTRAP_MAX_SERVICES];
	size_t			n, i, j, w;

	if (out == NULL || max == 0)
		return (0);

	n = bootstrap_snapshot(names, knames, BOOTSTRAP_MAX_SERVICES);
	w = 0;
	for (i = 0; i < n && w < max; i++) {
		/* Match the "dev/" prefix exactly. */
		if (names[i][0] != 'd' || names[i][1] != 'e' ||
		    names[i][2] != 'v' || names[i][3] != '/')
			continue;
		for (j = 0; j < DEV_NAME_MAX - 1 &&
		    names[i][DEV_PREFIX_LEN + j] != '\0'; j++)
			out[w][j] = names[i][DEV_PREFIX_LEN + j];
		out[w][j] = '\0';
		w++;
	}
	return (w);
}

int
dev_reply_info(const struct mach_msg_header *req, struct port_space *from,
    const char *name, uint32_t kind, uint32_t flags)
{
	uint8_t			 buf[sizeof(struct mach_msg_header) +
				     sizeof(struct dev_info_reply)];
	struct mach_msg_header	*rhdr;
	struct dev_info_reply	*body;
	size_t			 i;

	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	rhdr = (struct mach_msg_header *)buf;
	body = (struct dev_info_reply *)(buf + sizeof(struct mach_msg_header));

	for (i = 0; i < DEV_NAME_MAX; i++)
		body->dir_name[i] = 0;
	for (i = 0; i < DEV_NAME_MAX - 1 && name != NULL && name[i] != '\0'; i++)
		body->dir_name[i] = name[i];
	body->dir_kind  = kind;
	body->dir_flags = flags;

	rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	rhdr->msgh_size    = sizeof(buf);
	rhdr->msgh_remote  = req->msgh_local;
	rhdr->msgh_local   = MACH_PORT_NULL;
	rhdr->msgh_voucher = 0;
	rhdr->msgh_id      = req->msgh_id;
	return (mach_msg_send(from, rhdr));
}

int
dev_reply_stream(const struct mach_msg_header *req, struct port_space *from,
    mach_port_name_t stream_kname)
{
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply;

	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);
	if (stream_kname == MACH_PORT_NULL || stream_kname == MACH_PORT_DEAD)
		return (MACH_E_INVAL);

	reply.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	reply.hdr.msgh_size    = sizeof(reply);
	reply.hdr.msgh_remote  = req->msgh_local;
	reply.hdr.msgh_local   = MACH_PORT_NULL;
	reply.hdr.msgh_voucher = 0;
	reply.hdr.msgh_id      = req->msgh_id;

	reply.body.msgh_descriptor_count = 1;

	reply.pd.name        = stream_kname;
	reply.pd.pad1        = 0;
	reply.pd.disposition = MACH_MSG_TYPE_COPY_SEND;
	reply.pd.type        = MACH_MSG_PORT_DESCRIPTOR;
	reply.pd.pad2        = 0;

	return (mach_msg_send(from, &reply.hdr));
}
