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
 * Wire shapes are pinned by _Static_assert so kernel-side dispatchers
 * and future ring-3 consumers agree on the layout.  All replies COPY_SEND
 * back through req->msgh_local; the inline-reply fast path applies to
 * bare replies (INFO, WRITE) and skips the kmalloc / enqueue / wake of
 * the queue path entirely.
 */

#define	DEV_OP_INFO		1	/* request: header.  reply: dev_info_reply  */
#define	DEV_OP_OPEN_STREAM	2	/* request: header.  reply: complex w/ port_desc */
#define	DEV_OP_WRITE		3	/* request: dev_write_request.  reply: dev_write_reply */

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
 * Reply for DEV_OP_INFO.  Sits right after the mach_msg_header in the
 * reply message.  24 bytes; the fast path treats this as a bare reply.
 */
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
struct dev_write_request {
	uint32_t	dwr_len;
	uint32_t	dwr_pad;
	uint8_t		dwr_data[DEV_WRITE_MAX];
};

_Static_assert(sizeof(struct dev_write_request) == 8 + DEV_WRITE_MAX,
    "dev_write_request must be 264 bytes (wire format)");

struct dev_write_reply {
	int32_t		dwr_rv;		/* MACH_MSG_OK or MACH_E_*     */
	uint32_t	dwr_written;	/* bytes actually consumed     */
};

_Static_assert(sizeof(struct dev_write_reply) == 8,
    "dev_write_reply must be 8 bytes (wire format)");

#endif /* !_SYS_DEV_PROTO_H_ */
