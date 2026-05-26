/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DEV_SUBSYSTEM_H_
#define	_SYS_DEV_SUBSYSTEM_H_

#include <stddef.h>
#include <stdint.h>

#include "dev_proto.h"
#include "port.h"

/*
 * Driver registration helpers (dev/NAME).
 *
 * Every driver speaks the protocol defined in dev_proto.h on a CONTROL
 * port (PORT_SPECIAL_SERVICE).  dev_register() takes the driver's
 * already-minted control port, installs a SEND right in kernel_space,
 * and binds it under "dev/<short_name>" in the bootstrap registry --
 * one call instead of every driver hand-rolling bootstrap_register +
 * port_install_send_in_kernel + name composition.
 *
 * dev_list_names() walks the bootstrap registry and copies out the
 * short names of every entry whose registered name begins with "dev/".
 * Used by the `dev` shell command.
 */

#define	DEV_PREFIX		"dev/"
#define	DEV_PREFIX_LEN		4

int	dev_register(const char *short_name, struct port *control_port);

size_t	dev_list_names(char (*out)[DEV_NAME_MAX], size_t max);

/*
 * Helper for driver dispatchers: build and send a DEV_OP_INFO reply.
 * `from` is the caller's port_space (msgh_send target), `req` is the
 * incoming header, and the (name, kind, flags) become the reply body.
 * Returns the mach_msg_send rv.  Lets each driver implement INFO in
 * one line.
 */
int	dev_reply_info(const struct mach_msg_header *req,
	    struct port_space *from,
	    const char *name, uint32_t kind, uint32_t flags);

/*
 * Helper for stream drivers: send back a complex reply with a single
 * port_descriptor (COPY_SEND) naming `stream_kname` in kernel_space.
 * Counterpart of DEV_OP_OPEN_STREAM.
 */
int	dev_reply_stream(const struct mach_msg_header *req,
	    struct port_space *from,
	    mach_port_name_t stream_kname);

#endif /* !_SYS_DEV_SUBSYSTEM_H_ */
