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
 * The host port.
 *
 * A single kernel-owned Mach port that answers questions about the
 * machine as a whole -- the style9 analogue of Mach's host special port
 * (the object behind mach_host_self()).  It is tagged PORT_SPECIAL_SERVICE
 * so a send routes straight to host_self_dispatch with no queueing and no
 * server thread, exactly like the clock / stats / tasks services.
 *
 * Two acquisition routes, one object:
 *
 *	- native style9 tasks look it up by name, bootstrap_lookup(SVC_HOST_NAME),
 *	  like any other kernel service;
 *
 *	- genuine Darwin binaries call mach_host_self(), which traps into
 *	  host_self_trap (kern/darwin.c) and lands in host_self_acquire,
 *	  installing a fresh SEND right directly in the caller's space.  This
 *	  mirrors real Mach, where mach_host_self() is a trap rather than a
 *	  string lookup.
 *
 * Either way the caller ends up with a SEND right and drives the port with
 * the HOST_OP_* opcodes below via mach_msg_rpc.
 *
 * Wire structs are ABI-stable and mirrored verbatim in lib/style9.h (the
 * ring-3 view); reordering a field silently misparses the reply.
 */

#define	SVC_HOST_NAME		"host"

/*
 * Opcodes (msgh_id) on a message sent to the host port.  Reply layouts
 * are pinned by _Static_assert below.
 *
 *	HOST_OP_PAGE_SIZE	-> svc_host_page_size_reply
 *	HOST_OP_INFO		-> svc_host_info_reply (HOST_BASIC_INFO shape)
 */
#define	HOST_OP_PAGE_SIZE	1
#define	HOST_OP_INFO		2

/*
 * cpu_type / cpu_subtype reported by HOST_OP_INFO.  Values match Darwin's
 * <mach/machine.h> so a genuine binary reading them sees the numbers it
 * expects: CPU_TYPE_X86_64 = CPU_TYPE_X86 (7) | CPU_ARCH_ABI64 (0x01000000).
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
 * Bring up the host port: mint the kernel-owned service port and publish
 * it under SVC_HOST_NAME in the bootstrap registry.  Must run after
 * bootstrap_init + services_init (it shares the bootstrap registry and the
 * kernel_space install path).  Idempotent across the call.
 */
void		host_init(void);

/*
 * The host port object, for the Darwin host_self_trap acquisition path.
 * NULL until host_init has run.
 */
struct port	*host_get_port(void);

/*
 * Persistent kernel_space SEND name for the host port (minted in host_init).
 * task_get_special_port COPY_SENDs it; MACH_PORT_NULL before host_init.
 */
mach_port_name_t host_get_kernel_name(void);

/*
 * Install a fresh SEND right to the host port in `space` and return the
 * new name through *name_out -- the mach_host_self() acquisition primitive.
 * Returns MACH_E_DEAD if host_init has not run, otherwise the space_install
 * result (MACH_MSG_OK on success).
 */
int		host_self_acquire(struct port_space *space,
		    mach_port_name_t *name_out);

/*
 * Synchronous dispatcher for messages sent to the host port.  Matches the
 * port_service_fn signature; invoked from mach_msg_send when the
 * destination's p_special is PORT_SPECIAL_SERVICE carrying this function.
 */
int		host_self_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);

#endif /* !_MACH_HOST_H_ */
