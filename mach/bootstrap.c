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
		 * Compact the table in place; registry is a flat array
		 * walked linearly by bootstrap_lookup_locked, so order is
		 * irrelevant -- swap-last-into-hole would do too, but
		 * shift-down keeps the BSD-bar registration-order
		 * semantics that bootstrap_snapshot reports.
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
 * Send a bare status reply (no descriptors) to req->msgh_local with the
 * given status word in the body.  Used by REGISTER and DEREGISTER
 * dispatches; LOOKUP has its own shapes (COMPLEX with a port_descriptor
 * on hit, plain BOOTSTRAP_REPLY_NOT_FOUND on miss).
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
 * Lookup dispatcher.  Resolves the named service in the registry and
 * posts a reply back via msgh_local.  On a hit the reply is COMPLEX
 * with a single port_descriptor carrying COPY_SEND to the service; on
 * a miss the reply is a bare 24-byte header tagged with
 * BOOTSTRAP_REPLY_NOT_FOUND.
 *
 * Cross-space mechanics for the success path:
 *
 *	The registered service port is named in kernel_space, but the
 *	caller (the original sender of the lookup) lives in some other
 *	port_space.  If we asked mach_msg_send to translate the reply's
 *	port_descriptor against the caller's space, the lookup of
 *	svc_name would fail (or worse, "succeed" against a coincidentally
 *	co-numbered name).  Instead we send the reply FROM kernel_space:
 *
 *	  1. resolve the caller's reply port via the caller's space,
 *	  2. install a fresh SEND for it in kernel_space (so kernel_space
 *	     has a name we can use as msgh_remote),
 *	  3. build the reply with msgh_remote = that kernel name and
 *	     MOVE_SEND disposition, so the send consumes our temporary
 *	     install -- no kernel_space leak even after delivery,
 *	  4. pd.name is svc_name, which already lives in kernel_space,
 *	     and send_xlate_desc resolves it correctly.
 *
 *	The receive side is unchanged: deliver_msg sees a port_descriptor
 *	carrying SEND, installs a fresh name in the caller's space, and
 *	patches pd.name accordingly.
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

	/*
	 * Cross-space reply: install caller's reply port into kernel_space
	 * so we can address it from there alongside svc_name.  Look it up
	 * in the caller's space (where the lookup request named it via
	 * msgh_local), then install a fresh SEND in kernel_space.
	 */
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
	 * On send failure roll back the kernel_space install we did
	 * above; the MOVE_SEND clause inside mach_msg_send only fires on
	 * the success path.  space_drop_one_right port_derefs the SEND
	 * for us so refs balance with the space_install above.
	 */
	if (rv != MACH_MSG_OK)
		(void)space_drop_one_right(kernel_space, kernel_reply_name,
		    MACH_PORT_RIGHT_SEND);
	return (rv);
}

/*
 * Ring-3 service publish.  Request layout is the complex shape:
 *
 *	[ mach_msg_header | mach_msg_body | port_descriptor | name(32) ]
 *
 * The port_descriptor carries COPY_SEND of the port the caller wants
 * to publish (resolved against the caller's space), and the trailing
 * 32-byte buffer holds the registration name.
 *
 * The dispatcher takes a kernel-side SEND ref for the named port by
 * installing it into kernel_space, then stores that kernel name in the
 * registry.  The caller retains their own SEND under whatever name they
 * passed in pd.name -- COPY_SEND semantics, not MOVE_SEND.
 *
 * No authentication: any task can claim any unused name.  Lifetime of
 * the registration is the kernel's SEND ref; bootstrap_unregister (or
 * a future DEAD_NAME-driven cleanup) is the only way for the entry to
 * disappear.
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
	 * Look up the port in the SENDER's space (where pd.name lives),
	 * then install a fresh SEND right in kernel_space.  space_install
	 * bumps the port's SEND count by one; the kernel keeps that ref
	 * alive for as long as the registration stands.
	 *
	 * No ref bump from space_lookup itself; the sender is blocked in
	 * the RPC waiting for our reply, so their name table can't drop
	 * pd.name from under us.
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
 * Ring-3 service unpublish.  Request layout is plain:
 *
 *	[ mach_msg_header | name(32) ]
 *
 * Removes the registry entry for `name` and drops the kernel-side SEND
 * ref that backed it.  If `name` was a kernel-resident service (the
 * five from services_init), this will still succeed and orphan it --
 * nothing in v1 distinguishes ring-3 publishers from kernel-init ones.
 * A future revision may stamp ownership at register time so deregister
 * can refuse to remove a kernel service.
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
