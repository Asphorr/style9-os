/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "intr.h"
#include "io.h"
#include "pic.h"
#include "sched.h"
#include "thread.h"
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

#define	LSR_DATA_READY		0x01
#define	LSR_THR_EMPTY		0x20

#define	IER_ERBFI		0x01	/* enable received-data-available IRQ */

#define	COM1_IRQ		4

/*
 * RX ring buffer.  Single-producer (COM1 IRQ) / single-consumer
 * (uart_getc_block, called by the uart_drv thread).  Same locking
 * discipline as the keyboard ring in dev/kbd.c: head/tail are 32-bit
 * naturally atomic on amd64, writes from the IRQ run with IF=0.
 */
#define	UART_BUF_SIZE		256
#define	UART_BUF_MASK		(UART_BUF_SIZE - 1)

static uint16_t	uart_base = UART_COM1_BASE;

static volatile uint32_t	uart_buf_head;
static volatile uint32_t	uart_buf_tail;
static char			uart_buf[UART_BUF_SIZE];

static struct thread *volatile	uart_waiter;
static volatile int		uart_wake_pending;

static void	uart_send_raw(char);
static void	uart_irq(struct trapframe *);
static void	uart_buf_push(char);

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

void
uart_enable_rx(void)
{

	irq_install(COM1_IRQ, uart_irq);
	pic_unmask(COM1_IRQ);
	outb(COM_IER(uart_base), IER_ERBFI);
}

int
uart_getc_block(void)
{
	struct thread	*self;
	char		 c;

	self = current_thread;

	for (;;) {
		if (uart_buf_head != uart_buf_tail) {
			c = uart_buf[uart_buf_tail & UART_BUF_MASK];
			uart_buf_tail++;
			return ((unsigned char)c);
		}

		__atomic_store_n(&uart_wake_pending, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&uart_waiter, self, __ATOMIC_RELEASE);

		if (uart_buf_head != uart_buf_tail) {
			__atomic_store_n(&uart_waiter, NULL,
			    __ATOMIC_RELAXED);
			c = uart_buf[uart_buf_tail & UART_BUF_MASK];
			uart_buf_tail++;
			return ((unsigned char)c);
		}

		if (__atomic_load_n(&uart_wake_pending,
		    __ATOMIC_ACQUIRE) != 0) {
			__atomic_store_n(&uart_waiter, NULL,
			    __ATOMIC_RELAXED);
			continue;
		}

		thread_block(THREAD_BLOCK_SLEEP, (void *)&uart_buf_head);
	}
}

/*
 * COM1 IRQ handler.  Drains every byte currently in the receiver
 * (the 16550 fires one IRQ per FIFO-trigger-threshold but the line
 * may have several bytes queued), pushes into the ring, then hands
 * the parked consumer over via sched_post_irq_wake.
 */
static void
uart_irq(struct trapframe *tf)
{
	uint8_t	b;

	(void)tf;

	while ((inb(COM_LSR(uart_base)) & LSR_DATA_READY) != 0) {
		b = inb(COM_DR(uart_base));
		/*
		 * Convert CR (the terminal "Enter" byte on most TTY
		 * line disciplines) to LF so the shell's line-mode
		 * '\n' check matches without us replicating cooked-tty
		 * logic here.
		 */
		if (b == '\r')
			b = '\n';
		uart_buf_push((char)b);
	}
}

static void
uart_buf_push(char ch)
{
	struct thread	*w;
	uint32_t	 next;

	next = uart_buf_head + 1;
	if (next - uart_buf_tail > UART_BUF_SIZE)
		return;

	uart_buf[uart_buf_head & UART_BUF_MASK] = ch;
	uart_buf_head = next;

	w = __atomic_exchange_n(&uart_waiter, NULL, __ATOMIC_ACQUIRE);
	if (w != NULL) {
		__atomic_store_n(&uart_wake_pending, 1, __ATOMIC_RELEASE);
		sched_post_irq_wake(w);
	}
}
