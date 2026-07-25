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
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "spinlock.h"
#include "tty.h"
#include "uart.h"

#define	VGA_BASE	((volatile uint16_t *)0x000B8000)
#define	VGA_CELLS	((size_t)TTY_COLS * TTY_ROWS)

/*
 * The text cursor is hardware, not a cell.
 *
 * The CRT controller draws the underline from a 16-bit character offset
 * held in two of its indexed registers, and it stays exactly where it
 * was last told until something tells it again.  Nothing here ever did:
 * the blitter moved tty_row / tty_col and wrote cells, and the underline
 * sat wherever the machine came up with it while the text appeared
 * somewhere else on the screen.
 *
 * For a boot log that is cosmetic.  For a shell it is not -- the cursor
 * is the only thing on the screen that says where the next character
 * the user types will land, and a cursor that lies about that is worse
 * than no cursor at all.
 *
 * Two registers, addressed through an index port: write the register
 * number to VGA_CRTC_INDEX, then the byte to VGA_CRTC_DATA.  The offset
 * is the same cell index the blitter uses, so there is nothing to
 * convert.
 */
#define	VGA_CRTC_INDEX		0x3D4
#define	VGA_CRTC_DATA		0x3D5
#define	VGA_CRTC_CURSOR_HI	14
#define	VGA_CRTC_CURSOR_LO	15

/*
 * ANSI/VT CSI state machine.
 *
 * Bytes arriving at the VGA console flow through a three-state parser:
 *
 *	GROUND	default; printable bytes go to the cell grid, ESC starts a
 *		sequence.
 *	ESC	saw 0x1B; the next byte selects the family.  Only "ESC ["
 *		(CSI introducer) is honoured today; everything else dumps
 *		us back to GROUND silently.
 *	CSI	accumulating parameters (digits + ';'), with an optional
 *		leading '?' for DEC private sequences.  A "final byte"
 *		(0x40..0x7E) dispatches and resets to GROUND.
 *
 * Mirrors (uart, dbgcon) ALWAYS see the raw byte stream so a real
 * terminal connected to COM1 interprets the same CSI the VGA console
 * obeys.  Only the in-kernel VGA blitter cares about the parse.
 *
 * Final bytes implemented:
 *	H / f	CUP -- cursor position, 1-based (row;col)
 *	A B C D	CUU/CUD/CUF/CUB -- cursor up/down/right/left N
 *	J	ED  -- erase in display: 0=>EOS, 1=>start->cur, 2=screen
 *	K	EL  -- erase in line:    same parameter scheme
 *	m	SGR -- 0/30..37/40..47/90..97/100..107 (+ 1, 22)
 *	s / u	save / restore cursor
 *
 * Unknown finals drop silently rather than erroring out: a future TUI
 * speaking richer sequences must extend this table, but won't crash.
 */
#define	TTY_CSI_MAX_PARAMS	8

enum tty_state {
	TTY_S_GROUND = 0,
	TTY_S_ESC,
	TTY_S_CSI,
};

static uint16_t		tty_col;
static uint16_t		tty_row;
static uint8_t		tty_attr;
static uint8_t		tty_attr_default;

static uint16_t	tty_saved_col;
static uint16_t	tty_saved_row;
static uint8_t	tty_saved_attr;
static bool	tty_have_saved;

static enum tty_state	csi_state;
static uint16_t		csi_params[TTY_CSI_MAX_PARAMS];	/* clamped 0..9999 */
static uint8_t		csi_nparam;
static bool		csi_have_param;	/* current slot has digits */
static bool		csi_private;	/* leading '?' DEC-private */

static struct spinlock	tty_lock = SPINLOCK_INIT("tty");

/*
 * What the CRTC was last told, and how often it had to be told.
 *
 * Programming the cursor is four port writes, and this console's only
 * primitive is one character at a time -- kprintf, the ring-3 write
 * path and ddb all funnel through tty_putc.  Placed naively that spends
 * four port writes on every byte of a 50 KiB boot log, and the first
 * version here did exactly that.  It cost 420 ms of a 4.4 s boot,
 * measured by turning the sync off and booting again: 3970 ms to reach
 * the shell without it, 4390 ms with.  Eight microseconds per character
 * is what a legacy ISA-timed port write costs, and no amount of arguing
 * that the cursor matters makes a tenth of the boot spent on it a good
 * trade.
 *
 * So the cursor is programmed once per WRITE rather than once per byte,
 * which is what a real console driver does and what the hardware wants:
 * only the position after the last character of a run was ever going to
 * be visible.  tty_puts, tty_write and kvprintf bracket themselves with
 * tty_batch_begin / tty_batch_end; inside a batch a move only marks the
 * cursor dirty, and the four port writes happen once when the outermost
 * bracket closes.  Nesting is counted, so a kprintf reached from inside
 * another one still leaves exactly one programming at the end.
 *
 * The cached offset is the other half: a move that lands where the CRTC
 * already points costs nothing, which covers every byte of a CSI
 * sequence -- parameters and final bytes either leave the cursor alone
 * or move it once, not once per digit.
 *
 * All three numbers are reported in the boot log rather than asserted
 * here, so this comment can be checked instead of believed.
 */
static uint16_t		tty_cursor_at;		/* (p) offset the CRTC holds */
static bool		tty_cursor_dirty;	/* (p) moved inside a batch  */
static uint32_t		tty_batch_depth;	/* (p) open brackets         */
static uint64_t		tty_cursor_pokes;	/* (p) */
static uint64_t		tty_batches;		/* (p) */
static uint64_t		tty_chars;		/* (p) */

static void	tty_putcell(uint16_t, uint16_t, char);
static void	tty_cursor_sync(void);
static void	tty_cursor_program(uint16_t);
static void	tty_scroll(void);
static uint16_t	tty_cell(char);
static void	tty_putc_vga(char);
static void	tty_ground_put(char);
static void	tty_csi_dispatch(char);
static void	tty_csi_reset(void);
static void	tty_erase_range(size_t lo, size_t hi);
static uint16_t	csi_param_or(uint8_t idx, uint16_t fallback);
static uint16_t	clamp_u16(int v, int lo, int hi);

void
tty_init(void)
{

	tty_attr_default = TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK);
	tty_attr         = tty_attr_default;
	tty_col          = 0;
	tty_row          = 0;
	tty_saved_col    = 0;
	tty_saved_row    = 0;
	tty_saved_attr   = tty_attr_default;
	tty_have_saved   = false;
	csi_state        = TTY_S_GROUND;
	tty_csi_reset();
	/*
	 * Force the first sync: 0xFFFF is not a cell any 80x25 screen has,
	 * so tty_clear's move to the home position cannot be mistaken for
	 * "already there" and skipped.
	 */
	tty_cursor_at    = 0xFFFFu;
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
	tty_cursor_sync();
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

	/*
	 * Mirrors get the byte stream RAW -- a terminal hooked to COM1
	 * needs the ESC sequences intact to interpret CUP/SGR/etc.; the
	 * QEMU debugcon is purely a log sink and doesn't care either way.
	 * Only the VGA blitter runs the parse.
	 */
	dbgcon_putc(ch);
	uart_putc(ch);

	tty_putc_vga(ch);
	tty_chars++;
	tty_cursor_sync();

	if (locked)
		spin_unlock(&tty_lock);
}

/* ---- hardware cursor ------------------------------------------------- */

/*
 * Tell the CRTC where the cursor is, if it does not already know.
 *
 * Called at the tail of every tty_putc rather than from each of the
 * dozen places that move tty_row / tty_col: a cursor that is correct
 * after some moves and stale after others is harder to reason about
 * than one that is correct after all of them, and the cached offset
 * makes the redundant calls free.
 */
static void
tty_cursor_sync(void)
{
	uint16_t	off;

	off = (uint16_t)((size_t)tty_row * TTY_COLS + tty_col);
	if (off == tty_cursor_at) {
		/*
		 * Back where the hardware already points.  This can happen
		 * inside a batch -- a save/restore pair, or a backspace and
		 * a reprint -- and it settles the debt: there is nothing
		 * left to program at the end.
		 */
		tty_cursor_dirty = false;
		return;
	}
	if (tty_batch_depth != 0) {
		tty_cursor_dirty = true;
		return;
	}
	tty_cursor_program(off);
}

static void
tty_cursor_program(uint16_t off)
{

	tty_cursor_at    = off;
	tty_cursor_dirty = false;
	tty_cursor_pokes++;

	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_HI);
	outb(VGA_CRTC_DATA, (uint8_t)(off >> 8));
	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LO);
	outb(VGA_CRTC_DATA, (uint8_t)(off & 0xFFu));
}

/*
 * Open and close a run of output.
 *
 * The lock is taken for the counter rather than held across the run:
 * spin_lock panics on a same-CPU re-acquire, and every tty_putc inside
 * the bracket takes it itself.  Two acquisitions per kprintf against
 * the thousands it was already making per boot is not a cost worth
 * restructuring the console to avoid.
 *
 * Unbalanced brackets are survivable by construction -- the depth only
 * ever gates a cursor update, so the worst a leaked tty_batch_begin can
 * do is leave the underline one position stale until the next write
 * closes a bracket.
 */
void
tty_batch_begin(void)
{
	bool	locked;

	locked = false;
	if (!panic_in_progress) {
		spin_lock(&tty_lock);
		locked = true;
	}
	tty_batch_depth++;
	tty_batches++;
	if (locked)
		spin_unlock(&tty_lock);
}

void
tty_batch_end(void)
{
	bool	locked;

	locked = false;
	if (!panic_in_progress) {
		spin_lock(&tty_lock);
		locked = true;
	}
	if (tty_batch_depth > 0)
		tty_batch_depth--;
	if (tty_batch_depth == 0 && tty_cursor_dirty)
		tty_cursor_program((uint16_t)((size_t)tty_row * TTY_COLS +
		    tty_col));
	if (locked)
		spin_unlock(&tty_lock);
}

/*
 * Read the offset back out of the hardware.
 *
 * Exists for the selftest, and only for it: the cached tty_cursor_at
 * says what this code BELIEVES it wrote, which is exactly the thing a
 * test of "did the write happen" must not consult.  The CRTC index
 * register is shared state, so this leaves it pointing at the low byte
 * -- harmless, since every writer sets the index before the data.
 */
uint16_t
tty_cursor_hw(void)
{
	uint16_t	off;

	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_HI);
	off = (uint16_t)((uint16_t)inb(VGA_CRTC_DATA) << 8);
	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_LO);
	off = (uint16_t)(off | inb(VGA_CRTC_DATA));
	return (off);
}

uint16_t
tty_cursor_cell(void)
{

	return ((uint16_t)((size_t)tty_row * TTY_COLS + tty_col));
}

void
tty_stats(void)
{

	kprintf("tty: %llu characters blitted in %llu writes -- the cursor was "
	    "programmed %llu times, %llu per 100 characters\n",
	    (unsigned long long)tty_chars,
	    (unsigned long long)tty_batches,
	    (unsigned long long)tty_cursor_pokes,
	    (unsigned long long)(tty_chars == 0 ? 0 :
	    tty_cursor_pokes * 100 / tty_chars));
}

/* ---- ANSI/VT state machine ------------------------------------------- */

static void
tty_csi_reset(void)
{
	size_t	i;

	for (i = 0; i < TTY_CSI_MAX_PARAMS; i++)
		csi_params[i] = 0;
	csi_nparam     = 0;
	csi_have_param = false;
	csi_private    = false;
}

static uint16_t
clamp_u16(int v, int lo, int hi)
{

	if (v < lo)
		return ((uint16_t)lo);
	if (v > hi)
		return ((uint16_t)hi);
	return ((uint16_t)v);
}

static uint16_t
csi_param_or(uint8_t idx, uint16_t fallback)
{

	if (idx >= csi_nparam)
		return (fallback);
	if (csi_params[idx] == 0)
		return (fallback);
	return (csi_params[idx]);
}

/*
 * Blank a contiguous range of cells.  Both ends are inclusive of the
 * VGA cell index space; clamp the upper bound to VGA_CELLS - 1 so
 * malformed CSI sequences cannot scribble past the framebuffer.
 */
static void
tty_erase_range(size_t lo, size_t hi)
{
	uint16_t	blank;
	size_t		i;

	if (hi >= VGA_CELLS)
		hi = VGA_CELLS - 1;
	if (lo > hi)
		return;
	blank = tty_cell(' ');
	for (i = lo; i <= hi; i++)
		VGA_BASE[i] = blank;
}

static void
tty_putc_vga(char ch)
{
	uint16_t	v;

	switch (csi_state) {
	case TTY_S_GROUND:
		tty_ground_put(ch);
		return;

	case TTY_S_ESC:
		if (ch == '[') {
			tty_csi_reset();
			csi_state = TTY_S_CSI;
			return;
		}
		/*
		 * ESC c -- terminal reset (RIS).  Cheap to honour and the
		 * idiomatic "scrub state" sequence from a misbehaving app.
		 */
		if (ch == 'c') {
			tty_attr       = tty_attr_default;
			tty_have_saved = false;
			csi_state      = TTY_S_GROUND;
			tty_clear();
			return;
		}
		/* Unknown ESC family -- drop silently. */
		csi_state = TTY_S_GROUND;
		return;

	case TTY_S_CSI:
		if (ch >= '0' && ch <= '9') {
			if (!csi_have_param) {
				if (csi_nparam >= TTY_CSI_MAX_PARAMS)
					return;	/* clamp; drop further digits */
				csi_nparam++;
				csi_have_param = true;
			}
			v = csi_params[csi_nparam - 1];
			v = (uint16_t)(v * 10 + (uint16_t)(ch - '0'));
			if (v > 9999)
				v = 9999;
			csi_params[csi_nparam - 1] = v;
			return;
		}
		if (ch == ';') {
			if (!csi_have_param) {
				if (csi_nparam < TTY_CSI_MAX_PARAMS)
					csi_nparam++;
			}
			csi_have_param = false;
			return;
		}
		if (ch == '?') {
			csi_private = true;
			return;
		}
		if ((unsigned char)ch >= 0x40 && (unsigned char)ch <= 0x7E) {
			tty_csi_dispatch(ch);
			csi_state = TTY_S_GROUND;
			return;
		}
		/* Intermediate bytes / unknowns -- drop. */
		return;
	}
}

static void
tty_ground_put(char ch)
{

	if (ch == 0x1B) {
		csi_state = TTY_S_ESC;
		return;
	}

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
}

/* ---- CSI final-byte handlers ----------------------------------------- */

static void
csi_apply_sgr(void)
{
	uint8_t	fg;
	uint8_t	bg;
	uint8_t	i;

	/*
	 * Decompose the working attribute on entry so we can mutate fg /
	 * bg independently across the parameter stream.  4-bit fg, 3-bit
	 * bg (VGA leaves the high bg bit for blink in default text mode;
	 * we mask it off so SGR 100..107 can't accidentally set blink).
	 */
	fg = (uint8_t)(tty_attr & 0x0Fu);
	bg = (uint8_t)((tty_attr >> 4) & 0x07u);

	if (csi_nparam == 0) {
		/* CSI m == CSI 0 m -- reset to default. */
		tty_attr = tty_attr_default;
		return;
	}

	for (i = 0; i < csi_nparam; i++) {
		uint16_t	p = csi_params[i];

		if (p == 0) {
			fg = (uint8_t)(tty_attr_default & 0x0Fu);
			bg = (uint8_t)((tty_attr_default >> 4) & 0x07u);
			continue;
		}
		if (p == 1) {
			/* Bold == bright bit on VGA fg. */
			fg = (uint8_t)(fg | 0x08u);
			continue;
		}
		if (p == 22) {
			fg = (uint8_t)(fg & 0x07u);
			continue;
		}
		if (p >= 30 && p <= 37) {
			fg = (uint8_t)((fg & 0x08u) | (uint8_t)(p - 30u));
			continue;
		}
		if (p >= 40 && p <= 47) {
			bg = (uint8_t)(p - 40u);
			continue;
		}
		if (p >= 90 && p <= 97) {
			fg = (uint8_t)((uint8_t)(p - 90u) | 0x08u);
			continue;
		}
		if (p >= 100 && p <= 107) {
			bg = (uint8_t)((uint8_t)(p - 100u) & 0x07u);
			continue;
		}
		/* Unknown -- drop. */
	}

	tty_attr = TTY_ATTR(fg, bg);
}

static void
tty_csi_dispatch(char final)
{
	uint16_t	n;
	uint16_t	row, col;
	size_t		cur_off;
	size_t		row_lo;
	size_t		row_hi;

	/*
	 * Refuse DEC private sequences for now -- we don't implement any
	 * (no DECCKM / DECTCEM cursor visibility, no application keypad).
	 * Drop quietly so xterm-style apps that probe with "ESC [ ? 25 h"
	 * don't get their probe bytes blitted as garbage cells.
	 */
	if (csi_private)
		return;

	switch (final) {
	case 'H':
	case 'f':
		row = csi_param_or(0, 1);
		col = csi_param_or(1, 1);
		tty_row = clamp_u16((int)row - 1, 0, TTY_ROWS - 1);
		tty_col = clamp_u16((int)col - 1, 0, TTY_COLS - 1);
		return;

	case 'A':
		n = csi_param_or(0, 1);
		tty_row = clamp_u16((int)tty_row - (int)n, 0, TTY_ROWS - 1);
		return;
	case 'B':
		n = csi_param_or(0, 1);
		tty_row = clamp_u16((int)tty_row + (int)n, 0, TTY_ROWS - 1);
		return;
	case 'C':
		n = csi_param_or(0, 1);
		tty_col = clamp_u16((int)tty_col + (int)n, 0, TTY_COLS - 1);
		return;
	case 'D':
		n = csi_param_or(0, 1);
		tty_col = clamp_u16((int)tty_col - (int)n, 0, TTY_COLS - 1);
		return;

	case 'J':
		/*
		 * ED -- 0 (default): cursor to end of screen.
		 *        1: start of screen to cursor (inclusive).
		 *        2: entire screen, cursor home.
		 *        3: scrollback (n/a here).
		 */
		n = csi_nparam == 0 ? 0 : csi_params[0];
		cur_off = (size_t)tty_row * TTY_COLS + tty_col;
		switch (n) {
		case 0:
			tty_erase_range(cur_off, VGA_CELLS - 1);
			break;
		case 1:
			tty_erase_range(0, cur_off);
			break;
		case 2:
		case 3:
			tty_erase_range(0, VGA_CELLS - 1);
			tty_col = 0;
			tty_row = 0;
			break;
		}
		return;

	case 'K':
		/*
		 * EL -- same parameter scheme but bounded to the cursor row.
		 */
		n = csi_nparam == 0 ? 0 : csi_params[0];
		row_lo = (size_t)tty_row * TTY_COLS;
		row_hi = row_lo + (TTY_COLS - 1);
		cur_off = row_lo + tty_col;
		switch (n) {
		case 0:
			tty_erase_range(cur_off, row_hi);
			break;
		case 1:
			tty_erase_range(row_lo, cur_off);
			break;
		case 2:
			tty_erase_range(row_lo, row_hi);
			break;
		}
		return;

	case 'm':
		csi_apply_sgr();
		return;

	case 's':
		tty_saved_col  = tty_col;
		tty_saved_row  = tty_row;
		tty_saved_attr = tty_attr;
		tty_have_saved = true;
		return;
	case 'u':
		if (tty_have_saved) {
			tty_col  = tty_saved_col;
			tty_row  = tty_saved_row;
			tty_attr = tty_saved_attr;
		}
		return;

	default:
		/* Unknown final byte -- drop. */
		return;
	}
}

/* ---- selftest -------------------------------------------------------- */

/*
 * Ask the hardware, never the driver.
 *
 * Every check here compares tty_cursor_hw() -- four port reads from the
 * CRTC -- against a position worked out on paper, not against
 * tty_cursor_cell().  Comparing the driver's two ideas of where the
 * cursor is would pass with the CRTC never written at all, which is
 * precisely the bug this exists to catch.
 */
static bool
tty_cursor_is(uint16_t want, const char *what)
{
	uint16_t	got;

	got = tty_cursor_hw();
	if (got == want)
		return (true);
	tty_clear();
	kprintf("tty-cursor: FAIL %s -- the CRTC holds cell %u (row %u col %u),"
	    " the text is at cell %u (row %u col %u)\n",
	    what,
	    (unsigned)got, (unsigned)(got / TTY_COLS), (unsigned)(got % TTY_COLS),
	    (unsigned)want,
	    (unsigned)(want / TTY_COLS), (unsigned)(want % TTY_COLS));
	return (false);
}

void
tty_selftest(void)
{
	uint64_t	pokes_before;
	uint64_t	pokes_spent;

	/*
	 * 1. Absolute positioning.  CUP is the sequence every full-screen
	 *    program uses to put the cursor somewhere; if the CRTC does not
	 *    follow it, nothing else here can be trusted either.
	 */
	tty_puts("\x1b[8;13H");
	if (!tty_cursor_is(7 * TTY_COLS + 12, "after CUP to row 8 column 13"))
		return;

	/*
	 * 2. Ordinary text.  Five printable bytes move the cursor five
	 *    cells; this is the case that matters, since it is what typing
	 *    at a shell prompt looks like.
	 */
	tty_puts("style");
	if (!tty_cursor_is(7 * TTY_COLS + 17, "after five printable bytes"))
		return;

	/* 3. A newline returns to column zero of the next row. */
	tty_putc('\n');
	if (!tty_cursor_is(8 * TTY_COLS, "after a newline"))
		return;

	/*
	 * 4. A scroll.  The rows move up under the cursor and the cursor
	 *    stays on the bottom row -- a case the blitter handles in
	 *    tty_scroll rather than in the character path, so it is a
	 *    genuinely separate way to get this wrong.
	 */
	tty_puts("\x1b[25;1H");
	if (!tty_cursor_is((TTY_ROWS - 1) * TTY_COLS, "after CUP to the last row"))
		return;
	tty_putc('\n');
	if (!tty_cursor_is((TTY_ROWS - 1) * TTY_COLS, "after scrolling"))
		return;

	/*
	 * 5. The batch.  Twenty characters are one write, and a write is
	 *    what the CRTC is told about -- one programming, not twenty.
	 *    This is the property that makes it affordable to keep the
	 *    cursor honest on a per-character primitive, so it is checked
	 *    rather than assumed.
	 */
	pokes_before = tty_cursor_pokes;
	tty_puts("\x1b[12;34Hbatched output.");
	pokes_spent = tty_cursor_pokes - pokes_before;
	if (!tty_cursor_is(11 * TTY_COLS + 33 + 15, "after a batched write"))
		return;
	if (pokes_spent != 1) {
		tty_clear();
		kprintf("tty-cursor: FAIL a 23-byte write cost %llu cursor "
		    "programmings, not 1\n", (unsigned long long)pokes_spent);
		return;
	}

	/*
	 * 6. The cache, which is a separate claim from the batch: a write
	 *    that puts the cursor back where the hardware already points
	 *    must cost nothing at all.
	 */
	pokes_before = tty_cursor_pokes;
	tty_puts("\x1b[12;49H");
	pokes_spent = tty_cursor_pokes - pokes_before;
	if (pokes_spent != 0) {
		tty_clear();
		kprintf("tty-cursor: FAIL moving the cursor to where it "
		    "already was cost %llu programmings, not 0\n",
		    (unsigned long long)pokes_spent);
		return;
	}

	/* 7. And clearing the screen brings it home. */
	tty_clear();
	if (!tty_cursor_is(0, "after clearing the screen"))
		return;

	kprintf("tty-cursor: PASS -- the CRTC followed the text through "
	    "positioning, typing, a newline, a scroll and a clear; a 23-byte "
	    "write cost one programming and a move to where it already was "
	    "cost none\n");
}

void
tty_puts(const char *s)
{

	tty_batch_begin();
	while (*s != '\0')
		tty_putc(*s++);
	tty_batch_end();
}

void
tty_write(const char *s, size_t n)
{
	size_t	i;

	tty_batch_begin();
	for (i = 0; i < n; i++)
		tty_putc(s[i]);
	tty_batch_end();
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
