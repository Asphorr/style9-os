/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_TTY_H_
#define	_SYS_TTY_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Legacy VGA text-mode console.
 *
 * Memory-mapped at physical 0xB8000.  Eighty columns by twenty-five
 * rows, each cell two bytes: low = CP437 code point, high = attribute
 * (fg:4 | bg:3 | blink:1).  No locking yet -- there is exactly one
 * CPU and no scheduler.
 */

#define	TTY_COLS		80
#define	TTY_ROWS		25

#define	TTY_ATTR(fg, bg)	((uint8_t)(((bg) << 4) | ((fg) & 0x0F)))

enum tty_color {
	TTY_BLACK		= 0x0,
	TTY_BLUE		= 0x1,
	TTY_GREEN		= 0x2,
	TTY_CYAN		= 0x3,
	TTY_RED			= 0x4,
	TTY_MAGENTA		= 0x5,
	TTY_BROWN		= 0x6,
	TTY_LIGHT_GRAY		= 0x7,
	TTY_DARK_GRAY		= 0x8,
	TTY_LIGHT_BLUE		= 0x9,
	TTY_LIGHT_GREEN		= 0xA,
	TTY_LIGHT_CYAN		= 0xB,
	TTY_LIGHT_RED		= 0xC,
	TTY_LIGHT_MAGENTA	= 0xD,
	TTY_YELLOW		= 0xE,
	TTY_WHITE		= 0xF,
};

void	tty_init(void);
void	tty_clear(void);
void	tty_set_attr(uint8_t);
void	tty_putc(char);
void	tty_puts(const char *);
void	tty_write(const char *, size_t);

#endif /* !_SYS_TTY_H_ */
