/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "io.h"
#include "uart.h"

/*
 * 16550 register layout (offsets from base, with DLAB=0 unless noted).
 *	+0	DR	data register (TX/RX)
 *		DLL	divisor latch low	(DLAB=1)
 *	+1	IER	interrupt enable
 *		DLH	divisor latch high	(DLAB=1)
 *	+2	IIR/FCR	interrupt id (R) / FIFO control (W)
 *	+3	LCR	line control (parity, stop bits, word len, DLAB)
 *	+4	MCR	modem control (DTR/RTS/OUT1/OUT2/loopback)
 *	+5	LSR	line status   (RX-ready, THR-empty, errors)
 *	+6	MSR	modem status
 *	+7	SR	scratch
 */
#define	COM_DR(b)		((b) + 0)
#define	COM_IER(b)		((b) + 1)
#define	COM_FCR(b)		((b) + 2)
#define	COM_LCR(b)		((b) + 3)
#define	COM_MCR(b)		((b) + 4)
#define	COM_LSR(b)		((b) + 5)

#define	LCR_DLAB		0x80
#define	LCR_8N1			0x03	/* 8 data, no parity, 1 stop */

#define	FCR_ENABLE_RESET	0xC7	/* enable FIFO, clear TX/RX, 14B trig */

#define	MCR_DTR			0x01
#define	MCR_RTS			0x02
#define	MCR_OUT2		0x08	/* OUT2 gates IRQ delivery; harmless */

#define	LSR_THR_EMPTY		0x20

static uint16_t	uart_base = UART_COM1_BASE;

static void	uart_send_raw(char);

void
uart_init(void)
{

	/* Mask all UART interrupts; we only do polled output for now. */
	outb(COM_IER(uart_base), 0x00);

	/* Programme baud rate: enable DLAB, set divisor=1 (115200 baud). */
	outb(COM_LCR(uart_base), LCR_DLAB);
	outb(COM_DR(uart_base),  0x01);		/* DLL = 1 */
	outb(COM_IER(uart_base), 0x00);		/* DLH = 0 */

	/* Lock in 8N1, clear DLAB so subsequent writes hit DR/IER. */
	outb(COM_LCR(uart_base), LCR_8N1);

	/* Enable + reset FIFOs. */
	outb(COM_FCR(uart_base), FCR_ENABLE_RESET);

	/* Drive DTR/RTS so the host considers us ready; enable OUT2. */
	outb(COM_MCR(uart_base), MCR_DTR | MCR_RTS | MCR_OUT2);
}

void
uart_putc(char ch)
{

	/*
	 * Translate '\n' to CR-LF on the wire so any terminal program
	 * pointed at the serial line displays cleanly; the kernel proper
	 * still emits a single '\n' at the API boundary.
	 */
	if (ch == '\n')
		uart_send_raw('\r');
	uart_send_raw(ch);
}

void
uart_puts(const char *s)
{

	while (*s != '\0')
		uart_putc(*s++);
}

void
uart_write(const char *s, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		uart_putc(s[i]);
}

static void
uart_send_raw(char ch)
{

	while ((inb(COM_LSR(uart_base)) & LSR_THR_EMPTY) == 0)
		;
	outb(COM_DR(uart_base), (uint8_t)ch);
}
