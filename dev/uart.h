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
 * 16550 UART (PC-style 8250/16450/16550A line of chips), driven in
 * polled output mode.
 *
 * COM1 is at I/O port 0x3F8 in the legacy PC layout and is the chip
 * QEMU exposes by default; -serial file:PATH on the host side will
 * receive every byte written through uart_putc here.  Unlike dbgcon
 * (port 0xE9), this is real hardware and works on physical machines
 * as well as in emulation.
 *
 * Newlines are converted to CR-LF at the driver layer so logs read
 * correctly on any terminal program; the rest of the kernel still
 * speaks pure '\n'.
 */

#define	UART_COM1_BASE	0x3F8

void	uart_init(void);
void	uart_putc(char);
void	uart_puts(const char *);
void	uart_write(const char *, size_t);

#endif /* !_DEV_UART_H_ */
