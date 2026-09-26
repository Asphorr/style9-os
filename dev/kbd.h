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
 *	kbd_init / kbd_getc
 *	    IRQ-driven.  kbd_init installs the IRQ1 handler and unmasks
 *	    the line; kbd_getc dequeues one translated character from
 *	    the handler's ring, or returns -1 if it is empty.
 *
 *	kbd_poll_getc / kbd_poll_getc_block
 *	    Polled, reading the controller directly, for contexts that
 *	    cannot rely on interrupts (panic, ddb).  poll_getc returns -1
 *	    if nothing is available; poll_getc_block spins until a key.
 *
 * Modifiers: Shift selects the shifted table, and Ctrl folds letters to
 * control codes (Ctrl-C = 0x03, Ctrl-D = 0x04) for the line discipline.
 * No Caps-Lock, Alt or Meta.  Both paths share the modifier state.
 */

void	kbd_init(void);
int	kbd_getc(void);

/*
 * Blocking read: parks the caller until the IRQ pushes a character.
 * Single consumer: at most one thread (the kbd-drv thread) may be in
 * here at a time.
 */
int	kbd_getc_block(void);

int	kbd_poll_getc(void);
int	kbd_poll_getc_block(void);

#endif /* !_DEV_KBD_H_ */
