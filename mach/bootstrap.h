/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_BOOTSTRAP_H_
#define	_SYS_BOOTSTRAP_H_

#include <stddef.h>
#include <stdint.h>

#include "port.h"

/*
 * Bootstrap port + service registry.
 *
 * One global Mach port (the bootstrap port) sits behind a SEND right
 * installed in every task's port_space at MACH_PORT_BOOTSTRAP.  It is
 * tagged PORT_SPECIAL_BOOTSTRAP so mach_msg_send routes incoming
 * messages straight to bootstrap_dispatch, which consults a small
 * (name -> kernel port name) registry and replies with a port
 * descriptor carrying COPY_SEND to the matching service.
 *
 * The registry is intentionally tiny: kernel-resident services
 * register themselves with bootstrap_register at boot time, and
 * lookups by client tasks return references the kernel already holds.
 * Ring-3 registration is not exposed -- this is the "find a kernel
 * service" mechanism, not a generic name server.
 *
 * Wire format (request):
 *	mach_msg_header	(msgh_id = BOOTSTRAP_OP_LOOKUP)
 *	struct bootstrap_lookup_request	(payload, 32 bytes name buffer)
 *
 * Wire format (reply, success):
 *	mach_msg_header	(msgh_bits has COMPLEX, msgh_id = req.msgh_id)
 *	mach_msg_body	(descriptor_count = 1)
 *	mach_msg_port_descriptor (SEND right to the named service)
 *
 * Wire format (reply, service not found):
 *	mach_msg_header	(NO complex bit, msgh_id = BOOTSTRAP_REPLY_NOT_FOUND)
 *
 * The client distinguishes success vs failure by the COMPLEX bit on
 * the reply's msgh_bits.
 */

#define	BOOTSTRAP_NAME_MAX		32
#define	BOOTSTRAP_MAX_SERVICES		16

#define	BOOTSTRAP_OP_LOOKUP		1

#define	BOOTSTRAP_REPLY_NOT_FOUND	0xFFFFFFFFu

struct bootstrap_lookup_request {
	char	blr_name[BOOTSTRAP_NAME_MAX];
};

_Static_assert(sizeof(struct bootstrap_lookup_request) == 32,
    "bootstrap_lookup_request must be 32 bytes (wire format)");

/* Bring up the global bootstrap port.  Must run after kernel_space
 * exists and before any task is created (so task_create can install
 * the well-known SEND at name 2). */
void		bootstrap_init(void);

/* Make `name` resolve to the port currently bound at `kernel_name` in
 * kernel_space.  Stores the name; caller is responsible for keeping
 * that kernel name valid for the lifetime of the registration.  Returns
 * MACH_MSG_OK on success, MACH_E_NOSPACE if the registry is full,
 * MACH_E_INVAL on a bad/duplicate name. */
int		bootstrap_register(const char *name,
		    mach_port_name_t kernel_name);

/* Returns the port object backing the bootstrap port -- used by
 * port_install_bootstrap to take a fresh SEND ref into a task's space. */
struct port	*bootstrap_get_port(void);

/* Synchronous dispatcher for incoming bootstrap_port messages.
 * Called from mach_msg_send when dest->p_special == PORT_SPECIAL_BOOTSTRAP. */
int		bootstrap_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);

/*
 * Snapshot the current registry.  Copies up to `max` (name, kernel_name)
 * entries into the caller's `out_names` / `out_knames` arrays and
 * returns how many were filled.  Used by the shell's `mach` command;
 * order is registration order.
 */
size_t		bootstrap_snapshot(char (*out_names)[BOOTSTRAP_NAME_MAX],
		    mach_port_name_t *out_knames, size_t max);

#endif /* !_SYS_BOOTSTRAP_H_ */
