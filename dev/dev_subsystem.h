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
 * Every driver speaks the protocol in dev_proto.h on a control port
 * (PORT_SPECIAL_SERVICE).  dev_register() installs a SEND right for the
 * driver's control port in kernel_space and binds it under
 * "dev/<short_name>" in the bootstrap registry.
 *
 * dev_list_names() copies out the short names of every bootstrap entry
 * under "dev/".  Used by the `dev' shell command.
 */

#define	DEV_PREFIX		"dev/"
#define	DEV_PREFIX_LEN		4

int	dev_register(const char *short_name, struct port *control_port);

size_t	dev_list_names(char (*out)[DEV_NAME_MAX], size_t max);

/*
 * Build and send a DEV_OP_INFO reply to `req' in the caller's space
 * `from', with (name, kind, flags) as the body.  Returns the
 * mach_msg_send result.
 */
int	dev_reply_info(const struct mach_msg_header *req,
	    struct port_space *from,
	    const char *name, uint32_t kind, uint32_t flags);

/*
 * DEV_OP_OPEN_STREAM reply: one port descriptor that moves the RECEIVE
 * right for `stream_kname' (a kernel_space name) to the caller, so only
 * the first open of a stream succeeds.
 */
int	dev_reply_stream(const struct mach_msg_header *req,
	    struct port_space *from,
	    mach_port_name_t stream_kname);

#endif /* !_SYS_DEV_SUBSYSTEM_H_ */
