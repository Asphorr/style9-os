/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _DEV_KBD_H_
#define	_DEV_KBD_H_

#include <stdint.h>

/*
 * PS/2 keyboard, scancode set 1.
 *
 * Two interfaces are provided:
 *
 *	kbd_init / kbd_getc
 *	    The normal IRQ-driven path.  kbd_init installs the IRQ1
 *	    handler and unmasks the line at the PIC.  kbd_getc dequeues
 *	    one already-translated character from the ring buffer the
 *	    handler pushes into, returning -1 if the buffer is empty.
 *
 *	kbd_poll_getc / kbd_poll_getc_block
 *	    Polled path -- reads the controller directly without
 *	    relying on IRQs.  Required from contexts that cannot trust
 *	    interrupts (panic, in-kernel debugger).  poll_getc returns
 *	    -1 if no character is currently available; poll_getc_block
 *	    spins on PAUSE until one is.
 *
 * Modifier handling is intentionally minimal: only left/right Shift
 * track state.  Caps-Lock, Ctrl, Alt, Meta are deferred.  Shift state
 * is shared between the IRQ and polled paths.
 */

void	kbd_init(void);
int	kbd_getc(void);

int	kbd_poll_getc(void);
int	kbd_poll_getc_block(void);

#endif /* !_DEV_KBD_H_ */
