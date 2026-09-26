/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _DEV_MOUSE_H_
#define	_DEV_MOUSE_H_

#include <stdint.h>

/*
 * PS/2 mouse -- the auxiliary device on the very same i8042 controller the
 * keyboard hangs off (kbd.c), wired to IRQ12 instead of IRQ1.
 *
 * mouse_init brings the aux port up by polling.  It must run in Phase 2,
 * after clock_init: it holds interrupts off for the polled exchange and
 * the keyboard port at the controller (so no keystroke lands in front of
 * a reply), enables the aux port, flips the controller config to raise
 * IRQ12, probes the device two-way (set-defaults / get-id / enable-
 * streaming, each ACKed with 0xFA), then installs the IRQ12 handler and
 * unmasks the line -- plus the IRQ2 cascade, since IRQ12 lives on the
 * slave 8259 -- lets the keyboard go and puts interrupts back as it found
 * them.  Confining all command / ACK traffic to that polled phase keeps
 * the IRQ handler pure: once the line is live it only ever sees streaming
 * data packets, never an ACK byte it might mistake for byte 0 of a
 * packet.  (It cannot run beside kbd_init in Phase 1: IRQ12 firing before
 * clock_init divides by an uncalibrated pit_hz.)
 *
 * The first answer it does not like ends it: the aux port goes back down,
 * IRQ12 stays masked, the failed step is logged, and boot carries on
 * without a mouse.  So does a device id other than 0 -- a wheel mouse
 * already in four-byte mode, which this three-byte driver would misread.
 *
 * mouse_getpkt / mouse_getpkt_block dequeue completed packets.  The raw
 * 3-byte PS/2 movement-packet layout (scancode-independent) is:
 *	byte 0	YO XO YS XS  1 M R L	overflow + sign flags, always-1, buttons
 *	byte 1	X delta, two's complement (sign in byte 0 bit 4)
 *	byte 2	Y delta, two's complement (sign in byte 0 bit 5)
 *
 * Single-consumer, across BOTH readers: one thread at a time calls
 * mouse_getpkt and mouse_getpkt_block between them (the mouse-drv thread,
 * in this kernel) -- the ring's tail has one writer, and it is that
 * caller.  Exactly as for kbd_getc / kbd_getc_block.
 */

void	mouse_init(void);

/*
 * Copy the next assembled packet into out[0..2] -- out must have room for
 * three bytes.  mouse_getpkt returns 0 on success or -1 if the ring is
 * empty; mouse_getpkt_block parks the caller until a packet arrives and
 * always returns 0.
 */
int	mouse_getpkt(uint8_t *out);
int	mouse_getpkt_block(uint8_t *out);

/*
 * Test-only entry: feed a synthetic 3-byte packet through the exact
 * assembly + ring path the IRQ uses, so the boot self-test in mouse_drv.c
 * can prove feed -> ring -> driver-thread -> Mach-message end to end
 * without depending on physical mouse motion.  Not used in normal
 * operation.
 *
 * Returns 0 once the packet is on the ring, or -1 without feeding
 * anything if the assembler was part-way through a live packet -- three
 * bytes fed then would not be one packet.
 */
int	mouse_selftest_feed(uint8_t b0, uint8_t b1, uint8_t b2);

#endif /* !_DEV_MOUSE_H_ */
