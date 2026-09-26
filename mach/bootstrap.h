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
 * Bootstrap port and service registry.
 *
 * Every task holds a SEND right to the one global bootstrap port at
 * MACH_PORT_BOOTSTRAP.  It is PORT_SPECIAL_BOOTSTRAP, so mach_msg_send
 * hands its messages to bootstrap_dispatch, which keeps a small
 * name -> kernel_space name registry.
 *
 * Kernel services register at boot with bootstrap_register (services.c,
 * host.c, the drivers); ring-3 services with BOOTSTRAP_OP_REGISTER.  A
 * lookup from any task returns a fresh SEND right to the port behind the
 * name.  No authentication: the first to claim a name owns it.
 *
 * Lookup request:
 *	mach_msg_header	(msgh_id = BOOTSTRAP_OP_LOOKUP)
 *	struct bootstrap_lookup_request	(32-byte name)
 *
 * Lookup reply, found:
 *	mach_msg_header	(COMPLEX, msgh_id = req.msgh_id)
 *	mach_msg_body	(descriptor_count = 1)
 *	mach_msg_port_descriptor (SEND right to the service)
 *
 * Lookup reply, not found:
 *	mach_msg_header	(not complex, msgh_id = BOOTSTRAP_REPLY_NOT_FOUND)
 *
 * Register request:
 *	mach_msg_header	(COMPLEX, msgh_id = BOOTSTRAP_OP_REGISTER)
 *	mach_msg_body	(descriptor_count = 1)
 *	mach_msg_port_descriptor (COPY_SEND of the port to publish)
 *	struct bootstrap_lookup_request	(name to bind)
 *
 * Deregister request:
 *	mach_msg_header	(msgh_id = BOOTSTRAP_OP_DEREGISTER)
 *	struct bootstrap_lookup_request	(name to remove)
 *
 * Register / deregister reply:
 *	mach_msg_header	(not complex, msgh_id = req.msgh_id)
 *	struct bootstrap_status_reply (MACH_MSG_OK or MACH_E_*)
 *
 * A lookup succeeded if the reply is COMPLEX; register and deregister
 * report in bsr_status.  Wire structs are ABI-stable, as in port.h.
 */

#define	BOOTSTRAP_NAME_MAX		32
#define	BOOTSTRAP_MAX_SERVICES		16

#define	BOOTSTRAP_OP_LOOKUP		1
#define	BOOTSTRAP_OP_REGISTER		2
#define	BOOTSTRAP_OP_DEREGISTER		3

#define	BOOTSTRAP_REPLY_NOT_FOUND	0xFFFFFFFFu

/* WIRE FORMAT.  ABI-stable. */
struct bootstrap_lookup_request {
	char	blr_name[BOOTSTRAP_NAME_MAX];
};

_Static_assert(sizeof(struct bootstrap_lookup_request) == 32,
    "bootstrap_lookup_request must be 32 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  Reply body for register / deregister. */
struct bootstrap_status_reply {
	int32_t		bsr_status;	/* MACH_MSG_OK or MACH_E_*    */
	uint32_t	bsr_pad;
};

_Static_assert(sizeof(struct bootstrap_status_reply) == 8,
    "bootstrap_status_reply must be 8 bytes (wire format)");

/*
 * Create the bootstrap port: after kernel_space exists, before any task
 * (task_create installs its SEND at name 2).
 */
void		bootstrap_init(void);

/*
 * Give the bootstrap port a permanent kernel_space SEND name for
 * task_get_special_port.  Must run after task_subsystem_init, so as not
 * to take kernel_task's names 1 and 2.  Idempotent.
 */
void		bootstrap_publish(void);

/*
 * Make `name` resolve to the port at `kernel_name` in kernel_space.
 * Only the name is stored; the caller keeps it valid while registered.
 * MACH_E_NOSPACE if the registry is full, MACH_E_INVAL for a bad or
 * duplicate name.
 */
int		bootstrap_register(const char *name,
		    mach_port_name_t kernel_name);

/*
 * Remove the entry for `name`, returning its kernel_space name in
 * *kn_out for the caller to drop.  MACH_E_INVAL if not registered.
 */
int		bootstrap_unregister(const char *name,
		    mach_port_name_t *kn_out);

/* The bootstrap port object, for port_install_bootstrap. */
struct port	*bootstrap_get_port(void);

/*
 * The bootstrap port's kernel_space name, which task_get_special_port
 * COPY_SENDs; MACH_PORT_NULL before bootstrap_publish.
 */
mach_port_name_t bootstrap_get_kernel_name(void);

/* mach_msg_send's dispatcher for PORT_SPECIAL_BOOTSTRAP. */
int		bootstrap_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);

/*
 * Copy up to `max` (name, kernel name) pairs, in registration order,
 * and return how many.  Used by the shell's `mach` command and
 * dev_list_names.
 */
size_t		bootstrap_snapshot(char (*out_names)[BOOTSTRAP_NAME_MAX],
		    mach_port_name_t *out_knames, size_t max);

#endif /* !_SYS_BOOTSTRAP_H_ */
