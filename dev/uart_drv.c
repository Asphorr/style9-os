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
#include "panic.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"
#include "uart.h"
#include "uart_drv.h"

extern struct port	*port_create_kernel_owned(uint8_t kind, void *arg);

mach_port_name_t	uart_input_port;

static int	uart_drv_dispatch(const struct mach_msg_header *req,
		    struct port_space *from);
static int	uart_drv_handle_write(const struct mach_msg_header *req,
		    struct port_space *from);
static void	uart_drv_thread(void *) __attribute__((noreturn));

void
uart_drv_init(void)
{
	struct port	*ctl;
	struct thread	*th;
	int		 rv;

	uart_input_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (uart_input_port == MACH_PORT_NULL)
		panic("uart_drv_init: port_allocate failed");

	uart_enable_rx();

	/*
	 * Two-direction driver: the RX side mirrors kbd_drv (a stream
	 * port the consumer recvs on), the TX side accepts DEV_OP_WRITE
	 * messages and pushes the bytes out through uart_putc.  Both ride
	 * the same control port; the kind reports STREAM_RX because RX
	 * is the canonical use, with DEV_F_WRITABLE indicating WRITE is
	 * also valid.
	 */
	ctl = port_create_kernel_owned(PORT_SPECIAL_SERVICE,
	    (void *)(uintptr_t)uart_drv_dispatch);
	if (ctl == NULL)
		panic("uart_drv_init: control port creation failed");
	rv = dev_register("uart", ctl);
	if (rv != MACH_MSG_OK)
		panic("uart_drv_init: dev_register failed (rv=%d)", rv);

	th = thread_create(kernel_task, uart_drv_thread, NULL, "uart-drv");
	if (th == NULL)
		panic("uart_drv_init: thread_create failed");
	thread_start(th);

	kprintf("uart_drv: stream_port=%u thread=%llu\n",
	    (unsigned)uart_input_port, (unsigned long long)th->th_id);
}

static int
uart_drv_dispatch(const struct mach_msg_header *req, struct port_space *from)
{

	switch (req->msgh_id) {
	case DEV_OP_INFO:
		return (dev_reply_info(req, from,
		    "uart", DEV_KIND_STREAM_RX,
		    DEV_F_READABLE | DEV_F_WRITABLE | DEV_F_STREAM));
	case DEV_OP_OPEN_STREAM:
		return (dev_reply_stream(req, from, uart_input_port));
	case DEV_OP_WRITE:
		return (uart_drv_handle_write(req, from));
	default:
		return (MACH_E_INVAL);
	}
}

static int
uart_drv_handle_write(const struct mach_msg_header *req,
    struct port_space *from)
{
	uint8_t				 buf[sizeof(struct mach_msg_header) +
					     sizeof(struct dev_write_reply)];
	const struct dev_write_request	*wr;
	struct mach_msg_header		*rhdr;
	struct dev_write_reply		*body;
	const uint8_t			*payload;
	uint32_t			 wlen;
	uint32_t			 i;

	if (req->msgh_size < sizeof(struct mach_msg_header) +
	    sizeof(struct dev_write_request))
		return (MACH_E_INVAL);

	payload = (const uint8_t *)req + sizeof(struct mach_msg_header);
	wr      = (const struct dev_write_request *)payload;

	wlen = wr->dwr_len;
	if (wlen > DEV_WRITE_MAX)
		wlen = DEV_WRITE_MAX;

	for (i = 0; i < wlen; i++)
		uart_putc((char)wr->dwr_data[i]);

	rhdr = (struct mach_msg_header *)buf;
	body = (struct dev_write_reply *)(buf + sizeof(struct mach_msg_header));
	body->dwr_rv      = MACH_MSG_OK;
	body->dwr_written = wlen;

	rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	rhdr->msgh_size    = sizeof(buf);
	rhdr->msgh_remote  = req->msgh_local;
	rhdr->msgh_local   = MACH_PORT_NULL;
	rhdr->msgh_voucher = 0;
	rhdr->msgh_id      = req->msgh_id;
	return (mach_msg_send(from, rhdr));
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
