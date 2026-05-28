/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_KBD_DRV_H_
#define	_SYS_KBD_DRV_H_

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

extern mach_port_name_t	kbd_input_port;

void	kbd_drv_init(void);

#endif /* !_SYS_KBD_DRV_H_ */
