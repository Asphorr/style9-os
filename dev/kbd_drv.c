/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "dev_subsystem.h"
#include "kbd.h"
#include "kbd_drv.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"

extern struct port	*port_create_kernel_owned(uint8_t kind, void *arg);

mach_port_name_t	kbd_input_port;

/*
 * The optional second consumer (kbd_drv.h).  Written once at boot, before
 * any Darwin task exists, and read by the driver thread; no lock.
 */
static kbd_sink_fn	kbd_sink;

static int	kbd_drv_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);
static void	kbd_drv_thread(void *) __attribute__((noreturn));

void
kbd_drv_init(void)
{
	struct port	*ctl;
	struct thread	*th;
	int		 rv;

	kbd_input_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (kbd_input_port == MACH_PORT_NULL)
		panic("kbd_drv_init: port_allocate failed");

	/*
	 * Control port: INFO and OPEN_STREAM.  OPEN_STREAM moves the
	 * RECEIVE right for kbd_input_port to the caller (sh.elf); the
	 * legacy kern/shell.c receives on it in kernel_space instead.
	 */
	ctl = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)kbd_drv_dispatch);
	if (ctl == NULL)
		panic("kbd_drv_init: control port creation failed");
	rv = dev_register("kbd", ctl);
	if (rv != MACH_MSG_OK)
		panic("kbd_drv_init: dev_register failed (rv=%d)", rv);

	th = thread_create(kernel_task, kbd_drv_thread, NULL, "kbd-drv");
	if (th == NULL)
		panic("kbd_drv_init: thread_create failed");
	thread_start(th);

	kprintf("kbd_drv: stream_port=%u thread=%llu\n",
	    (unsigned)kbd_input_port, (unsigned long long)th->th_id);
}

void
kbd_drv_set_sink(kbd_sink_fn fn)
{

	kbd_sink = fn;
}

/*
 * dev/kbd control-port dispatcher.  Runs synchronously in the sender's
 * thread, so the reply lands straight in its reply buffer.
 */
static int
kbd_drv_dispatch(const struct mach_msg_header *req, struct port_space *from)
{

	switch (req->msgh_id) {
	case DEV_OP_INFO:
		return (dev_reply_info(req, from,
		    "kbd", DEV_KIND_STREAM_RX,
		    DEV_F_READABLE | DEV_F_STREAM));
	case DEV_OP_OPEN_STREAM:
		return (dev_reply_stream(req, from, kbd_input_port));
	default:
		return (MACH_E_INVAL);
	}
}

/*
 * Bridge from the IRQ-fed ring to Mach IPC: park in kbd_getc_block, then
 * send each character to the input port as a bare header with the byte in
 * msgh_id.  Never returns.
 */
static void
kbd_drv_thread(void *arg)
{
	struct mach_msg_header	msg;
	int			c;
	int			rv;

	(void)arg;

	for (;;) {
		c = kbd_getc_block();
		if (c < 0)
			continue;

		/*
		 * Offer the key to the sink first.  A key it takes must not
		 * also go to the port: the second copy would surface later,
		 * out of context, at whatever prompt is up by then.
		 */
		if (kbd_sink != NULL && kbd_sink((char)c))
			continue;

		msg.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0);
		msg.msgh_size    = sizeof(msg);
		msg.msgh_remote  = kbd_input_port;
		msg.msgh_local   = MACH_PORT_NULL;
		msg.msgh_voucher = 0;
		msg.msgh_id      = (uint32_t)(unsigned char)c;

		rv = mach_msg_send(kernel_space, &msg);
		/*
		 * The send blocks on a full queue, so a failure means the
		 * port died (its receiver went away).  Drop the byte and
		 * keep going.
		 */
		if (rv != MACH_MSG_OK) {
			kprintf("kbd_drv: send rv=%s, dropping byte 0x%02x\n",
			    mach_msg_strerror(rv), (unsigned)c);
		}
	}
}
