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
 * Serial-console driver, after dev/kbd_drv: uart_drv_init() allocates
 * `uart_input_port' in kernel_space with RECEIVE | SEND, enables the COM1
 * receive IRQ, and starts the `uart-drv' thread, which sends each byte to
 * the port in the keyboard's format (the character in msgh_id).  The
 * control port also takes DEV_OP_WRITE for output.
 *
 * kern/shell.c receives from both input ports through one port set.
 */

extern mach_port_name_t	uart_input_port;

void	uart_drv_init(void);

#endif /* !_SYS_UART_DRV_H_ */
