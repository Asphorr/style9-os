/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_KBD_DRV_H_
#define	_SYS_KBD_DRV_H_

#include <stdbool.h>

#include "port.h"

/*
 * Keyboard driver -- the first kernel subsystem that ships data over a
 * real Mach port instead of being called inline by the consumer.
 *
 * At boot, kbd_drv_init() allocates `kbd_input_port` in `kernel_space`
 * with RECEIVE | SEND rights, then spawns a dedicated kernel thread
 * (`kbd-drv`) that loops over the existing IRQ-driven dev/kbd ring.
 * For each byte the ring yields, the thread builds a 24-byte
 * mach_msg_header with the character packed into msgh_id and sends it
 * to `kbd_input_port` from `kernel_space`.
 *
 * The shell (or any other consumer holding the RECEIVE right) reads
 * keys with mach_msg_recv_block(kernel_space, kbd_input_port, ...).
 * Multiple consumers are possible in principle but only one can hold
 * RECEIVE at a time; the shell is the sole reader.
 *
 * The msg id space:
 *	1..0xFF		one ASCII byte in the low octet
 *	0		reserved / null
 *	0x100+		reserved for future events (key release, function
 *			keys, modifier-only edges, ...)
 */

/*
 * A second consumer, and the seam that keeps this file from knowing about
 * it.  The Mach port above is the native shell's; a Darwin binary reading
 * its stdin wants the same keystrokes and must not race the shell for them.
 * So the driver offers each byte to a registered sink FIRST: a sink that
 * returns true has taken the key and the Mach send is skipped; false means
 * nobody else wants it and the port gets it as always.
 *
 * The policy -- which is to say, who holds the console and when -- lives
 * entirely on the sink's side (kern/darwin.c).  A device driver deciding
 * that would be a device driver that knows what a Darwin task is.
 */
typedef bool	(*kbd_sink_fn)(char c);

extern mach_port_name_t	kbd_input_port;

void	kbd_drv_init(void);
void	kbd_drv_set_sink(kbd_sink_fn fn);

#endif /* !_SYS_KBD_DRV_H_ */
