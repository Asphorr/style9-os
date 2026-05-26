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
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "spinlock.h"
#include "task.h"

/*
 * Hooks into port.c.  port_create_kernel_owned mints a port object the
 * kernel itself owns (RECEIVE held, no name in any port_space) with a
 * p_special tag pre-set; we use it to mint the single bootstrap port.
 */
extern struct port	*port_create_kernel_owned(uint8_t special_kind,
			    void *special_arg);

extern struct port_space	*kernel_space;

/*
 * Service-registry entry.  Stores the name + the kernel-side name of
 * the port (NOT a port object pointer) so the dispatcher can reuse the
 * existing mach_msg_send descriptor-translation path on the reply.
 */
struct bootstrap_service {
	char			bs_name[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	bs_kname;
};

static struct spinlock		registry_lock = SPINLOCK_INIT("bootstrap");
static struct bootstrap_service	registry[BOOTSTRAP_MAX_SERVICES];	/* (registry_lock) */
static size_t			registry_count;				/* (registry_lock) */
static struct port		*the_bootstrap_port;			/* (c) */

void
bootstrap_init(void)
{

	if (the_bootstrap_port != NULL)
		return;

	the_bootstrap_port = port_create_kernel_owned(
	    PORT_SPECIAL_BOOTSTRAP, NULL);
	if (the_bootstrap_port == NULL)
		panic("bootstrap_init: port creation failed");

	registry_count = 0;
	kprintf("bootstrap: ready, capacity=%u services\n",
	    (unsigned)BOOTSTRAP_MAX_SERVICES);
}

struct port *
bootstrap_get_port(void)
{

	return (the_bootstrap_port);
}

static bool
bootstrap_name_equal(const char *a, const char *b)
{
	size_t	i;

	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++) {
		if (a[i] != b[i])
			return (false);
		if (a[i] == '\0')
			return (true);
	}
	return (true);	/* both filled the whole buffer with equal bytes */
}

static void
bootstrap_name_copy(char *dst, const char *src)
{
	size_t	i;

	for (i = 0; i < BOOTSTRAP_NAME_MAX - 1; i++) {
		dst[i] = src[i];
		if (src[i] == '\0') {
			i++;
			break;
		}
	}
	for (; i < BOOTSTRAP_NAME_MAX; i++)
		dst[i] = '\0';
}

int
bootstrap_register(const char *name, mach_port_name_t kernel_name)
{
	size_t	i;

	if (name == NULL || name[0] == '\0')
		return (MACH_E_INVAL);
	if (kernel_name == MACH_PORT_NULL || kernel_name == MACH_PORT_DEAD)
		return (MACH_E_INVAL);

	spin_lock(&registry_lock);
	for (i = 0; i < registry_count; i++) {
		if (bootstrap_name_equal(registry[i].bs_name, name)) {
			spin_unlock(&registry_lock);
			return (MACH_E_INVAL);	/* duplicate */
		}
	}
	if (registry_count >= BOOTSTRAP_MAX_SERVICES) {
		spin_unlock(&registry_lock);
		return (MACH_E_NOSPACE);
	}
	bootstrap_name_copy(registry[registry_count].bs_name, name);
	registry[registry_count].bs_kname = kernel_name;
	registry_count++;
	spin_unlock(&registry_lock);
	return (MACH_MSG_OK);
}

static mach_port_name_t
bootstrap_lookup_locked(const char *name)
{
	size_t	i;

	for (i = 0; i < registry_count; i++) {
		if (bootstrap_name_equal(registry[i].bs_name, name))
			return (registry[i].bs_kname);
	}
	return (MACH_PORT_NULL);
}

/*
 * Synchronous handler.  Validates the request shape, looks the named
 * service up in the registry, and posts the reply back to the caller
 * via msgh_local.  On a hit the reply is COMPLEX with a single
 * port_descriptor carrying COPY_SEND to the service; on a miss the
 * reply is a bare 24-byte header tagged with BOOTSTRAP_REPLY_NOT_FOUND.
 */
int
bootstrap_dispatch(const struct mach_msg_header *req,
    struct port_space *from)
{
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply_ok;
	struct mach_msg_header				reply_fail;
	const struct bootstrap_lookup_request		*rq;
	const uint8_t					*src;
	char			 name_buf[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	 svc_name;
	size_t			 i;

	if (req == NULL || from == NULL)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);
	if (req->msgh_id != BOOTSTRAP_OP_LOOKUP)
		return (MACH_E_INVAL);

	/*
	 * Payload starts right after the 24-byte header.  We accept any
	 * msgh_size that is at least header + lookup_request; a larger
	 * size just means the caller used a bigger buffer (e.g. one big
	 * enough to receive a complex reply with the port_descriptor).
	 */
	if (req->msgh_size < sizeof(struct mach_msg_header) +
	    sizeof(struct bootstrap_lookup_request))
		return (MACH_E_INVAL);

	src = (const uint8_t *)req + sizeof(struct mach_msg_header);
	rq  = (const struct bootstrap_lookup_request *)src;
	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		name_buf[i] = rq->blr_name[i];
	name_buf[BOOTSTRAP_NAME_MAX - 1] = '\0';	/* hard cap */

	spin_lock(&registry_lock);
	svc_name = bootstrap_lookup_locked(name_buf);
	spin_unlock(&registry_lock);

	if (svc_name == MACH_PORT_NULL) {
		reply_fail.msgh_bits    =
		    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		reply_fail.msgh_size    = sizeof(reply_fail);
		reply_fail.msgh_remote  = req->msgh_local;
		reply_fail.msgh_local   = MACH_PORT_NULL;
		reply_fail.msgh_voucher = 0;
		reply_fail.msgh_id      = BOOTSTRAP_REPLY_NOT_FOUND;
		return (mach_msg_send(from, &reply_fail));
	}

	reply_ok.hdr.msgh_bits    =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	reply_ok.hdr.msgh_size    = sizeof(reply_ok);
	reply_ok.hdr.msgh_remote  = req->msgh_local;
	reply_ok.hdr.msgh_local   = MACH_PORT_NULL;
	reply_ok.hdr.msgh_voucher = 0;
	reply_ok.hdr.msgh_id      = req->msgh_id;

	reply_ok.body.msgh_descriptor_count = 1;

	reply_ok.pd.name        = svc_name;
	reply_ok.pd.pad1        = 0;
	reply_ok.pd.disposition = MACH_MSG_TYPE_COPY_SEND;
	reply_ok.pd.type        = MACH_MSG_PORT_DESCRIPTOR;
	reply_ok.pd.pad2        = 0;

	return (mach_msg_send(from, &reply_ok.hdr));
}
