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
 * (fg:4 | bg:4; blink is turned off at init).  Output is serialised per
 * write by the console lock in dev/tty.c, which is recursive per CPU.
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

/*
 * The cursor as a cell offset (row * TTY_COLS + col), two ways:
 * tty_cursor_cell is what the driver thinks, tty_cursor_hw what the CRT
 * controller was told.  Tests compare the latter with an expected value.
 */
uint16_t	tty_cursor_cell(void);
uint16_t	tty_cursor_hw(void);

/*
 * Bracket a run of output: the console is held for the whole run, so
 * other CPUs' output cannot interleave with it, and the hardware cursor
 * is programmed once at the end.  Nesting is counted.  A single tty_putc
 * is already one write.
 */
void	tty_batch_begin(void);
void	tty_batch_end(void);

/* Print characters, writes, cursor programmings and console waits. */
void	tty_stats(void);

/*
 * Boot selftests.  Each scribbles on the console and clears it, so they
 * must run before anything worth reading is printed.
 *
 *	tty_selftest		the hardware cursor follows the text, and a
 *				write programs it once
 *	tty_wrap_selftest	a line exactly TTY_COLS wide costs one row
 *	tty_region_selftest	rows above the top margin stay put while the
 *				region scrolls; hiding the cursor reaches
 *				the hardware
 *	tty_colour_selftest	the DAC holds our palette, all sixteen
 *				backgrounds are colours, and SGR handles
 *				reverse video and extended colours
 */
void	tty_selftest(void);
void	tty_wrap_selftest(void);
void	tty_region_selftest(void);
void	tty_colour_selftest(void);

#endif /* !_SYS_TTY_H_ */
