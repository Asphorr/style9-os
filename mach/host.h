/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACH_HOST_H_
#define	_MACH_HOST_H_

#include <stddef.h>
#include <stdint.h>

#include "port.h"

/*
 * The host port: one kernel-owned port answering questions about the
 * machine, Mach's host special port (behind mach_host_self()).  It is
 * PORT_SPECIAL_SERVICE, so a send goes straight to host_self_dispatch,
 * as for the clock / stats / tasks services.
 *
 * Two ways to one object:
 *
 *	- native tasks look it up with bootstrap_lookup(SVC_HOST_NAME);
 *
 *	- Darwin binaries call mach_host_self(), a trap (host_self_trap in
 *	  kern/darwin.c) into host_self_acquire, which installs a SEND
 *	  right straight in the caller's space, as real Mach does.
 *
 * The caller then drives it with HOST_OP_* through mach_msg_rpc.  The
 * wire structs are ABI-stable and mirrored in lib/style9.h.
 */

#define	SVC_HOST_NAME		"host"

/*
 * Host port msgh_id values and their replies:
 *
 *	HOST_OP_PAGE_SIZE	-> svc_host_page_size_reply
 *	HOST_OP_INFO		-> svc_host_info_reply (HOST_BASIC_INFO shape)
 */
#define	HOST_OP_PAGE_SIZE	1
#define	HOST_OP_INFO		2

/*
 * HOST_OP_INFO's cpu_type / cpu_subtype, as in Darwin's <mach/machine.h>:
 * CPU_TYPE_X86_64 = CPU_TYPE_X86 (7) | CPU_ARCH_ABI64 (0x01000000).
 */
#define	HOST_CPU_TYPE_X86_64		0x01000007
#define	HOST_CPU_SUBTYPE_X86_64_ALL	3

/* WIRE FORMAT.  ABI-stable.  Reply body for HOST_OP_PAGE_SIZE. */
struct svc_host_page_size_reply {
	uint32_t	hps_page_size;
	uint32_t	hps_pad;
};

_Static_assert(sizeof(struct svc_host_page_size_reply) == 8,
    "svc_host_page_size_reply must be 8 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable.  Reply body for HOST_OP_INFO. */
struct svc_host_info_reply {
	uint32_t	hi_max_cpus;
	uint32_t	hi_avail_cpus;
	uint64_t	hi_memory_size;		/* physical RAM, bytes       */
	uint32_t	hi_cpu_type;
	uint32_t	hi_cpu_subtype;
	uint64_t	hi_memory_free;		/* free RAM right now, bytes */
};

_Static_assert(sizeof(struct svc_host_info_reply) == 32,
    "svc_host_info_reply must be 32 bytes (wire format)");

/*
 * Create the host port and register it as SVC_HOST_NAME.  After
 * bootstrap_init and services_init.  Idempotent.
 */
void		host_init(void);

/* The host port object; NULL until host_init. */
struct port	*host_get_port(void);

/*
 * The host port's kernel_space name, which task_get_special_port
 * COPY_SENDs; MACH_PORT_NULL before host_init.
 */
mach_port_name_t host_get_kernel_name(void);

/*
 * mach_host_self(): install a SEND right to the host port in `space`,
 * returning the name in *name_out.  MACH_E_DEAD before host_init,
 * otherwise space_install's result.
 */
int		host_self_acquire(struct port_space *space,
		    mach_port_name_t *name_out);

/* The host port's port_service_fn. */
int		host_self_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);

#endif /* !_MACH_HOST_H_ */
