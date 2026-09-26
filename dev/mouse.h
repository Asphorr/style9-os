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
 * PS/2 mouse: the aux device on the keyboard's i8042 controller (kbd.c),
 * on IRQ12.
 *
 * mouse_init brings the aux port up by polling, with interrupts off and
 * the keyboard port held (so no keystroke lands ahead of a reply): enable
 * the aux port, set the config to raise IRQ12, probe the device
 * (set-defaults / get-id / enable-streaming, each ACKed with 0xFA), then
 * install the handler and unmask IRQ12 and the IRQ2 cascade.  All
 * command/ACK traffic stays in that polled phase, so the IRQ handler only
 * ever sees data packets, never an ACK it could take for byte 0.  It must
 * run in Phase 2, after clock_init: IRQ12 before then would divide by an
 * uncalibrated pit_hz.
 *
 * The first bad answer, or a device id other than 0 (e.g. a wheel mouse
 * already in four-byte mode), ends it: aux port down, IRQ12 masked, the
 * step logged, and boot carries on without a mouse.
 *
 * mouse_getpkt / mouse_getpkt_block dequeue completed packets:
 *	byte 0	YO XO YS XS  1 M R L	overflow + sign flags, always-1, buttons
 *	byte 1	X delta, two's complement (sign in byte 0 bit 4)
 *	byte 2	Y delta, two's complement (sign in byte 0 bit 5)
 *
 * Single consumer across both calls: one thread (mouse-drv) writes the
 * ring's tail, as with kbd_getc / kbd_getc_block.
 */

void	mouse_init(void);

/*
 * Copy the next packet into out[0..2].  mouse_getpkt returns 0, or -1 if
 * the ring is empty; mouse_getpkt_block parks until a packet arrives and
 * always returns 0.
 */
int	mouse_getpkt(uint8_t *out);
int	mouse_getpkt_block(uint8_t *out);

/*
 * Test only: feed a synthetic packet through the IRQ's assembly and ring
 * path, for the boot self-test in mouse_drv.c.  Returns 0 once it is on
 * the ring, or -1, feeding nothing, if a live packet is half-assembled.
 */
int	mouse_selftest_feed(uint8_t b0, uint8_t b1, uint8_t b2);

#endif /* !_DEV_MOUSE_H_ */
