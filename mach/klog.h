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
 * One ring buffer of fixed-size entries; each entry carries a level,
 * an 8-byte source tag, a wall-clock-ms stamp, a monotonic sequence
 * number, and up to KLOG_LINE_MAX bytes of text.  Submissions append
 * to the ring AND echo to tty (which already mirrors to COM1 +
 * debugcon), so the same write lands on the VGA console, the serial
 * port, and the QEMU debugcon stream in one call.
 *
 * Exposed as a Mach service named "klog" so kernel internals + future
 * ring-3 code share the same producer interface.  Two ops:
 *	KLOG_OP_WRITE  -- append a line (payload = klog_write_request)
 *	KLOG_OP_TAIL   -- read the last entries (reply = klog_tail_reply)
 *
 * The wire formats below are pinned by _Static_assert so layouts stay
 * stable across the kernel/userland boundary.
 */

#define	KLOG_LEVEL_DEBUG	1
#define	KLOG_LEVEL_INFO		2
#define	KLOG_LEVEL_WARN		3
#define	KLOG_LEVEL_ERROR	4

#define	KLOG_SRC_MAX		8	/* "boot", "pmm", "sched", ... */
#define	KLOG_LINE_MAX		96	/* per-entry text bytes        */
#define	KLOG_RING_ENTRIES	128	/* ~16 KiB of ring             */
#define	KLOG_TAIL_BATCH		16	/* max entries per TAIL reply  */

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

struct klog_write_request {
	uint8_t		kwr_level;
	uint8_t		kwr_pad[7];
	char		kwr_src[KLOG_SRC_MAX];
	char		kwr_text[KLOG_LINE_MAX];
};

_Static_assert(sizeof(struct klog_write_request) ==
    8 + KLOG_SRC_MAX + KLOG_LINE_MAX,
    "klog_write_request layout pinned");

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
 * Stand up the "klog" Mach service.  Allocates a PORT_SPECIAL_SERVICE
 * port, installs SEND in kernel_space, registers it with the bootstrap
 * port, and emits the first INFO entry as a smoke test.
 *
 * Must run after bootstrap_init + task_subsystem_init (so kernel_space
 * exists with a real owner), and after clock_init (klog stamps each
 * entry with clock_uptime_ms).
 */
void	klog_service_init(void);

/*
 * Append a single line to the ring AND echo it to tty.  Idempotent if
 * `src` is NULL ("kern" is substituted).  If `text` is longer than
 * KLOG_LINE_MAX-1 bytes it is truncated; no embedded newlines are
 * filtered, but a missing trailing '\n' is added for the tty echo so
 * lines remain one-per-row on the VGA console.
 */
void	klog(uint8_t level, const char *src, const char *text);

/*
 * Snapshot up to `max` of the most recent entries into `out`, oldest
 * first.  Returns how many were written.
 */
size_t	klog_snapshot_tail(struct klog_entry *out, size_t max);

/*
 * One-letter abbreviation of a level for prefix printing.  Useful
 * outside klog.c when constructing display strings.
 */
const char	*klog_level_name(uint8_t level);

#endif /* !_SYS_KLOG_H_ */
