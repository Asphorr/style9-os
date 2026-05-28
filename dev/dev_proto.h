/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DEV_PROTO_H_
#define	_SYS_DEV_PROTO_H_

#include <stdint.h>

/*
 * Generic device-driver protocol.
 *
 * A driver registers itself in the bootstrap port under the name
 * "dev/<NAME>" (e.g. "dev/kbd").  The registered port is its CONTROL
 * port -- a PORT_SPECIAL_SERVICE port whose dispatcher handles the
 * DEV_OP_* opcodes below.  The protocol is small and intentionally
 * subsetted by class: a stream input driver implements INFO + OPEN_STREAM,
 * an output driver implements INFO + WRITE, a block device would
 * implement INFO + READ + WRITE + IOCTL.  INFO is always supported.
 *
 * Wire structs below are ABI-stable: existing fields keep their offsets,
 * new fields append, and the size is pinned by _Static_assert.  Reordering
 * an existing field breaks any consumer compiled against an older layout.
 * Each struct is preceded by a WIRE FORMAT banner for grep-ability.
 *
 * All replies COPY_SEND back through req->msgh_local; the inline-reply
 * fast path applies to bare replies (INFO, WRITE) and skips the kmalloc
 * + enqueue + wake of the queue path entirely.
 */

#define	DEV_OP_INFO		1	/* request: header.  reply: dev_info_reply  */
#define	DEV_OP_OPEN_STREAM	2	/* request: header.  reply: complex w/ port_desc */
#define	DEV_OP_WRITE		3	/* request: dev_write_request.  reply: dev_write_reply */

/*
 * BLOCK device ops -- random-access sector-addressable storage.
 *	GEOM        returns drive metadata (sector size, total sectors, model)
 *	READ_BLOCK  reads up to DEV_BLOCK_MAX_SECTORS at a given LBA
 *	WRITE_BLOCK writes up to DEV_BLOCK_MAX_SECTORS at a given LBA
 *	SYNC        flush write cache so previous writes hit the medium
 *
 * Sector size is fixed at 512 for the protocol.  Drives reporting
 * something else are rejected at probe time -- the wire shape would
 * have to change to accommodate them, and 512 is universal on the
 * supported hardware.
 */
#define	DEV_OP_GEOM		4	/* request: header.  reply: dev_geom_reply  */
#define	DEV_OP_READ_BLOCK	5	/* request: dev_block_io_req.  reply: dev_block_read_reply */
#define	DEV_OP_WRITE_BLOCK	6	/* request: dev_block_write_req.  reply: dev_block_io_reply */
#define	DEV_OP_SYNC		7	/* request: header.  reply: dev_block_io_reply */

/*
 * Device classes.  The kind tells a consumer how to talk to the device:
 *	STREAM_RX	push-style input (kbd, uart-rx, mouse).  OPEN_STREAM
 *			hands the consumer a SEND right to the underlying
 *			Mach port; the consumer recvs from it to get bytes.
 *	STREAM_TX	accepts WRITE bytes (uart-tx, future tty).
 *	CHAR		random-access byte device (future: rtc, random).
 *	BLOCK		fixed-size sector device (future: nvme, ramdisk).
 */
#define	DEV_KIND_NONE		0
#define	DEV_KIND_STREAM_RX	1
#define	DEV_KIND_STREAM_TX	2
#define	DEV_KIND_CHAR		3
#define	DEV_KIND_BLOCK		4

#define	DEV_F_READABLE		0x01	/* DEV_OP_OPEN_STREAM works  */
#define	DEV_F_WRITABLE		0x02	/* DEV_OP_WRITE works        */
#define	DEV_F_STREAM		0x04	/* events arrive unsolicited */

#define	DEV_NAME_MAX		16	/* short name, post-"dev/" prefix */
#define	DEV_WRITE_MAX		256	/* per-call write payload cap */

/*
 * Block-IO sizing.  At 512 B/sector, 4 sectors is the largest run that
 * fits in MAX_MSG_BYTES (4096) with room for header + descriptor.  A
 * higher-level layer (future FS) chains calls if it wants more.
 */
#define	DEV_BLOCK_SECTOR_BYTES	512
#define	DEV_BLOCK_MAX_SECTORS	4
#define	DEV_BLOCK_MAX_BYTES	\
	(DEV_BLOCK_SECTOR_BYTES * DEV_BLOCK_MAX_SECTORS)

/*
 * Reply for DEV_OP_INFO.  Sits right after the mach_msg_header in the
 * reply message.  24 bytes; the fast path treats this as a bare reply.
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_info_reply {
	char		dir_name[DEV_NAME_MAX];	/* NUL-terminated         */
	uint32_t	dir_kind;		/* DEV_KIND_*             */
	uint32_t	dir_flags;		/* DEV_F_*                */
};

_Static_assert(sizeof(struct dev_info_reply) == DEV_NAME_MAX + 8,
    "dev_info_reply must be 24 bytes (wire format)");

/*
 * Request for DEV_OP_WRITE.  dwr_len is the number of valid bytes in
 * dwr_data; the driver writes those out and returns dwr_written in the
 * reply (may be less than dwr_len if a short write occurred).
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_write_request {
	uint32_t	dwr_len;
	uint32_t	dwr_pad;
	uint8_t		dwr_data[DEV_WRITE_MAX];
};

_Static_assert(sizeof(struct dev_write_request) == 8 + DEV_WRITE_MAX,
    "dev_write_request must be 264 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable. */
struct dev_write_reply {
	int32_t		dwr_rv;		/* MACH_MSG_OK or MACH_E_*     */
	uint32_t	dwr_written;	/* bytes actually consumed     */
};

_Static_assert(sizeof(struct dev_write_reply) == 8,
    "dev_write_reply must be 8 bytes (wire format)");

/* ---- block-device wire formats ---- */

/*
 * Reply to DEV_OP_GEOM.  dgr_model is the device-reported model string
 * (NUL-padded).  dgr_total_sectors is in dgr_sector_bytes units; a
 * 1 GiB QEMU disk reports total=0x200000, bytes=512.
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_geom_reply {
	int32_t		dgr_rv;
	uint32_t	dgr_sector_bytes;
	uint64_t	dgr_total_sectors;
	uint32_t	dgr_flags;	/* reserved (LBA48-supported etc.) */
	uint32_t	dgr_pad;
	char		dgr_model[40];
};

_Static_assert(sizeof(struct dev_geom_reply) == 64,
    "dev_geom_reply must be 64 bytes (wire format)");

/*
 * Request for DEV_OP_READ_BLOCK.  Asks for `dbr_count` sectors starting
 * at LBA `dbr_lba`.  Caller's reply buffer must hold a dev_block_read_reply
 * (header + status + count * 512 bytes).
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_io_req {
	uint64_t	dbr_lba;
	uint32_t	dbr_count;	/* sectors; 1..DEV_BLOCK_MAX_SECTORS */
	uint32_t	dbr_pad;
};

_Static_assert(sizeof(struct dev_block_io_req) == 16,
    "dev_block_io_req must be 16 bytes (wire format)");

/*
 * Reply for DEV_OP_READ_BLOCK on success.  dbr_count is the actual
 * sector count delivered (== request count on success, 0 on error).
 * dbr_data carries (dbr_count * 512) bytes of payload, padded out to
 * the full DEV_BLOCK_MAX_BYTES so the buffer size is fixed and the
 * receiver doesn't need to special-case partial messages.
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_read_reply {
	int32_t		dbr_rv;
	uint32_t	dbr_count;
	uint8_t		dbr_data[DEV_BLOCK_MAX_BYTES];
};

_Static_assert(sizeof(struct dev_block_read_reply) == 8 + DEV_BLOCK_MAX_BYTES,
    "dev_block_read_reply must be 2056 bytes (wire format)");

/*
 * Request for DEV_OP_WRITE_BLOCK.  Carries the (lba, count) tuple plus
 * the inline data payload.  Symmetric to the read reply.
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_write_req {
	uint64_t	dbw_lba;
	uint32_t	dbw_count;	/* sectors */
	uint32_t	dbw_pad;
	uint8_t		dbw_data[DEV_BLOCK_MAX_BYTES];
};

_Static_assert(sizeof(struct dev_block_write_req) == 16 + DEV_BLOCK_MAX_BYTES,
    "dev_block_write_req must be 2064 bytes (wire format)");

/*
 * Reply for DEV_OP_WRITE_BLOCK and DEV_OP_SYNC.  Same shape as
 * dev_write_reply but field names track the block semantics.
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_io_reply {
	int32_t		dbr_rv;
	uint32_t	dbr_sectors;	/* sectors actually transferred */
};

_Static_assert(sizeof(struct dev_block_io_reply) == 8,
    "dev_block_io_reply must be 8 bytes (wire format)");

#endif /* !_SYS_DEV_PROTO_H_ */
