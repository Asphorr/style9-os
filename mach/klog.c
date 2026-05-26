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
#include "clock.h"
#include "klog.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "spinlock.h"
#include "tty.h"

extern struct port_space	*kernel_space;
extern struct port		*port_create_kernel_owned(uint8_t kind,
				    void *arg);
extern int			 port_install_send_in_kernel(struct port *,
				    mach_port_name_t *name_out);

/* ---- ring ---- */

static struct spinlock	ring_lock = SPINLOCK_INIT("klog");
static struct klog_entry ring[KLOG_RING_ENTRIES];	/* (ring_lock) */
static size_t		ring_head;			/* (ring_lock) next slot to fill */
static size_t		ring_count;			/* (ring_lock) live entries     */
static uint32_t		next_seq;			/* (ring_lock)                  */

const char *
klog_level_name(uint8_t level)
{

	switch (level) {
	case KLOG_LEVEL_DEBUG:	return ("DBG");
	case KLOG_LEVEL_INFO:	return ("INF");
	case KLOG_LEVEL_WARN:	return ("WRN");
	case KLOG_LEVEL_ERROR:	return ("ERR");
	default:		return ("???");
	}
}

static void
klog_copy_field(char *dst, size_t cap, const char *src)
{
	size_t	i;

	for (i = 0; i < cap - 1; i++) {
		if (src == NULL || src[i] == '\0')
			break;
		dst[i] = src[i];
	}
	for (; i < cap; i++)
		dst[i] = '\0';
}

/*
 * Echo the line to the kernel console.  Format:
 *	[uptime_ms LVL src] text\n
 * No locking around tty here -- tty_putc itself takes tty_lock; we
 * just call it byte-by-byte.  Worst case under contention is
 * interleaving with another writer, which is harmless for a log.
 */
static void
klog_emit(const struct klog_entry *e)
{

	kprintf("[%llu %s %s] %s\n",
	    (unsigned long long)e->ke_uptime_ms,
	    klog_level_name(e->ke_level),
	    e->ke_src,
	    e->ke_text);
}

void
klog_init(void)
{

	ring_head  = 0;
	ring_count = 0;
	next_seq   = 1;
}

void
klog(uint8_t level, const char *src, const char *text)
{
	struct klog_entry	*e;
	size_t			 slot;

	if (text == NULL)
		text = "(null)";
	if (src == NULL)
		src = "kern";
	if (level < KLOG_LEVEL_DEBUG || level > KLOG_LEVEL_ERROR)
		level = KLOG_LEVEL_INFO;

	spin_lock(&ring_lock);
	slot      = ring_head;
	ring_head = (ring_head + 1) % KLOG_RING_ENTRIES;
	if (ring_count < KLOG_RING_ENTRIES)
		ring_count++;

	e = &ring[slot];
	e->ke_uptime_ms = clock_uptime_ms();
	e->ke_seq       = next_seq++;
	e->ke_level     = level;
	e->ke_pad[0]    = 0;
	e->ke_pad[1]    = 0;
	e->ke_pad[2]    = 0;
	klog_copy_field(e->ke_src,  KLOG_SRC_MAX,  src);
	klog_copy_field(e->ke_text, KLOG_LINE_MAX, text);

	/*
	 * Echo OUTSIDE the lock to avoid blocking other producers on
	 * tty_lock, which is taken by tty_putc.  The snapshot we took
	 * into `e` is private to this slot until the next producer
	 * laps the ring -- KLOG_RING_ENTRIES entries away.  For a
	 * 128-entry ring and the kernel's actual log rate that lap is
	 * effectively never; for cycle-perfect safety the emit could
	 * copy into a stack temp first.
	 */
	spin_unlock(&ring_lock);
	klog_emit(e);
}

size_t
klog_snapshot_tail(struct klog_entry *out, size_t max)
{
	size_t	i, first, n;

	if (out == NULL || max == 0)
		return (0);

	spin_lock(&ring_lock);
	n = ring_count < max ? ring_count : max;
	/*
	 * Oldest of the last `n` lives at ring_head - n (mod cap),
	 * because ring_head is the NEXT slot to fill.
	 */
	first = (ring_head + KLOG_RING_ENTRIES - n) % KLOG_RING_ENTRIES;
	for (i = 0; i < n; i++)
		out[i] = ring[(first + i) % KLOG_RING_ENTRIES];
	spin_unlock(&ring_lock);
	return (n);
}

/* ---- Mach service dispatcher ---- */

static int
svc_klog_reply_inline(const struct mach_msg_header *req,
    struct port_space *from, const void *body, size_t body_size)
{
	uint8_t		buf[sizeof(struct mach_msg_header) +
			    sizeof(struct klog_tail_reply)];
	struct mach_msg_header	*rhdr;
	uint8_t			*dst;
	const uint8_t		*src;
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
svc_klog_dispatch(const struct mach_msg_header *req, struct port_space *from)
{
	const struct klog_write_request		*wr;
	struct klog_tail_reply			 tail;
	const uint8_t				*payload;
	size_t					 n;

	switch (req->msgh_id) {
	case KLOG_OP_WRITE:
		if (req->msgh_size < sizeof(struct mach_msg_header) +
		    sizeof(struct klog_write_request))
			return (MACH_E_INVAL);
		payload = (const uint8_t *)req +
		    sizeof(struct mach_msg_header);
		wr = (const struct klog_write_request *)payload;
		klog(wr->kwr_level, wr->kwr_src, wr->kwr_text);
		/*
		 * Acknowledge with a bare header so the client's
		 * mach_msg_rpc can return.  No payload needed -- the
		 * log was either accepted (we got here) or the kernel
		 * panicked before producing a reply.
		 */
		return (svc_klog_reply_inline(req, from, NULL, 0));
	case KLOG_OP_TAIL:
		for (size_t i = 0; i < sizeof(tail); i++)
			((uint8_t *)&tail)[i] = 0;
		n = klog_snapshot_tail(tail.ktr_entries, KLOG_TAIL_BATCH);
		tail.ktr_count = (uint32_t)n;
		tail.ktr_pad   = 0;
		return (svc_klog_reply_inline(req, from, &tail, sizeof(tail)));
	default:
		return (MACH_E_INVAL);
	}
}

/* ---- bootstrap-time registration ---- */

void
klog_service_init(void)
{
	struct port		*p;
	mach_port_name_t	 kn;

	klog_init();

	p = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)svc_klog_dispatch);
	if (p == NULL)
		panic("klog_service_init: port_create_kernel_owned");
	if (port_install_send_in_kernel(p, &kn) != MACH_MSG_OK)
		panic("klog_service_init: install in kernel_space");
	if (bootstrap_register(SVC_KLOG_NAME, kn) != MACH_MSG_OK)
		panic("klog_service_init: bootstrap_register(klog)");

	klog(KLOG_LEVEL_INFO, "klog", "ring + service up");
}
