/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "dbgcon.h"
#include "panic.h"
#include "spinlock.h"
#include "tty.h"
#include "uart.h"

#define	VGA_BASE	((volatile uint16_t *)0x000B8000)
#define	VGA_CELLS	((size_t)TTY_COLS * TTY_ROWS)

static uint16_t	tty_col;
static uint16_t	tty_row;
static uint8_t	tty_attr;

static struct spinlock	tty_lock = SPINLOCK_INIT("tty");

static void	tty_putcell(uint16_t, uint16_t, char);
static void	tty_scroll(void);
static uint16_t	tty_cell(char);

void
tty_init(void)
{

	tty_attr = TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK);
	tty_col  = 0;
	tty_row  = 0;
	tty_clear();
}

void
tty_clear(void)
{
	uint16_t	blank;
	size_t		i;

	blank = tty_cell(' ');
	for (i = 0; i < VGA_CELLS; i++)
		VGA_BASE[i] = blank;

	tty_col = 0;
	tty_row = 0;
}

void
tty_set_attr(uint8_t attr)
{

	tty_attr = attr;
}

void
tty_putc(char ch)
{
	bool	locked;

	/*
	 * Bypass the lock while we are in the panic / debugger path:
	 * the offending code may already hold tty_lock, and re-entering
	 * spin_lock would self-deadlock.  Standard BSD discipline.
	 */
	locked = false;
	if (!panic_in_progress) {
		spin_lock(&tty_lock);
		locked = true;
	}

	dbgcon_putc(ch);
	uart_putc(ch);

	switch (ch) {
	case '\n':
		tty_col = 0;
		tty_row++;
		break;
	case '\r':
		tty_col = 0;
		break;
	case '\t':
		tty_col = (uint16_t)((tty_col + 8) & ~7u);
		break;
	case '\b':
		if (tty_col > 0)
			tty_col--;
		tty_putcell(tty_col, tty_row, ' ');
		break;
	default:
		if ((unsigned char)ch >= 0x20) {
			tty_putcell(tty_col, tty_row, ch);
			tty_col++;
		}
		break;
	}

	if (tty_col >= TTY_COLS) {
		tty_col = 0;
		tty_row++;
	}
	if (tty_row >= TTY_ROWS)
		tty_scroll();

	if (locked)
		spin_unlock(&tty_lock);
}

void
tty_puts(const char *s)
{

	while (*s != '\0')
		tty_putc(*s++);
}

void
tty_write(const char *s, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		tty_putc(s[i]);
}

static void
tty_putcell(uint16_t col, uint16_t row, char ch)
{
	size_t	off;

	off = (size_t)row * TTY_COLS + col;
	VGA_BASE[off] = tty_cell(ch);
}

static void
tty_scroll(void)
{
	uint16_t	blank;
	size_t		i, top, bot;

	top = 0;
	bot = (size_t)(TTY_ROWS - 1) * TTY_COLS;

	for (i = top; i < bot; i++)
		VGA_BASE[i] = VGA_BASE[i + TTY_COLS];

	blank = tty_cell(' ');
	for (i = bot; i < VGA_CELLS; i++)
		VGA_BASE[i] = blank;

	tty_row = TTY_ROWS - 1;
}

static uint16_t
tty_cell(char ch)
{

	return ((uint16_t)((uint16_t)tty_attr << 8) | (uint8_t)ch);
}
