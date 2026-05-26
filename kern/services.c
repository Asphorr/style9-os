/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "bootstrap.h"
#include "clock.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "services.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

extern struct port_space	*kernel_space;
extern struct port		*port_create_kernel_owned(uint8_t kind,
				    void *arg);
extern int			 port_install_send_in_kernel(struct port *,
				    mach_port_name_t *name_out);

/*
 * Common reply-shaping helper.  Builds a header in `hdr`, points the
 * inline payload bytes at `body` of `body_size`, and sends back to
 * `req->msgh_local` via COPY_SEND on the caller's space.  Returns the
 * mach_msg_send result so caller can propagate.
 *
 * Layout: [header | body].  No mach_msg_body / port_descriptor needed
 * since none of our services hand out further capabilities -- the
 * caller already has a SEND to *us* via the bootstrap lookup.
 */
static int
svc_reply_inline(const struct mach_msg_header *req, struct port_space *from,
    const void *body, size_t body_size)
{
	uint8_t		buf[sizeof(struct mach_msg_header) + 768];
	struct mach_msg_header	*rhdr;
	uint8_t		*dst;
	const uint8_t	*src;
	size_t		 total, i;

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

/* ---- "clock" service -------------------------------------------------- */

static int
svc_clock_dispatch(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_clock_reply	r;

	if (req->msgh_id != CLOCK_OP_GET)
		return (MACH_E_INVAL);

	r.cr_uptime_ms = clock_uptime_ms();
	r.cr_uptime_us = clock_uptime_us();
	r.cr_ticks     = clock_ticks();
	return (svc_reply_inline(req, from, &r, sizeof(r)));
}

/* ---- "stats" service -------------------------------------------------- */

static int
svc_stats_dispatch(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_stats_reply	r;
	struct task		*tasks[SVC_TASKS_MAX];
	size_t			 ntasks, threads, i;

	if (req->msgh_id != STATS_OP_GET)
		return (MACH_E_INVAL);

	/*
	 * Collect a best-effort thread count by summing t_nthreads
	 * across the live tasks.  Reads t_nthreads without taking
	 * t_lock; the value is a monotonic counter under one writer at
	 * a time, so a torn read yields a slightly stale total -- fine
	 * for a stats snapshot.
	 */
	ntasks  = task_snapshot(tasks, SVC_TASKS_MAX);
	threads = 0;
	for (i = 0; i < ntasks; i++)
		threads += tasks[i]->t_nthreads;

	r.sr_pmm_used_pages    = pmm_used_pages();
	r.sr_kmem_cached_pages = kmem_cached_pages();
	r.sr_kernel_inuse      = port_space_inuse(kernel_space);
	r.sr_task_count        = ntasks;
	r.sr_thread_count      = threads;
	r.sr_ctx_switches      = sched_context_switches();
	return (svc_reply_inline(req, from, &r, sizeof(r)));
}

/* ---- "tasks" service -------------------------------------------------- */

static int
svc_tasks_dispatch(const struct mach_msg_header *req, struct port_space *from)
{
	struct svc_tasks_reply	r;
	struct task		*snap[SVC_TASKS_MAX];
	size_t			 n, i, j;

	if (req->msgh_id != TASKS_OP_LIST)
		return (MACH_E_INVAL);

	/* zero the whole reply -- entries past `n` stay NUL */
	for (i = 0; i < sizeof(r); i++)
		((uint8_t *)&r)[i] = 0;

	n = task_snapshot(snap, SVC_TASKS_MAX);
	r.tr_count = (uint32_t)n;
	r.tr_pad   = 0;

	for (i = 0; i < n; i++) {
		struct task *t = snap[i];

		spin_lock(&t->t_lock);
		r.tr_entries[i].te_task_id  = t->t_id;
		r.tr_entries[i].te_nthreads = t->t_nthreads;
		spin_unlock(&t->t_lock);
		r.tr_entries[i].te_pad = 0;

		if (t->t_name != NULL) {
			for (j = 0;
			    j < SVC_TASKS_NAME_MAX - 1 &&
			    t->t_name[j] != '\0';
			    j++)
				r.tr_entries[i].te_name[j] = t->t_name[j];
		}
	}

	return (svc_reply_inline(req, from, &r, sizeof(r)));
}

/* ---- subsystem bring-up ----------------------------------------------- */

static struct port	*svc_clock_port;	/* (c) */
static struct port	*svc_stats_port;	/* (c) */
static struct port	*svc_tasks_port;	/* (c) */

/*
 * Each service is a kernel-owned PORT_SPECIAL_SERVICE port (the
 * dispatcher function lives in p_special_arg).  We then install a
 * SEND right into kernel_space and hand the resulting name to
 * bootstrap_register so lookups can resolve it via the normal
 * descriptor-translation path.
 */
static struct port *
svc_register(const char *name, port_service_fn fn)
{
	mach_port_name_t	kn;
	struct port		*p;

	p = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)fn);
	if (p == NULL)
		panic("services: port_create_kernel_owned(%s)", name);

	if (port_install_send_in_kernel(p, &kn) != MACH_MSG_OK)
		panic("services: install %s in kernel_space", name);

	if (bootstrap_register(name, kn) != MACH_MSG_OK)
		panic("services: bootstrap_register(%s)", name);

	kprintf("svc: %s -> kernel name %u\n", name, (unsigned)kn);
	return (p);
}

void
services_init(void)
{

	svc_clock_port = svc_register(SVC_CLOCK_NAME, svc_clock_dispatch);
	svc_stats_port = svc_register(SVC_STATS_NAME, svc_stats_dispatch);
	svc_tasks_port = svc_register(SVC_TASKS_NAME, svc_tasks_dispatch);
}
