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
	r.sr_pmm_total_pages   = pmm_total_pages();
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

/* ---- "man" service ---------------------------------------------------- */

/*
 * Symbols emitted by `objcopy -I binary` when wrapping the rendered man
 * page text.  See the Makefile MAN_OBJS section: docs/man/port.9 ->
 * obj/port.9.txt -> obj/port_man.o with symbols _binary_port_9_txt_*.
 *
 * Adding a new page: drop docs/man/<name>.9 on disk (the Makefile auto-
 * detects it), then declare the matching extern symbols here and
 * append an entry to man_pages[] below.
 */
extern uint8_t	_binary_port_9_txt_start[];
extern uint8_t	_binary_port_9_txt_end[];

struct man_page {
	const char	*name;
	const uint8_t	*start;
	const uint8_t	*end;
};

static const struct man_page	man_pages[] = {
	{ "port", _binary_port_9_txt_start, _binary_port_9_txt_end },
};
static const size_t		man_pages_count =
	sizeof(man_pages) / sizeof(man_pages[0]);

static const struct man_page *
man_find(const char *name)
{
	const struct man_page	*p;
	size_t			 i, j;

	if (name == NULL || name[0] == '\0')
		return (NULL);

	for (i = 0; i < man_pages_count; i++) {
		p = &man_pages[i];
		for (j = 0; ; j++) {
			if (p->name[j] != name[j])
				break;
			if (p->name[j] == '\0')
				return (p);
		}
	}
	return (NULL);
}

/*
 * MAN_OP_GET dispatcher.  Reads the requested page name from the inline
 * body of the request, looks it up, and on success ships the rendered
 * text back as a single OOL descriptor.  On miss returns a bare reply
 * with msgh_id = MAN_NOT_FOUND so the caller can distinguish from a
 * generic send error.
 *
 * Uses mach_msg_send_trusted so send_capture_ool accepts the kernel
 * .rodata address in the OOL descriptor; without this exemption the
 * sender-VA range validation rejects every send from this dispatcher
 * (which runs in the ring-3 caller's thread context).
 */
static int
svc_man_dispatch(const struct mach_msg_header *req, struct port_space *from)
{
	const struct man_page		*page;
	const uint8_t			*body;
	const char			*name;
	size_t				 body_size, name_len, page_size;
	uint8_t				 reply_buf[sizeof(struct mach_msg_header) +
				    sizeof(struct mach_msg_body) +
				    sizeof(struct mach_msg_ool_descriptor)];
	struct mach_msg_header		 nfreply;
	struct mach_msg_header		*rhdr;
	struct mach_msg_body		*rbody;
	struct mach_msg_ool_descriptor	*rool;

	if (req->msgh_id != MAN_OP_GET)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);
	if (req->msgh_size < sizeof(struct mach_msg_header))
		return (MACH_E_INVAL);

	body      = (const uint8_t *)req + sizeof(struct mach_msg_header);
	body_size = req->msgh_size - sizeof(struct mach_msg_header);
	if (body_size == 0 || body_size > MAN_NAME_MAX)
		return (MACH_E_INVAL);

	name = (const char *)body;
	for (name_len = 0; name_len < body_size; name_len++) {
		if (body[name_len] == '\0')
			break;
	}
	if (name_len == body_size)
		return (MACH_E_INVAL);	/* not NUL-terminated */

	page = man_find(name);
	if (page == NULL) {
		nfreply.msgh_bits    =
		    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		nfreply.msgh_size    = sizeof(nfreply);
		nfreply.msgh_remote  = req->msgh_local;
		nfreply.msgh_local   = MACH_PORT_NULL;
		nfreply.msgh_voucher = 0;
		nfreply.msgh_id      = MAN_NOT_FOUND;
		return (mach_msg_send(from, &nfreply));
	}

	page_size = (size_t)(page->end - page->start);
	if (page_size == 0 || page_size > MACH_MSG_OOL_MAX_BYTES)
		return (MACH_E_NOMEM);

	rhdr  = (struct mach_msg_header *)reply_buf;
	rbody = (struct mach_msg_body *)
	    (reply_buf + sizeof(struct mach_msg_header));
	rool  = (struct mach_msg_ool_descriptor *)
	    (reply_buf + sizeof(struct mach_msg_header) +
	    sizeof(struct mach_msg_body));

	rhdr->msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    MACH_MSGH_BITS_COMPLEX;
	rhdr->msgh_size    = sizeof(reply_buf);
	rhdr->msgh_remote  = req->msgh_local;
	rhdr->msgh_local   = MACH_PORT_NULL;
	rhdr->msgh_voucher = 0;
	rhdr->msgh_id      = MAN_OP_GET;

	rbody->msgh_descriptor_count = 1;

	rool->type       = MACH_MSG_OOL_DESCRIPTOR;
	rool->copy       = MACH_MSG_PHYSICAL_COPY;
	rool->deallocate = 0;
	rool->pad        = 0;
	rool->size       = (uint32_t)page_size;
	rool->address    = (uint64_t)(uintptr_t)page->start;

	return (mach_msg_send_trusted(from, rhdr));
}

/* ---- "echool" service ------------------------------------------------- */

/*
 * Walk the descriptor area of the inbound message looking for the first
 * OOL descriptor; cap the search by msgh_size and use the same type-tag
 * dispatch as port_msg.c.  Returns the OOL descriptor's address+size
 * pair, or false if there is no usable OOL descriptor.  Validates
 * everything against the wire format before touching sender memory.
 */
static bool
echool_find_ool(const struct mach_msg_header *req, uint64_t *addr_out,
    uint32_t *size_out)
{
	const uint8_t			*buf;
	const struct mach_msg_body	*body;
	const struct mach_msg_ool_descriptor *od;
	const struct mach_msg_port_descriptor *pd;
	size_t				 hdrs_off, off, msg_size;
	uint32_t			 ndescs, walked;
	uint8_t				 t;

	if ((req->msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0)
		return (false);

	msg_size = req->msgh_size;
	hdrs_off = sizeof(struct mach_msg_header);
	if (msg_size < hdrs_off + sizeof(struct mach_msg_body))
		return (false);

	buf  = (const uint8_t *)req;
	body = (const struct mach_msg_body *)(buf + hdrs_off);
	ndescs = body->msgh_descriptor_count;

	off = hdrs_off + sizeof(struct mach_msg_body);
	for (walked = 0; walked < ndescs; walked++) {
		if (off >= msg_size)
			return (false);
		t = buf[off];
		if (t == MACH_MSG_PORT_DESCRIPTOR) {
			if (off + sizeof(struct mach_msg_port_descriptor) >
			    msg_size)
				return (false);
			pd = (const struct mach_msg_port_descriptor *)
			    (buf + off);
			(void)pd;
			off += sizeof(struct mach_msg_port_descriptor);
		} else if (t == MACH_MSG_OOL_DESCRIPTOR) {
			if (off + sizeof(struct mach_msg_ool_descriptor) >
			    msg_size)
				return (false);
			od = (const struct mach_msg_ool_descriptor *)
			    (buf + off);
			*addr_out = od->address;
			*size_out = od->size;
			return (true);
		} else {
			return (false);
		}
	}
	return (false);
}

static uint32_t
echool_fnv1a(const uint8_t *buf, uint32_t size)
{
	uint32_t	h, i;

	h = 0x811C9DC5u;
	for (i = 0; i < size; i++) {
		h ^= (uint32_t)buf[i];
		h *= 0x01000193u;
	}
	return (h);
}

/*
 * Special-port dispatcher: runs synchronously in the sender's thread,
 * so the sender's pmap is current and OOL VA dereferences resolve via
 * the user's own page tables.  Bypasses recv_install_ool entirely --
 * stress_ool already covers that leg of the pipeline; this oracle's job
 * is to verify userspace can construct a wire-format-correct OOL
 * descriptor that the kernel parses cleanly.
 *
 * Cap the payload at MACH_MSG_OOL_MAX_BYTES so a misbehaving sender
 * can't park us in a multi-megabyte byte-by-byte loop.  size_t is the
 * type the loop ranges over even though `size` is uint32_t, so the
 * 1 MiB cap is the real wall.
 */
static int
svc_echool_dispatch(const struct mach_msg_header *req, struct port_space *from)
{
	struct mach_msg_header	reply;
	const uint8_t		*payload;
	uint64_t		 addr;
	uint32_t		 size, sum;

	if (req->msgh_id != ECHOOL_OP_CHECKSUM)
		return (MACH_E_INVAL);
	if (req->msgh_local == MACH_PORT_NULL)
		return (MACH_E_INVAL);
	if (!echool_find_ool(req, &addr, &size))
		return (MACH_E_INVAL);
	if (size > MACH_MSG_OOL_MAX_BYTES)
		return (MACH_E_INVAL);

	if (size == 0) {
		sum = 0u;
	} else {
		payload = (const uint8_t *)(uintptr_t)addr;
		sum     = echool_fnv1a(payload, size);
	}

	reply.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	reply.msgh_size    = sizeof(reply);
	reply.msgh_remote  = req->msgh_local;
	reply.msgh_local   = MACH_PORT_NULL;
	reply.msgh_voucher = 0;
	reply.msgh_id      = sum;
	return (mach_msg_send(from, &reply));
}

/* ---- subsystem bring-up ----------------------------------------------- */

static struct port	*svc_clock_port;	/* (c) */
static struct port	*svc_stats_port;	/* (c) */
static struct port	*svc_tasks_port;	/* (c) */
static struct port	*svc_echool_port;	/* (c) */
static struct port	*svc_man_port;		/* (c) */

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

	svc_clock_port  = svc_register(SVC_CLOCK_NAME,  svc_clock_dispatch);
	svc_stats_port  = svc_register(SVC_STATS_NAME,  svc_stats_dispatch);
	svc_tasks_port  = svc_register(SVC_TASKS_NAME,  svc_tasks_dispatch);
	svc_echool_port = svc_register(SVC_ECHOOL_NAME, svc_echool_dispatch);
	svc_man_port    = svc_register(SVC_MAN_NAME,    svc_man_dispatch);
}
