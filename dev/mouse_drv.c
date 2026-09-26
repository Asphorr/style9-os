/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dev_subsystem.h"
#include "kprintf.h"
#include "mouse.h"
#include "mouse_drv.h"
#include "panic.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"

extern struct port	*port_create_kernel_owned(uint8_t kind, void *arg);

/* Byte 0 of a PS/2 movement packet (full layout in mouse.h). */
#define	PKT_BTN_MASK	0x07	/* bits 0..2: L, R, M               */
#define	PKT_XSIGN	0x10	/* bit 4: X delta is negative       */
#define	PKT_YSIGN	0x20	/* bit 5: Y delta is negative       */
#define	PKT_XOVF	0x40	/* bit 6: X counter overflowed      */
#define	PKT_YOVF	0x80	/* bit 7: Y counter overflowed      */

/*
 * Boot self-test vectors: a raw packet and the event it must come out as.
 * The first is the ordinary case; the rest are the ones a byte-as-int8
 * decode gets wrong.
 */
struct mouse_selftest {
	const char	*mst_what;
	uint8_t		 mst_pkt[3];
	uint8_t		 mst_btn;
	int8_t		 mst_dx;
	int8_t		 mst_dy;
};

static const struct mouse_selftest	mouse_selftests[] = {
	{ "L +5,-3",		{ 0x29, 0x05, 0xFD },
	    MOUSE_MSG_BTN_LEFT,		   5,	-3 },
	{ "+200 clamps",	{ 0x08, 0xC8, 0x00 },
	    0,				 127,	 0 },
	{ "-200,-1",		{ 0x38, 0x38, 0xFF },
	    0,				-128,	-1 },
	{ "R, sign on 0",	{ 0x1A, 0x00, 0x00 },
	    MOUSE_MSG_BTN_RIGHT,	   0,	 0 },
	{ "M, X overflow",	{ 0x5C, 0x12, 0x00 },
	    MOUSE_MSG_BTN_MIDDLE,	-128,	 0 },
};

#define	MOUSE_NSELFTESTS	\
	(sizeof(mouse_selftests) / sizeof(mouse_selftests[0]))

mach_port_name_t	mouse_input_port;

static int	mouse_axis(uint8_t lo, bool negative, bool overflow);
static int	mouse_drv_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);
static uint32_t	mouse_pack(const uint8_t *pkt);
static void	mouse_drv_selftest(void);
static void	mouse_drv_thread(void *) __attribute__((noreturn));

void
mouse_drv_init(void)
{
	struct port	*ctl;
	struct thread	*th;
	int		 rv;

	mouse_input_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (mouse_input_port == MACH_PORT_NULL)
		panic("mouse_drv_init: port_allocate failed");

	/*
	 * Control port: handles the dev-NAME protocol (INFO + OPEN_STREAM).
	 * OPEN_STREAM hands back a SEND right naming mouse_input_port -- the
	 * port the mouse-drv thread feeds decoded events into.
	 */
	ctl = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)mouse_drv_dispatch);
	if (ctl == NULL)
		panic("mouse_drv_init: control port creation failed");
	rv = dev_register("mouse", ctl);
	if (rv != MACH_MSG_OK)
		panic("mouse_drv_init: dev_register failed (rv=%d)", rv);

	th = thread_create(kernel_task, mouse_drv_thread, NULL, "mouse-drv");
	if (th == NULL)
		panic("mouse_drv_init: thread_create failed");
	thread_start(th);

	kprintf("mouse_drv: stream_port=%u thread=%llu\n",
	    (unsigned)mouse_input_port, (unsigned long long)th->th_id);

	mouse_drv_selftest();
}

/*
 * dev/mouse control-port dispatcher.  Synchronous: runs in the caller's
 * thread the moment they send to dev/mouse, so the inline-reply fast path
 * lands the answer straight into their reply buffer.
 */
static int
mouse_drv_dispatch(const struct mach_msg_header *req, struct port_space *from)
{

	switch (req->msgh_id) {
	case DEV_OP_INFO:
		return (dev_reply_info(req, from,
		    "mouse", DEV_KIND_STREAM_RX,
		    DEV_F_READABLE | DEV_F_STREAM));
	case DEV_OP_OPEN_STREAM:
		return (dev_reply_stream(req, from, mouse_input_port));
	default:
		return (MACH_E_INVAL);
	}
}

/*
 * One axis of a PS/2 packet, as the int8 the wire layout has room for.
 *
 * The device counts in nine bits: the low eight in the data byte, the
 * sign in byte 0.  The data byte on its own is NOT the delta -- a quick
 * flick right of +200 arrives as 0xC8 with the sign clear, and read as an
 * int8 that is -56, the pointer thrown the wrong way.  So the nine-bit
 * value is rebuilt first and clamped after.
 *
 * Two edges, each decided rather than left to the arithmetic:
 *
 *	sign set on a zero byte	-256 by the letter, but devices set the
 *				sign on no motion at all; read as none
 *				(Linux psmouse does the same).
 *	overflow bit		the device's counter ran past nine bits
 *				before it could report; the low byte is
 *				then meaningless but the sign is not, so
 *				a full-scale step that way.
 */
static int
mouse_axis(uint8_t lo, bool negative, bool overflow)
{
	int	v;

	if (overflow)
		return (negative ? INT8_MIN : INT8_MAX);
	if (lo == 0)
		return (0);
	v = (int)lo - (negative ? 256 : 0);
	if (v > INT8_MAX)
		return (INT8_MAX);
	if (v < INT8_MIN)
		return (INT8_MIN);
	return (v);
}

/*
 * Pack a raw 3-byte PS/2 packet into the msgh_id wire layout documented
 * in mouse_drv.h.
 */
static uint32_t
mouse_pack(const uint8_t *pkt)
{
	uint32_t	id;
	int		dx;
	int		dy;

	dx = mouse_axis(pkt[1], (pkt[0] & PKT_XSIGN) != 0,
	    (pkt[0] & PKT_XOVF) != 0);
	dy = mouse_axis(pkt[2], (pkt[0] & PKT_YSIGN) != 0,
	    (pkt[0] & PKT_YOVF) != 0);

	id = MOUSE_MSG_EVENT;
	id |= (uint32_t)(pkt[0] & PKT_BTN_MASK) << 16;
	id |= (uint32_t)(uint8_t)dy << 8;
	id |= (uint32_t)(uint8_t)dx;
	return (id);
}

/*
 * Bridge between the IRQ-fed packet ring and Mach IPC.  Park in
 * mouse_getpkt_block until a packet arrives, then ship the decoded event
 * to the input port as a tagged message.  msgh_id carries the whole
 * event, so no complex body is needed.
 *
 * Never returns: a kernel driver thread is part of the kernel for the
 * lifetime of the system.
 */
static void
mouse_drv_thread(void *arg)
{
	struct mach_msg_header	msg;
	uint8_t			pkt[3];
	int			rv;

	(void)arg;

	for (;;) {
		if (mouse_getpkt_block(pkt) != 0)
			continue;

		msg.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0);
		msg.msgh_size    = sizeof(msg);
		msg.msgh_remote  = mouse_input_port;
		msg.msgh_local   = MACH_PORT_NULL;
		msg.msgh_voucher = 0;
		msg.msgh_id      = mouse_pack(pkt);

		rv = mach_msg_send(kernel_space, &msg);
		/*
		 * A send failure means the port died (a consumer deallocated
		 * it -- not expected here, the kernel holds RECEIVE).  Drop
		 * the event and keep going so a transient outage does not
		 * bring the driver down.
		 */
		if (rv != MACH_MSG_OK) {
			kprintf("mouse_drv: send rv=%s, dropping event\n",
			    mach_msg_strerror(rv));
		}
	}
}

/*
 * Boot self-test: prove feed -> ring -> driver-thread -> Mach-message end
 * to end, deterministically, without needing physical mouse motion.
 * Inject each synthetic packet in mouse_selftests through the same
 * assembly path the IRQ uses, then recv the resulting event off
 * mouse_input_port and check the decode.  Bounded by a timeout so a
 * regression can never wedge the boot; loud on mismatch.
 *
 * Live motion during the test can still land an event of its own between
 * the drain and a recv and read as a mismatch.  The test says so rather
 * than guessing: it is a boot diagnostic, not an invariant.
 */
static void
mouse_drv_selftest(void)
{
	const struct mouse_selftest	*t;
	struct mach_msg_header		 msg;
	size_t				 i;
	unsigned			 btn;
	int				 dx;
	int				 dy;
	int				 rv;

	/* Discard any event already queued (e.g. live motion at boot). */
	while (mach_msg_recv_timed(kernel_space, mouse_input_port, &msg,
	    sizeof(msg), MACH_TIMEOUT_NONE) == MACH_MSG_OK)
		continue;

	for (i = 0; i < MOUSE_NSELFTESTS; i++) {
		t = &mouse_selftests[i];
		if (mouse_selftest_feed(t->mst_pkt[0], t->mst_pkt[1],
		    t->mst_pkt[2]) != 0) {
			kprintf("mouse_drv: SELF-TEST skipped -- a live "
			    "packet was half-assembled (path NOT proven)\n");
			return;
		}

		rv = mach_msg_recv_timed(kernel_space, mouse_input_port,
		    &msg, sizeof(msg), 2000);
		if (rv != MACH_MSG_OK) {
			kprintf("mouse_drv: SELF-TEST \"%s\" recv rv=%s "
			    "(path NOT proven)\n", t->mst_what,
			    mach_msg_strerror(rv));
			return;
		}

		btn = (unsigned)((msg.msgh_id >> 16) & PKT_BTN_MASK);
		dy = (int)(int8_t)(uint8_t)(msg.msgh_id >> 8);
		dx = (int)(int8_t)(uint8_t)msg.msgh_id;

		if ((msg.msgh_id & MOUSE_MSG_EVENT) == 0 ||
		    btn != t->mst_btn || dx != t->mst_dx ||
		    dy != t->mst_dy) {
			kprintf("mouse_drv: SELF-TEST MISMATCH \"%s\" "
			    "id=0x%08x btn=0x%x dx=%d dy=%d, want btn=0x%x "
			    "dx=%d dy=%d (live motion also reads as this)\n",
			    t->mst_what, (unsigned)msg.msgh_id, btn, dx, dy,
			    (unsigned)t->mst_btn, t->mst_dx, t->mst_dy);
			return;
		}
	}

	kprintf("mouse_drv: self-test %u events decoded, sign and clamp "
	    "included (feed->ring->thread->port OK)\n",
	    (unsigned)MOUSE_NSELFTESTS);
}
