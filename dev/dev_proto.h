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
 * A driver registers in the bootstrap port as "dev/<NAME>" (e.g.
 * "dev/kbd").  The registered port is its control port, a
 * PORT_SPECIAL_SERVICE port whose dispatcher handles the DEV_OP_* codes
 * below, subsetted by class: stream input implements INFO + OPEN_STREAM,
 * output INFO + WRITE, a block device INFO + GEOM + READ_BLOCK +
 * WRITE_BLOCK + SYNC.  INFO is always supported.
 *
 * The wire structs are ABI-stable: fields keep their offsets, new ones
 * append, and each size is pinned by _Static_assert.  Each struct carries
 * a WIRE FORMAT banner for grep.
 *
 * Replies go to req->msgh_local.  Bare replies (INFO, WRITE) take the
 * inline-reply fast path, skipping the queue's kmalloc, enqueue and wake.
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
 * The protocol's sector size is fixed at 512; ata_drv assumes 512-byte
 * sectors without checking.
 */
#define	DEV_OP_GEOM		4	/* request: header.  reply: dev_geom_reply  */
#define	DEV_OP_READ_BLOCK	5	/* request: dev_block_io_req.  reply: dev_block_read_reply */
#define	DEV_OP_WRITE_BLOCK	6	/* request: dev_block_write_req.  reply: dev_block_io_reply */
#define	DEV_OP_SYNC		7	/* request: header.  reply: dev_block_io_reply */

/*
 * Device classes.  The kind tells a consumer how to talk to the device:
 *	STREAM_RX	push-style input (kbd, uart, mouse).  OPEN_STREAM
 *			moves the stream port's RECEIVE right to the
 *			consumer, which receives the events from it.
 *	STREAM_TX	accepts WRITE bytes.
 *	CHAR		random-access byte device (none yet).
 *	BLOCK		fixed-size sector device (ATA disks).
 */
#define	DEV_KIND_NONE		0
#define	DEV_KIND_STREAM_RX	1
#define	DEV_KIND_STREAM_TX	2
#define	DEV_KIND_CHAR		3
#define	DEV_KIND_BLOCK		4

#define	DEV_F_READABLE		0x01	/* OPEN_STREAM or READ_BLOCK */
#define	DEV_F_WRITABLE		0x02	/* WRITE or WRITE_BLOCK      */
#define	DEV_F_STREAM		0x04	/* events arrive unsolicited */

#define	DEV_NAME_MAX		16	/* short name, post-"dev/" prefix */
#define	DEV_WRITE_MAX		256	/* per-call write payload cap */

/*
 * Block-IO sizing: 4 sectors (2 KiB) per call keeps a request or reply,
 * header included, inside MAX_MSG_BYTES (4096).  Larger transfers take
 * several calls.
 */
#define	DEV_BLOCK_SECTOR_BYTES	512
#define	DEV_BLOCK_MAX_SECTORS	4
#define	DEV_BLOCK_MAX_BYTES	\
	(DEV_BLOCK_SECTOR_BYTES * DEV_BLOCK_MAX_SECTORS)

/* Reply body for DEV_OP_INFO, after the header; a bare reply. */
/* WIRE FORMAT.  ABI-stable. */
struct dev_info_reply {
	char		dir_name[DEV_NAME_MAX];	/* NUL-terminated         */
	uint32_t	dir_kind;		/* DEV_KIND_*             */
	uint32_t	dir_flags;		/* DEV_F_*                */
};

_Static_assert(sizeof(struct dev_info_reply) == DEV_NAME_MAX + 8,
    "dev_info_reply must be 24 bytes (wire format)");

/*
 * Request for DEV_OP_WRITE: dwr_len valid bytes in dwr_data.  The reply's
 * dwr_written may be less on a short write.
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
 * Reply to DEV_OP_GEOM.  dgr_model is the device's model string,
 * NUL-padded; dgr_total_sectors counts dgr_sector_bytes units (a 1 GiB
 * disk: 0x200000 of 512).
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_geom_reply {
	int32_t		dgr_rv;
	uint32_t	dgr_sector_bytes;
	uint64_t	dgr_total_sectors;
	uint32_t	dgr_flags;	/* bit 0: LBA48 supported */
	uint32_t	dgr_pad;
	char		dgr_model[40];
};

_Static_assert(sizeof(struct dev_geom_reply) == 64,
    "dev_geom_reply must be 64 bytes (wire format)");

/*
 * Request for DEV_OP_READ_BLOCK: `dbr_count' sectors at LBA `dbr_lba'.
 * The caller's reply buffer must hold a header plus a
 * dev_block_read_reply.
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
 * Reply for DEV_OP_READ_BLOCK.  dbr_count is the request's count on
 * success, 0 on error.  dbr_data holds dbr_count * 512 bytes, padded to
 * DEV_BLOCK_MAX_BYTES so the reply size is fixed.
 */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_read_reply {
	int32_t		dbr_rv;
	uint32_t	dbr_count;
	uint8_t		dbr_data[DEV_BLOCK_MAX_BYTES];
};

_Static_assert(sizeof(struct dev_block_read_reply) == 8 + DEV_BLOCK_MAX_BYTES,
    "dev_block_read_reply must be 2056 bytes (wire format)");

/* Request for DEV_OP_WRITE_BLOCK: (lba, count) and the data inline. */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_write_req {
	uint64_t	dbw_lba;
	uint32_t	dbw_count;	/* sectors */
	uint32_t	dbw_pad;
	uint8_t		dbw_data[DEV_BLOCK_MAX_BYTES];
};

_Static_assert(sizeof(struct dev_block_write_req) == 16 + DEV_BLOCK_MAX_BYTES,
    "dev_block_write_req must be 2064 bytes (wire format)");

/* Reply for DEV_OP_WRITE_BLOCK and DEV_OP_SYNC. */
/* WIRE FORMAT.  ABI-stable. */
struct dev_block_io_reply {
	int32_t		dbr_rv;
	uint32_t	dbr_sectors;	/* sectors actually transferred */
};

_Static_assert(sizeof(struct dev_block_io_reply) == 8,
    "dev_block_io_reply must be 8 bytes (wire format)");

#endif /* !_SYS_DEV_PROTO_H_ */
