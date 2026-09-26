/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "intr.h"
#include "io.h"
#include "kprintf.h"
#include "mouse.h"
#include "pic.h"
#include "sched.h"
#include "thread.h"

/*
 * i8042 controller registers -- shared with the keyboard (see kbd.c).
 * 0x64 reads the status byte and writes a controller command; 0x60 is
 * the data port for both directions.
 */
#define	I8042_DATA		0x60
#define	I8042_STATUS		0x64
#define	I8042_CMD		0x64

#define	STS_OBF			0x01	/* output buffer full: a byte is ready */
#define	STS_IBF			0x02	/* input buffer full: do not write yet */
#define	STS_AUX			0x20	/* the ready byte came from the mouse  */

/* i8042 controller commands. */
#define	CTL_READ_CONFIG		0x20
#define	CTL_WRITE_CONFIG	0x60
#define	CTL_DISABLE_AUX		0xA7
#define	CTL_ENABLE_AUX		0xA8
#define	CTL_DISABLE_KBD		0xAD	/* hold the keyboard's clock           */
#define	CTL_ENABLE_KBD		0xAE	/* ... and let it go                   */
#define	CTL_WRITE_TO_AUX	0xD4	/* route the next 0x60 write to mouse  */

#define	CFG_IRQ12		0x02	/* config bit 1: raise IRQ12 on aux    */
#define	CFG_AUX_CLOCK_OFF	0x20	/* config bit 5: aux clock disabled    */

/* Mouse (aux) commands, sent through the CTL_WRITE_TO_AUX prefix. */
#define	AUX_GET_DEVICE_ID	0xF2
#define	AUX_ENABLE_STREAM	0xF4
#define	AUX_SET_DEFAULTS	0xF6
#define	AUX_ACK			0xFA
#define	AUX_RESEND		0xFE	/* "say that again"                    */

#define	AUX_ID_STANDARD		0x00	/* the 3-byte-packet mouse             */
#define	AUX_RETRIES		3	/* resend requests honoured per cmd    */

#define	MOUSE_IRQ		12
#define	MOUSE_CASCADE_IRQ	2	/* slave reaches the CPU via IRQ2      */

#define	I8042_SPIN		100000	/* bounded poll: never hang on no HW   */
#define	I8042_DRAIN_MAX		32	/* stale bytes discarded, at most      */

#define	MOUSE_PKT_BYTES		3
#define	PKT_BYTE0_ALWAYS1	0x08	/* byte 0 bit 3 is always set          */

/*
 * Ring of completed packets.  Single-producer (mouse_feed_byte: the IRQ12
 * handler, or the boot self-test borrowing its exclusion) / single-
 * consumer (mouse_getpkt, and so mouse_getpkt_block: the mouse-drv
 * thread).
 *
 * head and tail are free-running counters (unsigned wrap keeps head - tail
 * the fill level), each with one writer.  The producer fills a slot, then
 * releases head past it; the consumer reads a slot, then releases tail
 * past it; each acquires the other's index first.  That pairing, not
 * volatile, orders the packet bytes against the index that publishes them.
 */
#define	MOUSE_RING_SIZE		32
#define	MOUSE_RING_MASK		(MOUSE_RING_SIZE - 1)

_Static_assert(MOUSE_RING_SIZE > 0 &&
    (MOUSE_RING_SIZE & MOUSE_RING_MASK) == 0,
    "MOUSE_RING_SIZE must be a power of two");

struct mouse_packet {
	uint8_t	mp_byte[MOUSE_PKT_BYTES];
};

static uint32_t			mouse_ring_head;	/* producer's */
static uint32_t			mouse_ring_tail;	/* consumer's */
static struct mouse_packet	mouse_ring[MOUSE_RING_SIZE];

/*
 * In-progress packet assembly -- producer side only, which is to say
 * under the IRQ's exclusion: IRQ12 itself, or mouse_selftest_feed with
 * interrupts off on the one CPU IRQ12 is delivered to.
 */
static uint8_t			mouse_pkt[MOUSE_PKT_BYTES];
static uint8_t			mouse_phase;

/*
 * Blocking read for the single consumer, kbd.c's protocol (reasoning at
 * kbd_getc_block): the consumer installs itself in mouse_waiter by
 * exchange and rechecks the ring; the IRQ, having published a packet,
 * exchanges the slot empty and posts a deferred wake to whoever it found.
 * mouse_wake_pending lets a consumer taken between recheck and block skip
 * the block; a wake after that is kept as th_wake_pending by thread_wake.
 */
static struct thread *volatile	mouse_waiter;
static volatile int		mouse_wake_pending;

static int	aux_command(uint8_t cmd);
static int	i8042_command(uint8_t cmd);
static int	i8042_read_from(bool aux);
static int	i8042_wait_write(void);
static void	i8042_drain(void);
static void	mouse_feed_byte(uint8_t b);
static void	mouse_irq(struct trapframe *);
static int	mouse_probe(const char **stepp, int *gotp);
static void	mouse_ring_push(const uint8_t *pkt);

/*
 * Spin until the controller's input buffer is clear.  Returns 0, or -1
 * after I8042_SPIN polls, so a machine with no PS/2 controller loses the
 * mouse rather than hangs boot.
 */
static int
i8042_wait_write(void)
{
	int	i;

	for (i = 0; i < I8042_SPIN; i++)
		if ((inb(I8042_STATUS) & STS_IBF) == 0)
			return (0);
	return (-1);
}

/* Write one controller command byte; 0, or -1 if it would not take one. */
static int
i8042_command(uint8_t cmd)
{

	if (i8042_wait_write() != 0)
		return (-1);
	outb(I8042_CMD, cmd);
	return (0);
}

/*
 * Spin until a byte from the wanted side is readable and return it, or -1
 * on timeout.  The controller has one output buffer for keyboard, mouse
 * and its own replies; STS_AUX says whether a byte is the mouse's, and a
 * byte from the other side is dropped.  mouse_init holds the keyboard
 * port, so there should be none, but a reply taken from the wrong device
 * would derail the init silently.
 */
static int
i8042_read_from(bool aux)
{
	uint8_t	b;
	uint8_t	sts;
	int	i;

	for (i = 0; i < I8042_SPIN; i++) {
		sts = inb(I8042_STATUS);
		if ((sts & STS_OBF) == 0)
			continue;
		b = inb(I8042_DATA);
		if (((sts & STS_AUX) != 0) == aux)
			return (b);
	}
	return (-1);
}

/* Discard any stale bytes the controller has buffered before bring-up. */
static void
i8042_drain(void)
{
	int	i;

	for (i = 0; i < I8042_DRAIN_MAX; i++) {
		if ((inb(I8042_STATUS) & STS_OBF) == 0)
			return;
		(void)inb(I8042_DATA);
	}
}

/*
 * Send one command byte to the aux device and return its reply: AUX_ACK
 * normally, whatever else it said if it refused, -1 if nothing answered.
 * A resend request is honoured up to AUX_RETRIES times.  Polled, used only
 * from mouse_init before IRQ12 is unmasked, so it never races the IRQ
 * path.
 */
static int
aux_command(uint8_t cmd)
{
	int	i;
	int	reply;

	reply = -1;
	for (i = 0; i <= AUX_RETRIES; i++) {
		if (i8042_command(CTL_WRITE_TO_AUX) != 0 ||
		    i8042_wait_write() != 0)
			return (-1);
		outb(I8042_DATA, cmd);
		reply = i8042_read_from(true);
		if (reply != AUX_RESEND)
			break;
	}
	return (reply);
}

/*
 * The polled half of mouse_init, entered with interrupts off and the
 * keyboard port held.  Returns the device id, with the device streaming
 * and the controller raising IRQ12 for it; or -1, with *stepp naming the
 * step that failed and *gotp what came back instead (-1: nothing did).
 *
 * It stops at the first bad answer: later steps would build on a guess
 * about a controller the keyboard shares.
 */
static int
mouse_probe(const char **stepp, int *gotp)
{
	int	config;
	int	id;
	int	reply;

	reply = -1;
	*gotp = -1;

	*stepp = "enable aux port";
	if (i8042_command(CTL_ENABLE_AUX) != 0)
		goto fail;

	/*
	 * Read-modify-write the config byte: raise IRQ12 and enable the aux
	 * clock, keeping every other bit (notably bit 0, the keyboard's IRQ1
	 * enable).  Nothing is written unless the read succeeded, since a
	 * guessed byte would clobber the bits being kept.
	 */
	*stepp = "read config";
	if (i8042_command(CTL_READ_CONFIG) != 0)
		goto fail;
	config = i8042_read_from(false);
	if (config < 0)
		goto fail;
	config |= CFG_IRQ12;
	config &= ~CFG_AUX_CLOCK_OFF;

	*stepp = "write config";
	if (i8042_command(CTL_WRITE_CONFIG) != 0 || i8042_wait_write() != 0)
		goto fail;
	outb(I8042_DATA, (uint8_t)config);

	/*
	 * Probe the device two-way.  A standard PS/2 mouse powers on with
	 * reporting disabled, ACKs each command with 0xFA, and reports
	 * device id 0x00; get-id replies 0xFA and then the id byte.
	 */
	*stepp = "set defaults";
	reply = aux_command(AUX_SET_DEFAULTS);
	if (reply != AUX_ACK)
		goto fail;

	*stepp = "get device id";
	reply = aux_command(AUX_GET_DEVICE_ID);
	if (reply != AUX_ACK)
		goto fail;
	reply = i8042_read_from(true);
	if (reply < 0)
		goto fail;
	id = reply;

	/*
	 * Only id 0, the three-byte mouse, is read.  A wheel (3) or
	 * five-button (4) mouse already switched into that mode sends four
	 * bytes a packet, and set-defaults does not undo it (only a reset
	 * does); declined rather than misread.
	 */
	*stepp = "device id (not a 3-byte mouse)";
	if (id != AUX_ID_STANDARD)
		goto fail;

	*stepp = "enable streaming";
	reply = aux_command(AUX_ENABLE_STREAM);
	if (reply != AUX_ACK)
		goto fail;

	*stepp = NULL;
	return (id);

fail:
	*gotp = reply;
	return (-1);
}

void
mouse_init(void)
{
	const char	*step;
	int		 got;
	int		 id;
	bool		 kbd_back;
	bool		 kbd_held;
	bool		 was_on;

	/*
	 * Called in Phase 2 (after clock_init) with interrupts on; they are
	 * held off for the duration and restored as found.  The polled
	 * exchange must be atomic against kbd_irq, which reads the same data
	 * port, and IRQ12 must not arrive before pit_hz() is calibrated
	 * (intr_dispatch -> sched_check_timeouts -> clock_uptime_ms divides
	 * by it).
	 *
	 * The keyboard port is held too: disabling interrupts stops kbd_irq,
	 * not the keyboard, and a key pressed now would land in the output
	 * buffer ahead of the reply.  Held, the keyboard sends it later.
	 */
	was_on = intr_save_disable();
	kbd_held = (i8042_command(CTL_DISABLE_KBD) == 0);
	i8042_drain();

	id = mouse_probe(&step, &got);

	/*
	 * A failed probe takes the aux port back down: a half-configured
	 * device with IRQ12 masked could leave a byte in the shared output
	 * buffer that nobody takes, and every key would wait behind it.
	 */
	if (id < 0)
		(void)i8042_command(CTL_DISABLE_AUX);

	/*
	 * Enabling streaming can queue an initial packet at once; drain it
	 * while IRQ12 is still masked, so the ring starts empty.
	 */
	i8042_drain();

	if (id >= 0) {
		irq_install(MOUSE_IRQ, mouse_irq);
		pic_unmask(MOUSE_CASCADE_IRQ);
		pic_unmask(MOUSE_IRQ);
	}

	kbd_back = (i8042_command(CTL_ENABLE_KBD) == 0);
	intr_restore(was_on);

	/* Only a port this took and could not return is news. */
	if (kbd_held && !kbd_back)
		kprintf("mouse: controller would not take the keyboard "
		    "port back -- keyboard input is lost\n");
	if (id >= 0)
		kprintf("mouse: ready, device id 0x%02x\n", (unsigned)id);
	else if (got < 0)
		kprintf("mouse: %s: no answer -- aux port left off\n", step);
	else
		kprintf("mouse: %s: got 0x%02x -- aux port left off\n", step,
		    (unsigned)got);
}

int
mouse_getpkt(uint8_t *out)
{
	struct mouse_packet	*slot;
	uint32_t		 head;
	uint32_t		 tail;

	tail = __atomic_load_n(&mouse_ring_tail, __ATOMIC_RELAXED);
	head = __atomic_load_n(&mouse_ring_head, __ATOMIC_ACQUIRE);
	if (head == tail)
		return (-1);

	slot = &mouse_ring[tail & MOUSE_RING_MASK];
	out[0] = slot->mp_byte[0];
	out[1] = slot->mp_byte[1];
	out[2] = slot->mp_byte[2];
	/* Read out; only now is the slot the producer's again. */
	__atomic_store_n(&mouse_ring_tail, tail + 1, __ATOMIC_RELEASE);
	return (0);
}

/*
 * IRQ12 handler.  Drain every aux byte queued; a byte without STS_AUX is
 * the keyboard's and is left for kbd_irq.  intr_dispatch sends the EOI
 * (slave, then master); the wake is deferred via sched_post_irq_wake.
 */
static void
mouse_irq(struct trapframe *tf)
{
	uint8_t	sts;

	(void)tf;

	for (;;) {
		sts = inb(I8042_STATUS);
		if ((sts & STS_OBF) == 0)
			return;
		if ((sts & STS_AUX) == 0)
			return;
		mouse_feed_byte(inb(I8042_DATA));
	}
}

/*
 * Add one streaming byte to the current packet, pushing each complete
 * 3-byte packet onto the ring.  Shared by mouse_irq and the boot self-test.
 *
 * Resync: byte 0 always has bit 3 set, so at phase 0 a byte without it is
 * dropped.  Only a heuristic -- a delta byte can have bit 3 set too, so
 * after a lost byte the stream can stay misaligned until the check fails;
 * nothing stricter exists, since any value, 0xFA included, is a legal
 * delta.
 */
static void
mouse_feed_byte(uint8_t b)
{

	if (mouse_phase == 0 && (b & PKT_BYTE0_ALWAYS1) == 0)
		return;

	mouse_pkt[mouse_phase] = b;
	mouse_phase++;
	if (mouse_phase < MOUSE_PKT_BYTES)
		return;

	mouse_phase = 0;
	mouse_ring_push(mouse_pkt);
}

static void
mouse_ring_push(const uint8_t *pkt)
{
	struct mouse_packet	*slot;
	struct thread		*w;
	uint32_t		 head;
	uint32_t		 tail;

	head = __atomic_load_n(&mouse_ring_head, __ATOMIC_RELAXED);
	tail = __atomic_load_n(&mouse_ring_tail, __ATOMIC_ACQUIRE);
	if (head - tail >= MOUSE_RING_SIZE)
		return;				/* ring full, drop packet */

	slot = &mouse_ring[head & MOUSE_RING_MASK];
	slot->mp_byte[0] = pkt[0];
	slot->mp_byte[1] = pkt[1];
	slot->mp_byte[2] = pkt[2];
	/* Whole; only now is it the consumer's to see. */
	__atomic_store_n(&mouse_ring_head, head + 1, __ATOMIC_RELEASE);

	/*
	 * Take a parked consumer and defer its wake; mouse_wake_pending
	 * covers the recheck-to-block window.  The exchange's release half
	 * carries the head store above to a consumer whose exchange reads
	 * this one.
	 */
	w = __atomic_exchange_n(&mouse_waiter, NULL, __ATOMIC_ACQ_REL);
	if (w != NULL) {
		__atomic_store_n(&mouse_wake_pending, 1, __ATOMIC_RELEASE);
		sched_post_irq_wake(w);
	}
}

int
mouse_getpkt_block(uint8_t *out)
{
	struct thread	*self;

	self = current_thread;
	/* Noted for the length of the loop -- see kbd_getc_block. */
	thread_slot_note(self, &mouse_waiter);

	for (;;) {
		if (mouse_getpkt(out) == 0) {
			thread_slot_forget(self);
			return (0);
		}

		/*
		 * Empty: clear pending, install the waiter (by exchange),
		 * recheck -- the IRQ's order in reverse, as in kbd_getc_block,
		 * which says why.
		 */
		__atomic_store_n(&mouse_wake_pending, 0, __ATOMIC_RELAXED);
		(void)__atomic_exchange_n(&mouse_waiter, self,
		    __ATOMIC_ACQ_REL);

		if (mouse_getpkt(out) == 0) {
			__atomic_store_n(&mouse_waiter, NULL,
			    __ATOMIC_RELAXED);
			thread_slot_forget(self);
			return (0);
		}

		if (__atomic_load_n(&mouse_wake_pending,
		    __ATOMIC_ACQUIRE) != 0) {
			__atomic_store_n(&mouse_waiter, NULL,
			    __ATOMIC_RELAXED);
			continue;
		}

		thread_block(THREAD_BLOCK_SLEEP, (void *)&mouse_ring_head);
		/* Woken; loop and retry the ring. */
	}
}

/*
 * The self-test is a second producer into the IRQ's assembler, so it
 * borrows the IRQ's exclusion: interrupts off on this CPU, the one IRQ12
 * reaches (the 8259 delivers to the boot CPU only, and the self-test runs
 * there before the others are released).  It feeds only at a packet
 * boundary; three bytes added to a half-built live packet would complete
 * that one and leave two of its own behind.
 */
int
mouse_selftest_feed(uint8_t b0, uint8_t b1, uint8_t b2)
{
	int	rv;
	bool	was_on;

	was_on = intr_save_disable();
	rv = -1;
	if (mouse_phase == 0) {
		mouse_feed_byte(b0);
		mouse_feed_byte(b1);
		mouse_feed_byte(b2);
		rv = 0;
	}
	intr_restore(was_on);
	return (rv);
}
