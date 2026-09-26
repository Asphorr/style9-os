/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _DEV_UART_H_
#define	_DEV_UART_H_

#include <stddef.h>

/*
 * 16550 UART (8250/16450/16550A family): polled output, IRQ-driven
 * receive.
 *
 * COM1, at I/O port 0x3F8, is what QEMU exposes by default (-serial
 * file:PATH captures it).  Unlike dbgcon (port 0xE9) it exists on real
 * machines too.  '\n' is sent as CR-LF; the rest of the kernel speaks
 * plain '\n'.
 */

#define	UART_COM1_BASE	0x3F8

void	uart_init(void);
void	uart_putc(char);
void	uart_puts(const char *);
void	uart_write(const char *, size_t);

/*
 * Enable IRQ-driven receive on COM1 (irq_install + pic_unmask).  Call
 * only once the IDT is up and interrupts are enabled.
 */
void	uart_enable_rx(void);

/*
 * Blocking read, as kbd_getc_block: parks until the COM1 IRQ pushes a
 * byte.  Single consumer only.
 */
int	uart_getc_block(void);

#endif /* !_DEV_UART_H_ */
