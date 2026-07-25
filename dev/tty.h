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

/*
 * Where the cursor is, asked two different ways: tty_cursor_cell is
 * what this driver thinks, tty_cursor_hw is what the CRT controller was
 * actually told.  They exist as a pair because the only interesting
 * question about a hardware cursor is whether those two agree, and a
 * test that asked the driver twice would answer it wrongly every time.
 * Both return a cell offset, row * TTY_COLS + col.
 */
uint16_t	tty_cursor_cell(void);
uint16_t	tty_cursor_hw(void);

/*
 * Bracket a run of output so the hardware cursor is programmed once at
 * the end of it instead of once per character.  Nesting is counted, so
 * these may be used by anything that emits more than one byte -- see
 * the note in dev/tty.c for the measurement that made them necessary.
 * Callers that write a single character need not bother: tty_putc is
 * already exactly one write.
 */
void	tty_batch_begin(void);
void	tty_batch_end(void);

/* Characters blitted and cursor moves programmed, for the boot log. */
void	tty_stats(void);

/*
 * Prove, at boot and out loud, that the underline on the screen is
 * where the next character will go.  Scribbles on the console and
 * clears it afterwards, so it must run before anything worth reading
 * has been printed.
 */
void	tty_selftest(void);

/*
 * And that a line exactly TTY_COLS wide costs one row rather than two.
 * Same scribble-and-clear discipline as tty_selftest, and runs beside
 * it for the same reason.
 */
void	tty_wrap_selftest(void);

/*
 * And that rows above the top margin stay put while the rows below
 * them scroll -- the property a status bar is built on.  Also checks
 * that hiding the cursor reaches the hardware.
 */
void	tty_region_selftest(void);

#endif /* !_SYS_TTY_H_ */
