/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

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

mach_port_name_t	mouse_input_port;

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
 * Pack a raw 3-byte PS/2 packet into the msgh_id wire layout documented
 * in mouse_drv.h.  The overflow flags (byte 0 bits 6,7) are dropped: a
 * delta that overflowed int8 is clamped by the byte cast, the
 * conventional handling for a simple mover.
 */
static uint32_t
mouse_pack(const uint8_t *pkt)
{
	uint32_t	id;

	id = MOUSE_MSG_EVENT;
	id |= (uint32_t)(pkt[0] & 0x07) << 16;
	id |= (uint32_t)pkt[2] << 8;		/* dy */
	id |= (uint32_t)pkt[1];			/* dx */
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
 * Inject one synthetic packet (Left button, dx=+5, dy=-3) through the
 * same assembly path the IRQ uses, then recv the resulting event off
 * mouse_input_port and check the decode.  Bounded by a timeout so a
 * regression can never wedge the boot; loud on mismatch.
 */
static void
mouse_drv_selftest(void)
{
	struct mach_msg_header	msg;
	unsigned		btn;
	int			dx;
	int			dy;
	int			rv;

	/* Discard any event already queued (e.g. live motion at boot). */
	while (mach_msg_recv_timed(kernel_space, mouse_input_port, &msg,
	    sizeof(msg), MACH_TIMEOUT_NONE) == MACH_MSG_OK)
		continue;

	mouse_selftest_feed(0x09, 0x05, 0xFD);	/* L + dx=+5 + dy=-3 */

	rv = mach_msg_recv_timed(kernel_space, mouse_input_port, &msg,
	    sizeof(msg), 2000);
	if (rv != MACH_MSG_OK) {
		kprintf("mouse_drv: SELF-TEST recv rv=%s (path NOT proven)\n",
		    mach_msg_strerror(rv));
		return;
	}

	btn = (unsigned)((msg.msgh_id >> 16) & 0x07);
	dy = (int)(int8_t)(uint8_t)(msg.msgh_id >> 8);
	dx = (int)(int8_t)(uint8_t)msg.msgh_id;

	if ((msg.msgh_id & MOUSE_MSG_EVENT) == 0 ||
	    btn != MOUSE_MSG_BTN_LEFT || dx != 5 || dy != -3) {
		kprintf("mouse_drv: SELF-TEST MISMATCH id=0x%08x btn=0x%x "
		    "dx=%d dy=%d\n", (unsigned)msg.msgh_id, btn, dx, dy);
		return;
	}

	kprintf("mouse_drv: self-test event btn=L dx=%d dy=%d "
	    "(feed->ring->thread->port OK)\n", dx, dy);
}
