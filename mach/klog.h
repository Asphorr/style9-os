/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_KLOG_H_
#define	_SYS_KLOG_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Structured kernel log.
 *
 * A ring of fixed-size entries, each with a level, an 8-byte source tag,
 * an uptime stamp in ms, a sequence number and up to KLOG_LINE_MAX bytes
 * of text.  Each entry is also echoed through kprintf, and so reaches
 * every console tty mirrors to.
 *
 * Also a Mach service, "klog", so ring-3 code can log the same way:
 *	KLOG_OP_WRITE  -- append a line (payload = klog_write_request)
 *	KLOG_OP_TAIL   -- read the last entries (reply = klog_tail_reply)
 *
 * WIRE FORMAT structs are ABI-stable, as in port.h.
 */

#define	KLOG_LEVEL_DEBUG	1
#define	KLOG_LEVEL_INFO		2
#define	KLOG_LEVEL_WARN		3
#define	KLOG_LEVEL_ERROR	4

#define	KLOG_SRC_MAX		8	/* "boot", "pmm", "sched", ... */
#define	KLOG_LINE_MAX		96	/* per-entry text bytes        */
#define	KLOG_RING_ENTRIES	128	/* ~16 KiB of ring             */
#define	KLOG_TAIL_BATCH		16	/* max entries per TAIL reply  */

/* WIRE FORMAT.  ABI-stable. */
struct klog_entry {
	uint64_t	ke_uptime_ms;
	uint32_t	ke_seq;
	uint8_t		ke_level;
	uint8_t		ke_pad[3];
	char		ke_src[KLOG_SRC_MAX];
	char		ke_text[KLOG_LINE_MAX];
};

_Static_assert(sizeof(struct klog_entry) == 8 + 4 + 1 + 3 +
    KLOG_SRC_MAX + KLOG_LINE_MAX,
    "klog_entry must be 120 bytes (wire format)");

/* ---- Mach service wire formats ---- */

#define	SVC_KLOG_NAME		"klog"
#define	KLOG_OP_WRITE		1
#define	KLOG_OP_TAIL		2

/* WIRE FORMAT.  ABI-stable. */
struct klog_write_request {
	uint8_t		kwr_level;
	uint8_t		kwr_pad[7];
	char		kwr_src[KLOG_SRC_MAX];
	char		kwr_text[KLOG_LINE_MAX];
};

_Static_assert(sizeof(struct klog_write_request) ==
    8 + KLOG_SRC_MAX + KLOG_LINE_MAX,
    "klog_write_request layout pinned");

/* WIRE FORMAT.  ABI-stable. */
struct klog_tail_reply {
	uint32_t		ktr_count;
	uint32_t		ktr_pad;
	struct klog_entry	ktr_entries[KLOG_TAIL_BATCH];
};

_Static_assert(sizeof(struct klog_tail_reply) ==
    8 + KLOG_TAIL_BATCH * sizeof(struct klog_entry),
    "klog_tail_reply layout pinned");

/* ---- kernel-side API ---- */

void	klog_init(void);

/*
 * Start the "klog" service: create its PORT_SPECIAL_SERVICE port,
 * register it with bootstrap, and log a first INFO entry.  After
 * bootstrap_init, task_subsystem_init and clock_init (entries are
 * stamped with clock_uptime_ms).
 */
void	klog_service_init(void);

/*
 * Append one line to the ring and echo it.  A NULL `src` becomes
 * "kern", NULL `text` "(null)", and an unknown level INFO.  Text beyond
 * KLOG_LINE_MAX-1 bytes is truncated; embedded newlines are not
 * filtered, and the echo always ends the line.
 */
void	klog(uint8_t level, const char *src, const char *text);

/*
 * Snapshot up to `max` of the most recent entries into `out`, oldest
 * first.  Returns how many were written.
 */
size_t	klog_snapshot_tail(struct klog_entry *out, size_t max);

/* Three-letter tag for a level ("DBG", "INF", "WRN", "ERR"). */
const char	*klog_level_name(uint8_t level);

#endif /* !_SYS_KLOG_H_ */
