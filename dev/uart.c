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
 * RX ring: single producer (COM1 IRQ), single consumer (uart_getc_block,
 * from the uart-drv thread), with the keyboard ring's discipline (kbd.c).
 */
#define	UART_BUF_SIZE		256
#define	UART_BUF_MASK		(UART_BUF_SIZE - 1)

_Static_assert(UART_BUF_SIZE > 0 && (UART_BUF_SIZE & UART_BUF_MASK) == 0,
    "UART_BUF_SIZE must be a power of two");

static uint16_t	uart_base = UART_COM1_BASE;

static uint32_t			uart_buf_head;	/* IRQ's      */
static uint32_t			uart_buf_tail;	/* consumer's */
static char			uart_buf[UART_BUF_SIZE];

static struct thread *volatile	uart_waiter;
static volatile int		uart_wake_pending;

static void	uart_send_raw(char);
static void	uart_irq(struct trapframe *);
static void	uart_buf_push(char);
static int	uart_buf_take(void);

void
uart_init(void)
{

	/* All UART interrupts masked until uart_enable_rx; output polls. */
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

	/* '\n' goes out as CR-LF, for terminal programs on the line. */
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

/* Take the next byte off the ring, or -1 if it is empty.  Consumer only. */
static int
uart_buf_take(void)
{
	uint32_t	head;
	uint32_t	tail;
	char		c;

	tail = __atomic_load_n(&uart_buf_tail, __ATOMIC_RELAXED);
	head = __atomic_load_n(&uart_buf_head, __ATOMIC_ACQUIRE);
	if (head == tail)
		return (-1);

	c = uart_buf[tail & UART_BUF_MASK];
	__atomic_store_n(&uart_buf_tail, tail + 1, __ATOMIC_RELEASE);
	return ((unsigned char)c);
}

int
uart_getc_block(void)
{
	struct thread	*self;
	int		 c;

	self = current_thread;
	/* Noted for the length of the loop -- see kbd_getc_block. */
	thread_slot_note(self, &uart_waiter);

	for (;;) {
		c = uart_buf_take();
		if (c >= 0) {
			thread_slot_forget(self);
			return (c);
		}

		/* An exchange, not a store -- kbd_getc_block says why. */
		__atomic_store_n(&uart_wake_pending, 0, __ATOMIC_RELAXED);
		(void)__atomic_exchange_n(&uart_waiter, self,
		    __ATOMIC_ACQ_REL);

		c = uart_buf_take();
		if (c >= 0) {
			__atomic_store_n(&uart_waiter, NULL,
			    __ATOMIC_RELAXED);
			thread_slot_forget(self);
			return (c);
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
 * COM1 IRQ handler.  Drains every byte in the receiver (one interrupt can
 * cover several) into the ring; the push wakes the consumer.
 */
static void
uart_irq(struct trapframe *tf)
{
	uint8_t	b;

	(void)tf;

	while ((inb(COM_LSR(uart_base)) & LSR_DATA_READY) != 0) {
		b = inb(COM_DR(uart_base));
		/* CR, a terminal's Enter, becomes the '\n' readers expect. */
		if (b == '\r')
			b = '\n';
		uart_buf_push((char)b);
	}
}

static void
uart_buf_push(char ch)
{
	struct thread	*w;
	uint32_t	 head;
	uint32_t	 tail;

	head = __atomic_load_n(&uart_buf_head, __ATOMIC_RELAXED);
	tail = __atomic_load_n(&uart_buf_tail, __ATOMIC_ACQUIRE);
	if (head - tail >= UART_BUF_SIZE)
		return;

	uart_buf[head & UART_BUF_MASK] = ch;
	__atomic_store_n(&uart_buf_head, head + 1, __ATOMIC_RELEASE);

	w = __atomic_exchange_n(&uart_waiter, NULL, __ATOMIC_ACQ_REL);
	if (w != NULL) {
		__atomic_store_n(&uart_wake_pending, 1, __ATOMIC_RELEASE);
		sched_post_irq_wake(w);
	}
}
