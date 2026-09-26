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
#include "port_internal.h"
#include "spinlock.h"
#include "task.h"

/* From port_object.c; the first creates the bootstrap port. */
extern struct port	*port_create_kernel_owned(uint8_t special_kind,
			    void *special_arg);

extern int		 port_install_send_in_kernel(struct port *p,
			    mach_port_name_t *name_out);

extern struct port_space	*kernel_space;

/*
 * A registered service: its name and the port's name in kernel_space
 * (not a pointer), so a lookup reply can use mach_msg_send's ordinary
 * descriptor translation.
 */
struct bootstrap_service {
	char			bs_name[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	bs_kname;
};

static struct spinlock		registry_lock = SPINLOCK_INIT("bootstrap");
static struct bootstrap_service	registry[BOOTSTRAP_MAX_SERVICES];	/* (registry_lock) */
static size_t			registry_count;				/* (registry_lock) */
static struct port		*the_bootstrap_port;			/* (c) */

static mach_port_name_t		the_bootstrap_kn;			/* (c) */

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

/*
 * Give the bootstrap port a permanent SEND name in kernel_space, for
 * task_get_special_port to COPY_SEND.  Must run after
 * task_subsystem_init: kernel_task's TASK_SELF (1) and BOOTSTRAP (2) must
 * be claimed first, or the task-self install lands off name 1.
 * Idempotent.
 */
void
bootstrap_publish(void)
{

	if (the_bootstrap_kn != MACH_PORT_NULL)
		return;
	if (port_install_send_in_kernel(the_bootstrap_port,
	    &the_bootstrap_kn) != MACH_MSG_OK)
		panic("bootstrap_publish: install bootstrap in kernel_space");
}

struct port *
bootstrap_get_port(void)
{

	return (the_bootstrap_port);
}

/*
 * The bootstrap port's kernel_space name, or MACH_PORT_NULL before
 * bootstrap_publish has run.
 */
mach_port_name_t
bootstrap_get_kernel_name(void)
{

	return (the_bootstrap_kn);
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

size_t
bootstrap_snapshot(char (*out_names)[BOOTSTRAP_NAME_MAX],
    mach_port_name_t *out_knames, size_t max)
{
	size_t	i, n;

	if (out_names == NULL || out_knames == NULL || max == 0)
		return (0);

	spin_lock(&registry_lock);
	n = registry_count < max ? registry_count : max;
	for (i = 0; i < n; i++) {
		size_t j;
		for (j = 0; j < BOOTSTRAP_NAME_MAX; j++)
			out_names[i][j] = registry[i].bs_name[j];
		out_knames[i] = registry[i].bs_kname;
	}
	spin_unlock(&registry_lock);
	return (n);
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

int
bootstrap_unregister(const char *name, mach_port_name_t *kn_out)
{
	size_t	i, j;

	if (name == NULL || name[0] == '\0' || kn_out == NULL)
		return (MACH_E_INVAL);

	spin_lock(&registry_lock);
	for (i = 0; i < registry_count; i++) {
		if (!bootstrap_name_equal(registry[i].bs_name, name))
			continue;
		*kn_out = registry[i].bs_kname;
		/*
		 * Shift down rather than move the last entry into the hole,
		 * so bootstrap_snapshot keeps reporting registration order.
		 */
		for (j = i; j + 1 < registry_count; j++)
			registry[j] = registry[j + 1];
		registry_count--;
		spin_unlock(&registry_lock);
		return (MACH_MSG_OK);
	}
	spin_unlock(&registry_lock);
	return (MACH_E_INVAL);
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
 * Reply to req->msgh_local with a bare bootstrap_status_reply, for
 * REGISTER and DEREGISTER.
 */
static int
bootstrap_send_status(const struct mach_msg_header *req,
    struct port_space *from, int status)
{
	struct {
		struct mach_msg_header		hdr;
		struct bootstrap_status_reply	body;
	} reply;

	reply.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	reply.hdr.msgh_size    = sizeof(reply);
	reply.hdr.msgh_remote  = req->msgh_local;
	reply.hdr.msgh_local   = MACH_PORT_NULL;
	reply.hdr.msgh_voucher = 0;
	reply.hdr.msgh_id      = req->msgh_id;
	reply.body.bsr_status  = status;
	reply.body.bsr_pad     = 0;
	return (mach_msg_send(from, &reply.hdr));
}

/*
 * BOOTSTRAP_OP_LOOKUP.  A hit replies COMPLEX with one port descriptor
 * carrying COPY_SEND to the service; a miss replies with a bare header,
 * msgh_id BOOTSTRAP_REPLY_NOT_FOUND.
 *
 * The service is named in kernel_space, not in the caller's space, so
 * the hit is sent from kernel_space: the caller's reply port gets a
 * temporary SEND name there, used as msgh_remote with MOVE_SEND so the
 * send consumes it, and pd.name is the service's kernel name.  Delivery
 * then installs a fresh name for the service in the caller's space.
 */
static int
bootstrap_dispatch_lookup(const struct mach_msg_header *req,
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
	struct port		*reply_port;
	char			 name_buf[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	 kernel_reply_name;
	mach_port_name_t	 svc_name;
	uint8_t			 dummy;
	size_t			 i;
	int			 rv;

	/*
	 * The name follows the header.  A larger msgh_size is fine: the
	 * caller's buffer may be sized for the complex reply.
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

	/* The caller's reply port, given a SEND name in kernel_space. */
	reply_port = space_lookup(from, req->msgh_local,
	    MACH_PORT_RIGHT_SEND, &dummy);
	if (reply_port == NULL)
		return (MACH_E_RIGHT);

	rv = space_install(kernel_space, reply_port, MACH_PORT_RIGHT_SEND,
	    &kernel_reply_name);
	if (rv != MACH_MSG_OK)
		return (rv);

	reply_ok.hdr.msgh_bits    =
	    MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	reply_ok.hdr.msgh_size    = sizeof(reply_ok);
	reply_ok.hdr.msgh_remote  = kernel_reply_name;
	reply_ok.hdr.msgh_local   = MACH_PORT_NULL;
	reply_ok.hdr.msgh_voucher = 0;
	reply_ok.hdr.msgh_id      = req->msgh_id;

	reply_ok.body.msgh_descriptor_count = 1;

	reply_ok.pd.name        = svc_name;
	reply_ok.pd.pad1        = 0;
	reply_ok.pd.disposition = MACH_MSG_TYPE_COPY_SEND;
	reply_ok.pd.type        = MACH_MSG_PORT_DESCRIPTOR;
	reply_ok.pd.pad2        = 0;

	rv = mach_msg_send(kernel_space, &reply_ok.hdr);

	/*
	 * A failed send did not consume the temporary name (MOVE_SEND
	 * happens only on success): drop it, and its ref.
	 */
	if (rv != MACH_MSG_OK)
		(void)space_drop_one_right(kernel_space, kernel_reply_name,
		    MACH_PORT_RIGHT_SEND);
	return (rv);
}

/*
 * BOOTSTRAP_OP_REGISTER, from ring 3.  Request:
 *
 *	[ mach_msg_header | mach_msg_body | port_descriptor | name(32) ]
 *
 * The descriptor names, with COPY_SEND, the caller's port to publish.
 * The kernel gives it its own SEND name in kernel_space and registers
 * that; the caller keeps its right.
 *
 * No authentication: any task can claim any unused name.  The entry
 * lasts until deregistered; nothing removes it when the port dies.
 */
static int
bootstrap_dispatch_register(const struct mach_msg_header *req,
    struct port_space *from)
{
	const struct bootstrap_lookup_request	*nq;
	const struct mach_msg_port_descriptor	*pd;
	const struct mach_msg_body		*body;
	const uint8_t				*buf;
	struct port		*p;
	char			 name_buf[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	 kname;
	uint8_t			 dummy;
	size_t			 i;
	int			 rv;
	int			 status;

	if ((req->msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0)
		return (bootstrap_send_status(req, from, MACH_E_INVAL));
	if (req->msgh_size < sizeof(struct mach_msg_header) +
	    sizeof(struct mach_msg_body) +
	    sizeof(struct mach_msg_port_descriptor) +
	    sizeof(struct bootstrap_lookup_request))
		return (bootstrap_send_status(req, from, MACH_E_INVAL));

	buf  = (const uint8_t *)req;
	body = (const struct mach_msg_body *)(buf +
	    sizeof(struct mach_msg_header));
	if (body->msgh_descriptor_count != 1)
		return (bootstrap_send_status(req, from, MACH_E_INVAL));

	pd = (const struct mach_msg_port_descriptor *)(buf +
	    sizeof(struct mach_msg_header) +
	    sizeof(struct mach_msg_body));
	if (pd->type != MACH_MSG_PORT_DESCRIPTOR)
		return (bootstrap_send_status(req, from, MACH_E_INVAL));
	if (pd->disposition != MACH_MSG_TYPE_COPY_SEND)
		return (bootstrap_send_status(req, from, MACH_E_INVAL));

	nq = (const struct bootstrap_lookup_request *)(buf +
	    sizeof(struct mach_msg_header) +
	    sizeof(struct mach_msg_body) +
	    sizeof(struct mach_msg_port_descriptor));
	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		name_buf[i] = nq->blr_name[i];
	name_buf[BOOTSTRAP_NAME_MAX - 1] = '\0';

	/*
	 * pd.name is in the sender's space.  space_lookup takes no ref --
	 * the sending thread is inside this very send -- and the
	 * kernel_space install takes the SEND ref the registration holds.
	 */
	p = space_lookup(from, pd->name, MACH_PORT_RIGHT_SEND, &dummy);
	if (p == NULL)
		return (bootstrap_send_status(req, from, MACH_E_RIGHT));

	rv = space_install(kernel_space, p, MACH_PORT_RIGHT_SEND, &kname);
	if (rv != MACH_MSG_OK)
		return (bootstrap_send_status(req, from, rv));

	status = bootstrap_register(name_buf, kname);
	if (status != MACH_MSG_OK)
		(void)space_drop_one_right(kernel_space, kname,
		    MACH_PORT_RIGHT_SEND);
	return (bootstrap_send_status(req, from, status));
}

/*
 * BOOTSTRAP_OP_DEREGISTER, from ring 3.  Request:
 *
 *	[ mach_msg_header | name(32) ]
 *
 * Removes the entry and drops the kernel's SEND ref behind it.  Entries
 * carry no owner, so a kernel service can be deregistered too, and is
 * then orphaned.
 */
static int
bootstrap_dispatch_deregister(const struct mach_msg_header *req,
    struct port_space *from)
{
	const struct bootstrap_lookup_request	*nq;
	const uint8_t				*buf;
	char			 name_buf[BOOTSTRAP_NAME_MAX];
	mach_port_name_t	 kname;
	size_t			 i;
	int			 status;

	if (req->msgh_size < sizeof(struct mach_msg_header) +
	    sizeof(struct bootstrap_lookup_request))
		return (bootstrap_send_status(req, from, MACH_E_INVAL));

	buf = (const uint8_t *)req;
	nq  = (const struct bootstrap_lookup_request *)(buf +
	    sizeof(struct mach_msg_header));
	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		name_buf[i] = nq->blr_name[i];
	name_buf[BOOTSTRAP_NAME_MAX - 1] = '\0';

	status = bootstrap_unregister(name_buf, &kname);
	if (status == MACH_MSG_OK)
		(void)space_drop_one_right(kernel_space, kname,
		    MACH_PORT_RIGHT_SEND);
	return (bootstrap_send_status(req, from, status));
}

int
bootstrap_dispatch(const struct mach_msg_header *req, struct port_space *from)
{

	if (req == NULL || from == NULL)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	switch (req->msgh_id) {
	case BOOTSTRAP_OP_LOOKUP:
		return (bootstrap_dispatch_lookup(req, from));
	case BOOTSTRAP_OP_REGISTER:
		return (bootstrap_dispatch_register(req, from));
	case BOOTSTRAP_OP_DEREGISTER:
		return (bootstrap_dispatch_deregister(req, from));
	default:
		return (MACH_E_INVAL);
	}
}
