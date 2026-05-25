/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"
#include "uart.h"
#include "uart_drv.h"

mach_port_name_t	uart_input_port;

static void	uart_drv_thread(void *) __attribute__((noreturn));

void
uart_drv_init(void)
{
	struct thread	*th;

	uart_input_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (uart_input_port == MACH_PORT_NULL)
		panic("uart_drv_init: port_allocate failed");

	uart_enable_rx();

	th = thread_create(kernel_task, uart_drv_thread, NULL, "uart-drv");
	if (th == NULL)
		panic("uart_drv_init: thread_create failed");
	thread_start(th);

	kprintf("uart_drv: port=%u thread=%llu\n",
	    (unsigned)uart_input_port, (unsigned long long)th->th_id);
}

static void
uart_drv_thread(void *arg)
{
	struct mach_msg_header	msg;
	int			c;
	int			rv;

	(void)arg;

	for (;;) {
		c = uart_getc_block();
		if (c < 0)
			continue;

		msg.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0);
		msg.msgh_size    = sizeof(msg);
		msg.msgh_remote  = uart_input_port;
		msg.msgh_local   = MACH_PORT_NULL;
		msg.msgh_voucher = 0;
		msg.msgh_id      = (uint32_t)(unsigned char)c;

		rv = mach_msg_send(kernel_space, &msg);
		if (rv != MACH_MSG_OK) {
			kprintf("uart_drv: send rv=%s, dropping byte 0x%02x\n",
			    mach_msg_strerror(rv), (unsigned)c);
		}
	}
}
