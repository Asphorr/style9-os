/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "kbd.h"
#include "kbd_drv.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"

mach_port_name_t	kbd_input_port;

static void	kbd_drv_thread(void *) __attribute__((noreturn));

void
kbd_drv_init(void)
{
	struct thread	*th;

	kbd_input_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (kbd_input_port == MACH_PORT_NULL)
		panic("kbd_drv_init: port_allocate failed");

	th = thread_create(kernel_task, kbd_drv_thread, NULL, "kbd-drv");
	if (th == NULL)
		panic("kbd_drv_init: thread_create failed");
	thread_start(th);

	kprintf("kbd_drv: port=%u thread=%llu\n",
	    (unsigned)kbd_input_port, (unsigned long long)th->th_id);
}

/*
 * Bridge between the IRQ-fed scancode ring and Mach IPC.  Park in
 * kbd_getc_block until a keypress arrives, then push it onto the input
 * port as a tagged message.  msgh_id carries the byte; the receiver
 * does not need a complex body because the payload is one character.
 *
 * Never returns: a kernel driver thread is part of the kernel for the
 * lifetime of the system.
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

		msg.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0);
		msg.msgh_size    = sizeof(msg);
		msg.msgh_remote  = kbd_input_port;
		msg.msgh_local   = MACH_PORT_NULL;
		msg.msgh_voucher = 0;
		msg.msgh_id      = (uint32_t)(unsigned char)c;

		rv = mach_msg_send(kernel_space, &msg);
		/*
		 * Send failure here means either the queue is full
		 * past its blocking threshold (impossible, we use a
		 * blocking send) or the port has died (the shell
		 * deallocated it -- not expected in normal operation).
		 * Drop the byte and keep going so a transient receiver
		 * outage does not bring the driver down.
		 */
		if (rv != MACH_MSG_OK) {
			kprintf("kbd_drv: send rv=%s, dropping byte 0x%02x\n",
			    mach_msg_strerror(rv), (unsigned)c);
		}
	}
}
