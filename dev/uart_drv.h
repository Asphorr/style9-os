/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_UART_DRV_H_
#define	_SYS_UART_DRV_H_

#include "port.h"

/*
 * Serial-console RX driver.  Mirrors dev/kbd_drv: at boot,
 * uart_drv_init() allocates `uart_input_port` in kernel_space with
 * RECEIVE | SEND, enables the COM1 receive IRQ, and spawns a
 * `uart-drv` kernel thread that bridges uart_getc_block to
 * mach_msg_send.  Each received byte arrives at uart_input_port as a
 * 24-byte header with the character in msgh_id, identical wire format
 * to the keyboard's events.
 *
 * Shell can recv from kbd_input_port + uart_input_port via a port set
 * so either source drives the same input stream.
 */

extern mach_port_name_t	uart_input_port;

void	uart_drv_init(void);

#endif /* !_SYS_UART_DRV_H_ */
