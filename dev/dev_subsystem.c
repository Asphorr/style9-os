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
#include "port_internal.h"

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

/*
 * Stream OPEN reply: hand the caller MOVE_RECEIVE on `stream_kname`.
 *
 * Rationale: a consumer that "opens" the stream wants to recv bytes from
 * it, which requires the RECEIVE right.  COPY_SEND would only let the
 * caller send back into the port -- useless for a stream sink.  The
 * MOVE transfers the unique RECV out of kernel_space (the driver's
 * SEND right stays put, so the driver thread still pushes scancodes in
 * the same direction); the consumer becomes the sole receiver.
 *
 * Single-consumer limitation: only one OPEN_STREAM per stream port
 * succeeds.  After the move the kernel no longer holds RECV, so a
 * second OPEN_STREAM finds no name to MOVE and the send_xlate_desc
 * lookup returns MACH_E_RIGHT.  This is sufficient for the phase-2
 * shell-as-init world; the phase-3 design (per-open ports + fan-out
 * from the driver thread) is captured in MEMORY but not built yet.
 *
 * MOVE_RECEIVE also requires that the port is NOT a current port_set
 * member and has no recv-blocked threads parked on it -- see the
 * send_xlate_desc MOVE_RECEIVE branch in port_msg.c.  kbd_drv and
 * uart_drv satisfy this naturally as long as nothing else in the
 * kernel parks a recv on the same port (the legacy kern/shell.c does,
 * which is why it must not be started when sh.elf is the init).
 *
 * Cross-space mechanics (mirrors bootstrap_dispatch):
 *
 *	`stream_kname` is a kernel_space name, but `from` is the caller's
 *	port_space -- if we asked mach_msg_send to translate the body
 *	descriptor against `from`, the lookup of stream_kname would fail
 *	(no such name in the caller's space) or "succeed" against a
 *	co-numbered unrelated port.  Same fix as bootstrap_dispatch:
 *
 *	  1. resolve the caller's reply port via the caller's space,
 *	  2. install a fresh SEND for it in kernel_space (gives kernel
 *	     a name we can use as msgh_remote),
 *	  3. build the reply with msgh_remote = that kernel name and
 *	     MOVE_SEND disposition -- the send consumes the temp install,
 *	  4. pd.name is stream_kname, already a kernel_space name, so
 *	     send_xlate_desc resolves it correctly with MOVE_RECEIVE.
 */
int
dev_reply_stream(const struct mach_msg_header *req, struct port_space *from,
    mach_port_name_t stream_kname)
{
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply;
	struct port		*reply_port;
	mach_port_name_t	 kernel_reply_name;
	uint8_t			 dummy;
	int			 rv;

	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);
	if (stream_kname == MACH_PORT_NULL || stream_kname == MACH_PORT_DEAD)
		return (MACH_E_INVAL);

	reply_port = space_lookup(from, req->msgh_local,
	    MACH_PORT_RIGHT_SEND, &dummy);
	if (reply_port == NULL)
		return (MACH_E_RIGHT);

	rv = space_install(kernel_space, reply_port, MACH_PORT_RIGHT_SEND,
	    &kernel_reply_name);
	if (rv != MACH_MSG_OK)
		return (rv);

	reply.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	reply.hdr.msgh_size    = sizeof(reply);
	reply.hdr.msgh_remote  = kernel_reply_name;
	reply.hdr.msgh_local   = MACH_PORT_NULL;
	reply.hdr.msgh_voucher = 0;
	reply.hdr.msgh_id      = req->msgh_id;

	reply.body.msgh_descriptor_count = 1;

	reply.pd.name        = stream_kname;
	reply.pd.pad1        = 0;
	reply.pd.disposition = MACH_MSG_TYPE_MOVE_RECEIVE;
	reply.pd.type        = MACH_MSG_PORT_DESCRIPTOR;
	reply.pd.pad2        = 0;

	rv = mach_msg_send(kernel_space, &reply.hdr);
	if (rv != MACH_MSG_OK)
		(void)space_drop_one_right(kernel_space, kernel_reply_name,
		    MACH_PORT_RIGHT_SEND);
	return (rv);
}
