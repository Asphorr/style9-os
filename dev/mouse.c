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
#define	CTL_ENABLE_AUX		0xA8
#define	CTL_WRITE_TO_AUX	0xD4	/* route the next 0x60 write to mouse  */

#define	CFG_IRQ12		0x02	/* config bit 1: raise IRQ12 on aux    */
#define	CFG_AUX_CLOCK_OFF	0x20	/* config bit 5: aux clock disabled    */

/* Mouse (aux) commands, sent through the CTL_WRITE_TO_AUX prefix. */
#define	AUX_GET_DEVICE_ID	0xF2
#define	AUX_ENABLE_STREAM	0xF4
#define	AUX_SET_DEFAULTS	0xF6
#define	AUX_ACK			0xFA

#define	MOUSE_IRQ		12
#define	MOUSE_CASCADE_IRQ	2	/* slave reaches the CPU via IRQ2      */

#define	I8042_SPIN		100000	/* bounded poll: never hang on no HW   */

#define	MOUSE_PKT_BYTES		3
#define	PKT_BYTE0_ALWAYS1	0x08	/* byte 0 bit 3 is always set          */
#define	PKT_BTN_MASK		0x07	/* byte 0 bits 0..2: L, R, M           */

/*
 * Ring of completed packets.  Single-producer (mouse_feed_byte, from the
 * IRQ12 handler or the boot self-test) / single-consumer (the mouse-drv
 * thread in mouse_getpkt_block).  Same lock-free discipline as kbd.c: the
 * producer runs with IF=0 (interrupt gate) and head/tail are 32-bit
 * aligned, so no lock is needed.
 */
#define	MOUSE_RING_SIZE		32	/* must be a power of two */
#define	MOUSE_RING_MASK		(MOUSE_RING_SIZE - 1)

struct mouse_packet {
	uint8_t	mp_byte[MOUSE_PKT_BYTES];
};

static volatile uint32_t	mouse_ring_head;
static volatile uint32_t	mouse_ring_tail;
static struct mouse_packet	mouse_ring[MOUSE_RING_SIZE];

/* In-progress packet assembly -- producer side only. */
static uint8_t			mouse_pkt[MOUSE_PKT_BYTES];
static uint8_t			mouse_phase;

/*
 * Single-consumer block-on-read support, mirroring kbd.c byte for byte:
 * the IRQ pushes a packet, atomically harvests any parked consumer, and
 * defers the wake via sched_post_irq_wake; wake_pending closes the race
 * where a wake lands between the consumer's recheck and its block.
 */
static struct thread *volatile	mouse_waiter;
static volatile int		mouse_wake_pending;

static int	aux_command(uint8_t cmd);
static int	i8042_wait_read(void);
static int	i8042_wait_write(void);
static void	i8042_drain(void);
static void	mouse_feed_byte(uint8_t b);
static void	mouse_irq(struct trapframe *);
static void	mouse_ring_push(const uint8_t *pkt);

/*
 * Spin until the controller's input buffer is clear (safe to write) or
 * the bounded budget elapses.  Returns 0 if writable, -1 on timeout --
 * the caller logs and presses on rather than hanging boot on a machine
 * with no PS/2 controller.
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

/* Spin until a byte is readable from the controller; 0 / -1 as above. */
static int
i8042_wait_read(void)
{
	int	i;

	for (i = 0; i < I8042_SPIN; i++)
		if ((inb(I8042_STATUS) & STS_OBF) != 0)
			return (0);
	return (-1);
}

/* Discard any stale bytes the controller has buffered before bring-up. */
static void
i8042_drain(void)
{
	int	i;

	for (i = 0; i < MOUSE_RING_SIZE; i++) {
		if ((inb(I8042_STATUS) & STS_OBF) == 0)
			return;
		(void)inb(I8042_DATA);
	}
}

/*
 * Send one command byte to the aux device and return the byte it replies
 * with (normally AUX_ACK), or -1 if the controller or device never
 * answered.  Polled, used only from mouse_init before IRQ12 is unmasked,
 * so it never races the IRQ path.
 */
static int
aux_command(uint8_t cmd)
{

	if (i8042_wait_write() != 0)
		return (-1);
	outb(I8042_CMD, CTL_WRITE_TO_AUX);
	if (i8042_wait_write() != 0)
		return (-1);
	outb(I8042_DATA, cmd);
	if (i8042_wait_read() != 0)
		return (-1);
	return ((int)inb(I8042_DATA));
}

void
mouse_init(void)
{
	int	ack_defaults;
	int	ack_stream;
	int	id;
	uint8_t	config;

	/*
	 * Brought up in Phase 2 (after clock_init), interrupts already on.
	 * Mask them for the duration: the polled i8042 exchange must be
	 * atomic against kbd_irq, which drains the same 0x60 data port, and
	 * IRQ12 must not be delivered until pit_hz() is calibrated (the
	 * intr_dispatch -> sched_check_timeouts -> clock_uptime_ms path
	 * divides by it).  Re-enabled at the end.
	 */
	intr_disable();

	i8042_drain();

	if (i8042_wait_write() == 0)
		outb(I8042_CMD, CTL_ENABLE_AUX);

	/*
	 * Read-modify-write the controller config byte: turn on "raise
	 * IRQ12" and re-enable the aux clock, preserving every other bit --
	 * notably bit 0, the keyboard's own IRQ1 enable that kbd_init
	 * relies on.
	 */
	config = 0;
	if (i8042_wait_write() == 0) {
		outb(I8042_CMD, CTL_READ_CONFIG);
		if (i8042_wait_read() == 0)
			config = inb(I8042_DATA);
	}
	config |= CFG_IRQ12;
	config &= (uint8_t)~CFG_AUX_CLOCK_OFF;
	if (i8042_wait_write() == 0) {
		outb(I8042_CMD, CTL_WRITE_CONFIG);
		if (i8042_wait_write() == 0)
			outb(I8042_DATA, config);
	}

	/*
	 * Probe the device two-way.  A standard PS/2 mouse powers on with
	 * reporting disabled, ACKs each command with 0xFA, and reports
	 * device id 0x00.  get-id replies 0xFA then the id byte, so read one
	 * extra byte for it.  Whatever comes back is logged rather than
	 * asserted -- the kernel must boot on a box with no mouse.
	 */
	ack_defaults = aux_command(AUX_SET_DEFAULTS);
	id = aux_command(AUX_GET_DEVICE_ID);
	if (id == AUX_ACK)
		id = (i8042_wait_read() == 0) ? (int)inb(I8042_DATA) : -1;
	ack_stream = aux_command(AUX_ENABLE_STREAM);

	/*
	 * Enabling streaming can make the device immediately queue an
	 * initial data packet; drain it (still IRQ12-masked) so the line is
	 * quiescent when we unmask and the consumer's ring starts empty.
	 * Real motion arms IRQ12 from here on.
	 */
	i8042_drain();

	irq_install(MOUSE_IRQ, mouse_irq);
	pic_unmask(MOUSE_CASCADE_IRQ);
	pic_unmask(MOUSE_IRQ);

	intr_enable();

	kprintf("mouse: aux up cfg=0x%02x defaults=0x%02x id=0x%02x "
	    "stream=0x%02x\n", (unsigned)config, (unsigned)(ack_defaults & 0xFF),
	    (unsigned)(id & 0xFF), (unsigned)(ack_stream & 0xFF));
}

int
mouse_getpkt(uint8_t *out)
{
	struct mouse_packet	*slot;

	if (mouse_ring_head == mouse_ring_tail)
		return (-1);

	slot = &mouse_ring[mouse_ring_tail & MOUSE_RING_MASK];
	out[0] = slot->mp_byte[0];
	out[1] = slot->mp_byte[1];
	out[2] = slot->mp_byte[2];
	mouse_ring_tail++;
	return (0);
}

/*
 * IRQ12 handler.  Drain every aux byte the controller has queued; the
 * status port's AUX bit tells a mouse byte from a keyboard byte on the
 * shared i8042, so a byte without it is left in place for kbd_irq.  No
 * EOI here -- intr_dispatch issues it (slave then master) after we
 * return; no scheduler call either -- the deferred wake is posted via
 * sched_post_irq_wake and drained at a safe preempt point.
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
 * Assemble one streaming data byte into the current packet, pushing a
 * completed 3-byte packet onto the ring.  Shared by mouse_irq and the
 * boot self-test, mirroring how kbd.c shares kbd_decode_scancode between
 * its IRQ and polled paths.
 *
 * Resync: byte 0 of a PS/2 packet always has bit 3 set.  At phase 0 a
 * byte lacking it means the stream is misaligned -- drop it and keep
 * waiting for a real byte 0.
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
	uint32_t		 next;

	next = mouse_ring_head + 1;
	if (next - mouse_ring_tail > MOUSE_RING_SIZE)
		return;				/* ring full, drop packet */

	slot = &mouse_ring[mouse_ring_head & MOUSE_RING_MASK];
	slot->mp_byte[0] = pkt[0];
	slot->mp_byte[1] = pkt[1];
	slot->mp_byte[2] = pkt[2];
	mouse_ring_head = next;

	/*
	 * Harvest a parked consumer atomically against one installing
	 * itself concurrently, then defer the wake (sched_post_irq_wake
	 * must not take sched_lock from IRQ context); wake_pending covers
	 * the recheck-and-block race.  Identical to kbd_buf_push.
	 */
	w = __atomic_exchange_n(&mouse_waiter, NULL, __ATOMIC_ACQUIRE);
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

	for (;;) {
		if (mouse_getpkt(out) == 0)
			return (0);

		/*
		 * Empty: clear pending, install the waiter, recheck.  Mirror
		 * of the IRQ-side store order (push, exchange waiter, set
		 * pending) run in reverse, the way kbd_getc_block does it.
		 */
		__atomic_store_n(&mouse_wake_pending, 0, __ATOMIC_RELAXED);
		__atomic_store_n(&mouse_waiter, self, __ATOMIC_RELEASE);

		if (mouse_getpkt(out) == 0) {
			__atomic_store_n(&mouse_waiter, NULL,
			    __ATOMIC_RELAXED);
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

void
mouse_selftest_feed(uint8_t b0, uint8_t b1, uint8_t b2)
{

	mouse_feed_byte(b0);
	mouse_feed_byte(b1);
	mouse_feed_byte(b2);
}
