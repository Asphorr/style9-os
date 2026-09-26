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
#define	KBD_STS_AUX	0x20	/* ... and it is the mouse's (mouse.c)   */
#define	KBD_IRQ		1

#define	SC_RELEASE	0x80	/* bit set on key-release scancodes */
#define	SC_LSHIFT	0x2A
#define	SC_RSHIFT	0x36
#define	SC_LCTRL	0x1D	/* RCtrl is 0xE0 0x1D, handled in extended */

#define	KBD_BUF_SIZE	128
#define	KBD_BUF_MASK	(KBD_BUF_SIZE - 1)

_Static_assert(KBD_BUF_SIZE > 0 && (KBD_BUF_SIZE & KBD_BUF_MASK) == 0,
    "KBD_BUF_SIZE must be a power of two");

/*
 * Scancode set 1 -> ASCII, US layout, indexed by press code (releases,
 * press|0x80, never reach it).  Zero means no character: modifiers,
 * function keys and unused codes, dropped by kbd_decode_scancode.
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
 * Single-producer (IRQ1) / single-consumer (kbd_getc, from the kbd-drv
 * thread) ring.  No lock: head and tail are free-running counters with one
 * writer each.  The IRQ stores a byte, then releases head past it; the
 * consumer takes a byte, then releases tail past it; each acquires the
 * other's index first.  That pairing orders each byte against the index
 * that publishes it, with the consumer possibly on another CPU.
 */
static uint32_t			kbd_buf_head;	/* IRQ's      */
static uint32_t			kbd_buf_tail;	/* consumer's */
static char			kbd_buf[KBD_BUF_SIZE];

static volatile uint8_t		kbd_shift;	/* either Shift key down */
static volatile uint8_t		kbd_ctrl;	/* either Ctrl key down  */

/*
 * Extended-scancode latch.  Set 1 prefixes the cursor and page keys (and
 * a few others) with 0xE0 to tell them from their numpad twins.  The IRQ
 * consumes the 0xE0, sets this, and treats the next byte as an extended
 * press or release; a press becomes its ANSI/VT100 escape sequence, pushed
 * byte by byte, as a serial terminal would send it.
 */
static volatile uint8_t		kbd_extended;

/*
 * Blocking read for the single consumer.
 *
 *	kbd_waiter	The thread parked in kbd_getc_block, or NULL.  Set
 *			by the consumer just before it rechecks the ring and
 *			blocks; taken by the IRQ when it pushes a byte.  Both
 *			sides exchange it, so a push racing the install
 *			either finds the waiter or is seen by the recheck.
 *
 *	kbd_wake_pending Set by the IRQ when it took a waiter.  Checked by
 *			the consumer after its recheck: if set, it loops
 *			instead of blocking, so a wake between recheck and
 *			block is not lost.
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
	uint32_t	head;
	uint32_t	tail;
	char		c;

	tail = __atomic_load_n(&kbd_buf_tail, __ATOMIC_RELAXED);
	head = __atomic_load_n(&kbd_buf_head, __ATOMIC_ACQUIRE);
	if (head == tail)
		return (-1);

	c = kbd_buf[tail & KBD_BUF_MASK];
	__atomic_store_n(&kbd_buf_tail, tail + 1, __ATOMIC_RELEASE);
	return ((unsigned char)c);
}

/*
 * Translate an extended (post-0xE0) press to its ANSI/VT100 escape
 * sequence and push it into the ring.  Releases and unmapped codes are
 * dropped.
 */
static void
kbd_emit_extended(uint8_t sc)
{
	const char	*p;
	const char	*seq;

	/*
	 * RCtrl is 0xE0 0x1D: it drives the same kbd_ctrl flag as LCtrl.
	 * Checked before the release filter, since its release (0x9D) is
	 * what clears the flag.
	 */
	if (sc == SC_LCTRL) {
		kbd_ctrl = 1;
		return;
	}
	if (sc == (SC_LCTRL | SC_RELEASE)) {
		kbd_ctrl = 0;
		return;
	}

	if ((sc & 0x80) != 0)
		return;	/* release -- ignored */

	seq = NULL;
	switch (sc) {
	case 0x48: seq = "\x1b[A";  break;	/* Up arrow             */
	case 0x50: seq = "\x1b[B";  break;	/* Down arrow           */
	case 0x4D: seq = "\x1b[C";  break;	/* Right arrow          */
	case 0x4B: seq = "\x1b[D";  break;	/* Left arrow           */
	case 0x49: seq = "\x1b[5~"; break;	/* Page Up              */
	case 0x51: seq = "\x1b[6~"; break;	/* Page Down            */
	case 0x47: seq = "\x1b[H";  break;	/* Home                 */
	case 0x4F: seq = "\x1b[F";  break;	/* End                  */
	case 0x53: seq = "\x1b[3~"; break;	/* Delete               */
	case 0x52: seq = "\x1b[2~"; break;	/* Insert               */
	}
	if (seq == NULL)
		return;
	for (p = seq; *p != '\0'; p++)
		kbd_buf_push(*p);
}

static void
kbd_irq(struct trapframe *tf)
{
	uint8_t	sc;
	uint8_t	sts;
	int	c;

	(void)tf;

	/*
	 * Only a byte that is there, and is the keyboard's.  IRQ1 is latched
	 * at the 8259, but the byte can be gone by the time this runs
	 * (mouse_init drains the controller with interrupts off), and reading
	 * an empty data port returns the last byte again.  The output buffer
	 * is shared with the mouse; a byte with STS_AUX set is mouse_irq's.
	 */
	sts = inb(KBD_STATUS_PORT);
	if ((sts & KBD_STS_OBF) == 0 || (sts & KBD_STS_AUX) != 0)
		return;
	sc = inb(KBD_DATA_PORT);

	if (sc == 0xE0) {
		kbd_extended = 1;
		return;
	}
	if (kbd_extended) {
		kbd_extended = 0;
		kbd_emit_extended(sc);
		return;
	}

	c = kbd_decode_scancode(sc);
	if (c >= 0)
		kbd_buf_push((char)c);
}

int
kbd_poll_getc(void)
{
	uint8_t	sts;

	sts = inb(KBD_STATUS_PORT);
	if ((sts & KBD_STS_OBF) == 0)
		return (-1);

	/*
	 * A mouse byte is read and dropped.  It cannot be left: this polls
	 * with interrupts off (ddb), so mouse_irq will not take it, and it
	 * would block the shared output buffer for every later key.
	 */
	if ((sts & KBD_STS_AUX) != 0) {
		(void)inb(KBD_DATA_PORT);
		return (-1);
	}

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
 * Decode one raw scancode byte.  Updates the shared modifier state and
 * returns the character for a printable press, or -1 for releases,
 * modifiers, and unmapped keys.  Shared by the IRQ and polled paths so
 * they agree on modifier state.
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
		else if (pressed == SC_LCTRL)
			kbd_ctrl = 0;
		return (-1);
	}

	if (sc == SC_LSHIFT || sc == SC_RSHIFT) {
		kbd_shift = 1;
		return (-1);
	}
	if (sc == SC_LCTRL) {
		kbd_ctrl = 1;
		return (-1);
	}

	ch = (kbd_shift != 0) ? sc_table_shift[sc] : sc_table[sc];
	if (ch == 0)
		return (-1);

	/*
	 * Ctrl folds letters to control codes 0x01..0x1A (Ctrl-C = 0x03,
	 * Ctrl-D = 0x04), the VT100 convention; other keys pass unchanged.
	 */
	if (kbd_ctrl != 0) {
		if (ch >= 'a' && ch <= 'z')
			ch = (char)(ch - 'a' + 1);
		else if (ch >= 'A' && ch <= 'Z')
			ch = (char)(ch - 'A' + 1);
	}
	return ((unsigned char)ch);
}

static void
kbd_buf_push(char ch)
{
	struct thread	*w;
	uint32_t	 head;
	uint32_t	 tail;

	head = __atomic_load_n(&kbd_buf_head, __ATOMIC_RELAXED);
	tail = __atomic_load_n(&kbd_buf_tail, __ATOMIC_ACQUIRE);
	if (head - tail >= KBD_BUF_SIZE)
		return;				/* buffer full, drop */

	kbd_buf[head & KBD_BUF_MASK] = ch;
	__atomic_store_n(&kbd_buf_head, head + 1, __ATOMIC_RELEASE);

	/*
	 * Take any parked consumer and wake it.  sched_post_irq_wake defers
	 * the thread_wake to a safe point, keeping sched_lock out of IRQ
	 * context; kbd_wake_pending covers a consumer between recheck and
	 * block.  The exchange releases as well as acquires: that carries
	 * the push above to a consumer whose own exchange reads this one
	 * (see kbd_getc_block).
	 */
	w = __atomic_exchange_n(&kbd_waiter, NULL, __ATOMIC_ACQ_REL);
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
	/*
	 * Noted for the length of the loop: kbd_waiter may name us anywhere
	 * in it, and an exit mid-park would leave that name for the next
	 * keystroke's ISR to wake.  The only caller, the kernel_task kbd-drv
	 * thread, cannot be killed; the note makes that not matter
	 * (thread_exit CASes the name out; th_wait_slot, kern/thread.h).
	 */
	thread_slot_note(self, &kbd_waiter);

	for (;;) {
		c = kbd_getc();
		if (c >= 0) {
			thread_slot_forget(self);
			return (c);
		}

		/*
		 * Empty: clear the pending flag, install ourselves, then
		 * recheck the ring -- the IRQ's order (push, take waiter,
		 * set pending) in reverse.
		 *
		 * The install is an exchange, not a store, and the recheck
		 * depends on it.  x86 lets a load pass an earlier store to
		 * a different address, so the recheck could read head while
		 * our name is still in the store buffer; an IRQ on another
		 * CPU could push a byte and find the slot empty in that
		 * window, and we would park over a ring with data in it.
		 * Two exchanges on one slot are totally ordered and the
		 * second sees the first: the IRQ finds our name, or ours
		 * reads the IRQ's and acquires the push before it.
		 */
		__atomic_store_n(&kbd_wake_pending, 0, __ATOMIC_RELAXED);
		(void)__atomic_exchange_n(&kbd_waiter, self, __ATOMIC_ACQ_REL);

		c = kbd_getc();
		if (c >= 0) {
			__atomic_store_n(&kbd_waiter, NULL,
			    __ATOMIC_RELAXED);
			thread_slot_forget(self);
			return (c);
		}

		/*
		 * A wake between the recheck and the block sets
		 * kbd_wake_pending: loop instead of parking.  Otherwise the
		 * next push will wake us.
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
