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
#include "kbd.h"
#include "pic.h"
#include "sched.h"
#include "thread.h"

#define	KBD_DATA_PORT	0x60
#define	KBD_STATUS_PORT	0x64
#define	KBD_STS_OBF	0x01	/* output buffer full -- data byte ready */
#define	KBD_IRQ		1

#define	SC_RELEASE	0x80	/* bit set on key-release scancodes */
#define	SC_LSHIFT	0x2A
#define	SC_RSHIFT	0x36

#define	KBD_BUF_SIZE	128	/* must be power of two */
#define	KBD_BUF_MASK	(KBD_BUF_SIZE - 1)

/*
 * Scancode-set-1 -> ASCII translation, US layout.  Indexed by the low
 * 7 bits of the scancode (the press code); release codes are press|0x80
 * and stripped before indexing.  Zero means "no character" -- modifier
 * keys, function keys, and reserved positions all map to 0 and are
 * filtered at the producer.
 */
static const char	sc_table[128] = {
	0,    27,  '1', '2', '3', '4', '5', '6',	/* 00 - 07 */
	'7',  '8', '9', '0', '-', '=', '\b','\t',	/* 08 - 0F */
	'q',  'w', 'e', 'r', 't', 'y', 'u', 'i',	/* 10 - 17 */
	'o',  'p', '[', ']', '\n',  0, 'a', 's',	/* 18 - 1F  (1D = LCtrl) */
	'd',  'f', 'g', 'h', 'j', 'k', 'l', ';',	/* 20 - 27 */
	'\'', '`',  0, '\\','z', 'x', 'c', 'v',		/* 28 - 2F  (2A = LShift)*/
	'b',  'n', 'm', ',', '.', '/',  0,  '*',	/* 30 - 37  (36 = RShift)*/
	0,    ' ',  0,   0,   0,   0,   0,   0,		/* 38 - 3F */
	0,    0,    0,   0,   0,   0,   0,   0,		/* 40 - 47 */
	0,    0,    0,   0,   0,   0,   0,   0,		/* 48 - 4F */
	0,    0,    0,   0,   0,   0,   0,   0,		/* 50 - 57 */
	0,    0,    0,   0,   0,   0,   0,   0,		/* 58 - 5F */
};

static const char	sc_table_shift[128] = {
	0,    27,  '!', '@', '#', '$', '%', '^',
	'&',  '*', '(', ')', '_', '+', '\b','\t',
	'Q',  'W', 'E', 'R', 'T', 'Y', 'U', 'I',
	'O',  'P', '{', '}', '\n',  0, 'A', 'S',
	'D',  'F', 'G', 'H', 'J', 'K', 'L', ':',
	'"',  '~',  0, '|', 'Z', 'X', 'C', 'V',
	'B',  'N', 'M', '<', '>', '?',  0,  '*',
	0,    ' ',  0,   0,   0,   0,   0,   0,
	0,    0,    0,   0,   0,   0,   0,   0,
	0,    0,    0,   0,   0,   0,   0,   0,
	0,    0,    0,   0,   0,   0,   0,   0,
	0,    0,    0,   0,   0,   0,   0,   0,
};

/*
 * Single-producer (IRQ1 handler) / single-consumer (kbd_getc, called
 * from kmain's idle loop) ring buffer.  No lock needed: writes to head
 * happen only with interrupts disabled (the IRQ handler runs with IF=0
 * thanks to the interrupt gate), and reads of head from kbd_getc are
 * 32-bit aligned and naturally atomic on i386.
 */
static volatile uint32_t	kbd_buf_head;
static volatile uint32_t	kbd_buf_tail;
static char			kbd_buf[KBD_BUF_SIZE];

static volatile uint8_t		kbd_shift;	/* either Shift key down */

/*
 * Single-consumer block-on-read support.
 *
 *	kbd_waiter	The one thread currently parked in kbd_getc_block,
 *			or NULL.  Set by the consumer immediately before it
 *			rechecks the ring and blocks; cleared by the IRQ
 *			when it pushes a byte and harvests the waiter.  The
 *			exchange is atomic so a producer push that races
 *			with the consumer's set always observes one side
 *			or the other -- never both.
 *
 *	kbd_wake_pending Set by the IRQ when it consumed a waiter slot.
 *			The consumer checks this flag after registering and
 *			rechecking the ring; if set, it skips the block and
 *			loops back instead, so a wake delivered between the
 *			recheck and the block is not lost.
 */
static struct thread *volatile	kbd_waiter;
static volatile int		kbd_wake_pending;

static void	kbd_irq(struct trapframe *);
static void	kbd_buf_push(char);
static int	kbd_decode_scancode(uint8_t sc);

void
kbd_init(void)
{

	irq_install(KBD_IRQ, kbd_irq);
	pic_unmask(KBD_IRQ);
}

int
kbd_getc(void)
{
	char	c;

	if (kbd_buf_head == kbd_buf_tail)
		return (-1);

	c = kbd_buf[kbd_buf_tail & KBD_BUF_MASK];
	kbd_buf_tail++;
	return ((unsigned char)c);
}

static void
kbd_irq(struct trapframe *tf)
{
	int	c;

	(void)tf;

	c = kbd_decode_scancode(inb(KBD_DATA_PORT));
	if (c >= 0)
		kbd_buf_push((char)c);
}

int
kbd_poll_getc(void)
{

	if ((inb(KBD_STATUS_PORT) & KBD_STS_OBF) == 0)
		return (-1);

	return (kbd_decode_scancode(inb(KBD_DATA_PORT)));
}

int
kbd_poll_getc_block(void)
{
	int	c;

	for (;;) {
		c = kbd_poll_getc();
		if (c >= 0)
			return (c);
		__asm__ __volatile__ ("pause");
	}
}

/*
 * Decode one raw scancode byte.  Updates shared shift state, returns
 * the translated character on a printable keypress, or -1 on release
 * events, modifier presses, and keys that do not map to a character.
 *
 * Used by both the IRQ handler and the polled-input path so the two
 * agree on shift state and the translation table.
 */
static int
kbd_decode_scancode(uint8_t sc)
{
	uint8_t	pressed;
	char	ch;

	if ((sc & SC_RELEASE) != 0) {
		pressed = (uint8_t)(sc & 0x7F);
		if (pressed == SC_LSHIFT || pressed == SC_RSHIFT)
			kbd_shift = 0;
		return (-1);
	}

	if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
		kbd_shift = 1;
		return (-1);
	}

	ch = (kbd_shift != 0) ? sc_table_shift[sc] : sc_table[sc];
	if (ch == 0)
		return (-1);
	return ((unsigned char)ch);
}

static void
kbd_buf_push(char ch)
{
	struct thread	*w;
	uint32_t	 next;

	next = kbd_buf_head + 1;
	if (next - kbd_buf_tail > KBD_BUF_SIZE)
		return;				/* buffer full, drop */

	kbd_buf[kbd_buf_head & KBD_BUF_MASK] = ch;
	kbd_buf_head = next;

	/*
	 * Harvest any parked consumer and signal it.  The exchange
	 * makes the take-the-slot operation atomic against a consumer
	 * that's installing itself concurrently; sched_post_irq_wake
	 * defers the actual thread_wake to the next safe point (so
	 * we don't take sched_lock from IRQ context).  The
	 * wake_pending flag covers the narrow race where the
	 * consumer is between recheck-and-block and would otherwise
	 * miss the signal.
	 */
	w = __atomic_exchange_n(&kbd_waiter, NULL, __ATOMIC_ACQUIRE);
	if (w != NULL) {
		__atomic_store_n(&kbd_wake_pending, 1, __ATOMIC_RELEASE);
		sched_post_irq_wake(w);
	}
}

int
kbd_getc_block(void)
{
	struct thread	*self;
	int		 c;

	self = current_thread;

	for (;;) {
		c = kbd_getc();
		if (c >= 0)
			return (c);

		/*
		 * Empty: register, clear the pending flag, then recheck
		 * the ring under the now-installed waiter slot.  Order
		 * matters -- the IRQ-side store sequence is push, then
		 * exchange waiter, then set pending; we mirror it in
		 * reverse: clear pending, install waiter, recheck.
		 */
		__atomic_store_n(&kbd_wake_pending, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&kbd_waiter, self, __ATOMIC_RELEASE);

		c = kbd_getc();
		if (c >= 0) {
			__atomic_store_n(&kbd_waiter, NULL,
			    __ATOMIC_RELAXED);
			return (c);
		}

		/*
		 * A wake delivered between the recheck above and the
		 * block below would set wake_pending; if so, skip the
		 * block and loop.  Otherwise it is safe to park -- a
		 * later IRQ will sched_post_irq_wake us once the buffer
		 * has data.
		 */
		if (__atomic_load_n(&kbd_wake_pending,
		    __ATOMIC_ACQUIRE) != 0) {
			__atomic_store_n(&kbd_waiter, NULL,
			    __ATOMIC_RELAXED);
			continue;
		}

		thread_block(THREAD_BLOCK_SLEEP, (void *)&kbd_buf_head);
		/* Woken; loop and retry the ring. */
	}
}
