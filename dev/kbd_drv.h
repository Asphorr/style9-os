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
 * Keyboard driver: delivers keys over a Mach port.
 *
 * kbd_drv_init() allocates `kbd_input_port' in kernel_space with RECEIVE
 * and SEND rights and starts the `kbd-drv' kernel thread, which sends
 * each byte from the dev/kbd ring to the port as a bare 24-byte header
 * with the character in msgh_id.
 *
 * One consumer holds the RECEIVE right: sh.elf, which takes it with
 * DEV_OP_OPEN_STREAM (or the legacy kern/shell.c, in kernel_space).
 *
 * The msg id space:
 *	1..0xFF		one ASCII byte in the low octet
 *	0		reserved / null
 *	0x100+		reserved for future events (key release, function
 *			keys, modifier-only edges, ...)
 */

/*
 * A second consumer.  A Darwin binary reading stdin wants the same keys
 * and must not race the shell for them, so each byte is offered to the
 * sink first: true means it took the key and the send is skipped; false
 * sends it to the port as usual.  Who holds the console is the sink's
 * policy (kern/darwin.c), not the driver's.
 */
typedef bool	(*kbd_sink_fn)(char c);

extern mach_port_name_t	kbd_input_port;

void	kbd_drv_init(void);
void	kbd_drv_set_sink(kbd_sink_fn fn);

#endif /* !_SYS_KBD_DRV_H_ */
