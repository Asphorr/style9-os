/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "bootstrap.h"
#include "host.h"
#include "kprintf.h"
#include "panic.h"
#include "pmm.h"
#include "port.h"
#include "port_internal.h"

/*
 * Host port -- the kernel-owned Mach object behind mach_host_self().  See
 * host.h for the acquisition model + wire protocol.  Structurally this is
 * a PORT_SPECIAL_SERVICE port like the ones in services.c; it lives in its
 * own file because the host is conceptually distinct (it is the machine,
 * not a kernel subsystem) and has room to grow more HOST_OP_* opcodes.
 *
 * Hooks into port_object.c: port_create_kernel_owned mints a kernel-owned
 * port (RECEIVE held, no name in any space) with a p_special tag pre-set;
 * port_install_send_in_kernel wires a SEND right for it into kernel_space
 * so bootstrap can hand it out via the normal descriptor-translation path.
 */
extern struct port	*port_create_kernel_owned(uint8_t special_kind,
			    void *special_arg);
extern int		 port_install_send_in_kernel(struct port *p,
			    mach_port_name_t *name_out);

static struct port	*the_host_port;		/* (c) */
static mach_port_name_t	 the_host_kn;		/* (c) kernel_space SEND */

/*
 * Reply helper: ship `body` of `body_size` bytes back to req->msgh_local as
 * a bare [header | body] message via COPY_SEND on the caller's space.  Same
 * shape as services.c's svc_reply_inline -- the host port hands out no
 * further capabilities, so no descriptors are needed.
 */
static int
host_reply_inline(const struct mach_msg_header *req, struct port_space *from,
    const void *body, size_t body_size)
{
	uint8_t			 buf[sizeof(struct mach_msg_header) + 64];
	struct mach_msg_header	*rhdr;
	const uint8_t		*src;
	uint8_t			*dst;
	size_t			 total, i;

	total = sizeof(struct mach_msg_header) + body_size;
	if (total > sizeof(buf))
		return (MACH_E_NOMEM);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	rhdr = (struct mach_msg_header *)buf;
	rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	rhdr->msgh_size    = (uint32_t)total;
	rhdr->msgh_remote  = req->msgh_local;
	rhdr->msgh_local   = MACH_PORT_NULL;
	rhdr->msgh_voucher = 0;
	rhdr->msgh_id      = req->msgh_id;

	dst = buf + sizeof(struct mach_msg_header);
	src = (const uint8_t *)body;
	for (i = 0; i < body_size; i++)
		dst[i] = src[i];

	return (mach_msg_send(from, rhdr));
}

static int
host_dispatch_page_size(const struct mach_msg_header *req,
    struct port_space *from)
{
	struct svc_host_page_size_reply	r;

	r.hps_page_size = (uint32_t)PAGE_SIZE;
	r.hps_pad       = 0;
	return (host_reply_inline(req, from, &r, sizeof(r)));
}

static int
host_dispatch_info(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_host_info_reply	r;
	uint64_t			total_pages;
	uint64_t			used_pages;

	/*
	 * Single-CPU machine for now; report 1/1 rather than probing an
	 * APIC count we do not yet track.  Memory comes straight off the
	 * physical allocator's running totals -- the same numbers the
	 * "stats" service reports, recast from pages to bytes here so a
	 * Darwin caller reading HOST_BASIC_INFO sees byte counts.
	 */
	total_pages = pmm_total_pages();
	used_pages  = pmm_used_pages();

	r.hi_max_cpus    = 1;
	r.hi_avail_cpus  = 1;
	r.hi_memory_size = total_pages * PAGE_SIZE;
	r.hi_cpu_type    = HOST_CPU_TYPE_X86_64;
	r.hi_cpu_subtype = HOST_CPU_SUBTYPE_X86_64_ALL;
	r.hi_memory_free = (total_pages - used_pages) * PAGE_SIZE;
	return (host_reply_inline(req, from, &r, sizeof(r)));
}

int
host_self_dispatch(const struct mach_msg_header *req, struct port_space *from)
{

	if (req == NULL || from == NULL)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);

	switch (req->msgh_id) {
	case HOST_OP_PAGE_SIZE:
		return (host_dispatch_page_size(req, from));
	case HOST_OP_INFO:
		return (host_dispatch_info(req, from));
	default:
		return (MACH_E_INVAL);
	}
}

int
host_self_acquire(struct port_space *space, mach_port_name_t *name_out)
{

	if (space == NULL || name_out == NULL)
		return (MACH_E_INVAL);
	if (the_host_port == NULL)
		return (MACH_E_DEAD);
	return (space_install(space, the_host_port, MACH_PORT_RIGHT_SEND,
	    name_out));
}

struct port *
host_get_port(void)
{

	return (the_host_port);
}

/*
 * Persistent kernel_space SEND name for the host port, minted in host_init.
 * task_get_special_port COPY_SENDs it to hand the host port to a task;
 * MACH_PORT_NULL before host_init has run.
 */
mach_port_name_t
host_get_kernel_name(void)
{

	return (the_host_kn);
}

void
host_init(void)
{
	mach_port_name_t	kn;

	if (the_host_port != NULL)
		return;

	the_host_port = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)host_self_dispatch);
	if (the_host_port == NULL)
		panic("host_init: port_create_kernel_owned failed");

	if (port_install_send_in_kernel(the_host_port, &kn) != MACH_MSG_OK)
		panic("host_init: install host port in kernel_space");
	the_host_kn = kn;

	if (bootstrap_register(SVC_HOST_NAME, kn) != MACH_MSG_OK)
		panic("host_init: bootstrap_register(host)");

	kprintf("host: %s -> kernel name %u (page_size=%u)\n",
	    SVC_HOST_NAME, (unsigned)kn, (unsigned)PAGE_SIZE);
}
