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
#define	VGA_CRTC_CURSOR_START	10	/* bit 5 turns the cursor off */
#define	VGA_CRTC_CURSOR_HI	14
#define	VGA_CRTC_CURSOR_LO	15
#define	VGA_CURSOR_DISABLE	0x20u

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
 *	r	DECSTBM -- top and bottom margins of the scrolling region
 *	s / u	save / restore cursor
 *	?25 h/l	DECTCEM -- show / hide the cursor
 *
 * Unknown finals drop silently rather than erroring out: a future TUI
 * speaking richer sequences must extend this table, but won't crash.
 *
 * DECOM (origin mode) is deliberately absent: CUP addresses the screen,
 * not the scrolling region, which is the mode every program here
 * assumes and the one a program that cares can no longer be surprised
 * by, since asking for the other is a DEC-private sequence that drops.
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

/*
 * The last column is a place to be, not a place to fall off.
 *
 * This blitter used to advance the column after every printable byte
 * and then wrap if the column had reached eighty -- so a line exactly
 * eighty columns wide left the cursor already on the next row, and the
 * newline that followed it advanced a SECOND time.  One blank row per
 * full-width line, silently.
 *
 * That is not a rounding error in a hobby console; it is why the
 * shell's own splash was missing its NAME section.  sh.elf paints an
 * eighty-column status line and then an eighty-column rule, expecting
 * them on rows one and two; they landed on rows one and three, and the
 * rule wiped the text that had been drawn there.
 *
 * Every real terminal defers the wrap instead: a character written to
 * the last column leaves the cursor IN the last column and arms a flag,
 * and only the next printable character actually moves to the next row.
 * Anything that positions the cursor -- carriage return, newline,
 * backspace, tab, any CSI -- disarms it without wrapping.  That is the
 * whole of DEC's "last column flag", and it is what makes a full-width
 * line cost one row.
 */
static bool		tty_wrap_pending;

/*
 * The rows that scroll, and the rows that do not.
 *
 * Everything on this console scrolled together, which meant a status
 * bar was a lie the moment anything printed: sh.elf painted one at row
 * zero on every prompt, and the first command whose output reached the
 * bottom of the screen carried the bar off the top and left its rule
 * behind as debris.  Repainting harder does not fix that -- the bar has
 * to survive until the next prompt, and between prompts a foreground
 * job is free to print.
 *
 * DECSTBM is the answer terminals settled on decades ago: a top and a
 * bottom margin, and only the rows between them move.  A program paints
 * its chrome above the top margin once and then forgets about it.
 * Inclusive, zero-based, and the whole screen until someone says
 * otherwise.
 */
static uint16_t		tty_scroll_top;
static uint16_t		tty_scroll_bot;

/*
 * Whether the underline is drawn at all.  A full-screen program hides
 * it across a repaint so the cursor is not seen sweeping the screen on
 * the way to where it belongs; DECTCEM is how it asks.
 */
static bool		tty_cursor_visible;

static uint16_t	tty_saved_col;
static uint16_t	tty_saved_row;
static uint8_t	tty_saved_attr;
static bool	tty_saved_wrap;
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
static void	tty_cursor_show(bool);
static void	tty_scroll(void);
static void	tty_linefeed(void);
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
	tty_saved_wrap   = false;
	tty_have_saved   = false;
	tty_scroll_top   = 0;
	tty_scroll_bot   = TTY_ROWS - 1;
	csi_state        = TTY_S_GROUND;
	tty_csi_reset();
	/*
	 * Whatever the firmware left the cursor register saying, this
	 * driver's opinion is "visible" -- so force the write rather than
	 * let the cached flag agree with a state nobody set.
	 */
	tty_cursor_visible = false;
	tty_cursor_show(true);
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
	tty_wrap_pending = false;
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
 * Draw the underline, or stop drawing it.
 *
 * The scan-line-start register holds the top row of the cursor glyph in
 * its low bits and the disable bit at 0x20, so this is a read-modify-
 * write rather than a store: clobbering the shape while turning the
 * cursor back on would leave a cursor that is visible and the wrong
 * height, which looks like a different bug entirely.
 */
static void
tty_cursor_show(bool on)
{
	uint8_t	v;

	if (on == tty_cursor_visible)
		return;
	tty_cursor_visible = on;

	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_START);
	v = inb(VGA_CRTC_DATA);
	if (on)
		v = (uint8_t)(v & (uint8_t)~VGA_CURSOR_DISABLE);
	else
		v = (uint8_t)(v | VGA_CURSOR_DISABLE);
	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_START);
	outb(VGA_CRTC_DATA, v);
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
		tty_wrap_pending = false;
		tty_col = 0;
		tty_linefeed();
		break;
	case '\r':
		tty_wrap_pending = false;
		tty_col = 0;
		break;
	case '\t':
		/*
		 * A tab never wraps -- it stops at the last column, the way
		 * every VT does.  Landing there arms the wrap the same as
		 * printing there would, so the next character starts the
		 * next row.
		 */
		tty_col = (uint16_t)((tty_col + 8) & ~7u);
		if (tty_col >= TTY_COLS) {
			tty_col = TTY_COLS - 1;
			tty_wrap_pending = true;
		} else {
			tty_wrap_pending = false;
		}
		break;
	case '\b':
		tty_wrap_pending = false;
		if (tty_col > 0)
			tty_col--;
		tty_putcell(tty_col, tty_row, ' ');
		break;
	default:
		if ((unsigned char)ch >= 0x20) {
			/*
			 * The wrap owed from the previous character is paid
			 * here, immediately before this one is placed --
			 * which is the entire point of deferring it.
			 */
			if (tty_wrap_pending) {
				tty_wrap_pending = false;
				tty_col = 0;
				tty_linefeed();
			}
			tty_putcell(tty_col, tty_row, ch);
			if (tty_col + 1 >= TTY_COLS)
				tty_wrap_pending = true;
			else
				tty_col++;
		}
		break;
	}
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
	uint16_t	margin_top, margin_bot;
	size_t		cur_off;
	size_t		row_lo;
	size_t		row_hi;

	/*
	 * DEC private sequences.  Only DECTCEM is answered; the rest drop
	 * quietly, so an xterm-style program probing for application
	 * keypad or bracketed paste gets no reply rather than its probe
	 * bytes blitted across the screen as garbage cells.
	 */
	if (csi_private) {
		if ((final == 'h' || final == 'l') && csi_nparam == 1 &&
		    csi_params[0] == 25)
			tty_cursor_show(final == 'h');
		return;
	}

	/*
	 * Anything that positions the cursor or erases under it disarms
	 * the deferred wrap -- the character that owed it is no longer the
	 * last thing written.  SGR and save-cursor are deliberately absent
	 * from this list: neither moves the cursor, so a colour change
	 * between the eightieth character and the eighty-first must not
	 * turn the wrap into an overwrite of the last column.
	 */
	switch (final) {
	case 'H':
	case 'f':
	case 'A':
	case 'B':
	case 'C':
	case 'D':
	case 'J':
	case 'K':
		tty_wrap_pending = false;
		break;
	default:
		break;
	}

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

	case 'r':
		/*
		 * DECSTBM.  One-based and inclusive on the wire; a region
		 * that would be empty or inverted is treated as "no region"
		 * rather than honoured into a screen that cannot scroll,
		 * which is what a VT does with the same input and what keeps
		 * a typo in a program from wedging the console.
		 *
		 * The cursor goes home to the top-left of the SCREEN, not of
		 * the region -- origin mode is not implemented, so a caller
		 * that wants to be inside its region says so with a CUP.
		 */
		margin_top = csi_param_or(0, 1);
		margin_bot = csi_param_or(1, TTY_ROWS);
		if (margin_bot > TTY_ROWS)
			margin_bot = TTY_ROWS;
		if (margin_top < 1 || margin_top >= margin_bot) {
			margin_top = 1;
			margin_bot = TTY_ROWS;
		}
		tty_scroll_top   = (uint16_t)(margin_top - 1);
		tty_scroll_bot   = (uint16_t)(margin_bot - 1);
		tty_row          = 0;
		tty_col          = 0;
		tty_wrap_pending = false;
		return;

	case 's':
		tty_saved_col  = tty_col;
		tty_saved_row  = tty_row;
		tty_saved_attr = tty_attr;
		tty_saved_wrap = tty_wrap_pending;
		tty_have_saved = true;
		return;
	case 'u':
		/*
		 * The armed wrap is part of the cursor's position, not a
		 * side effect of it -- restoring to the last column of a
		 * full row must restore whether the next character belongs
		 * there or on the row below.
		 */
		if (tty_have_saved) {
			tty_col          = tty_saved_col;
			tty_row          = tty_saved_row;
			tty_attr         = tty_saved_attr;
			tty_wrap_pending = tty_saved_wrap;
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
tty_cursor_is(const char *who, uint16_t want, const char *what)
{
	uint16_t	got;

	got = tty_cursor_hw();
	if (got == want)
		return (true);
	tty_clear();
	kprintf("%s: FAIL %s -- the CRTC holds cell %u (row %u col %u),"
	    " the text is at cell %u (row %u col %u)\n",
	    who, what,
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
	if (!tty_cursor_is("tty-cursor", 7 * TTY_COLS + 12,
	    "after CUP to row 8 column 13"))
		return;

	/*
	 * 2. Ordinary text.  Five printable bytes move the cursor five
	 *    cells; this is the case that matters, since it is what typing
	 *    at a shell prompt looks like.
	 */
	tty_puts("style");
	if (!tty_cursor_is("tty-cursor", 7 * TTY_COLS + 17,
	    "after five printable bytes"))
		return;

	/* 3. A newline returns to column zero of the next row. */
	tty_putc('\n');
	if (!tty_cursor_is("tty-cursor", 8 * TTY_COLS, "after a newline"))
		return;

	/*
	 * 4. A scroll.  The rows move up under the cursor and the cursor
	 *    stays on the bottom row -- a case the blitter handles in
	 *    tty_scroll rather than in the character path, so it is a
	 *    genuinely separate way to get this wrong.
	 */
	tty_puts("\x1b[25;1H");
	if (!tty_cursor_is("tty-cursor", (TTY_ROWS - 1) * TTY_COLS,
	    "after CUP to the last row"))
		return;
	tty_putc('\n');
	if (!tty_cursor_is("tty-cursor", (TTY_ROWS - 1) * TTY_COLS,
	    "after scrolling"))
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
	if (!tty_cursor_is("tty-cursor", 11 * TTY_COLS + 33 + 15,
	    "after a batched write"))
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
	if (!tty_cursor_is("tty-cursor", 0, "after clearing the screen"))
		return;

	kprintf("tty-cursor: PASS -- the CRTC followed the text through "
	    "positioning, typing, a newline, a scroll and a clear; a 23-byte "
	    "write cost one programming and a move to where it already was "
	    "cost none\n");
}

/*
 * Read a cell back off the screen.  The wrap test needs to know WHERE a
 * character landed, which the cursor position alone cannot say: a
 * blitter that wrapped early and one that wrapped late can leave the
 * cursor in the same place having written to different rows.
 */
static char
tty_glyph_at(uint16_t row, uint16_t col)
{

	return ((char)(VGA_BASE[(size_t)row * TTY_COLS + col] & 0xFFu));
}

static void
tty_fill_row(uint16_t row, uint16_t n, char ch)
{
	uint16_t	i;

	kprintf("\x1b[%u;1H", (unsigned)(row + 1));
	tty_batch_begin();
	for (i = 0; i < n; i++)
		tty_putc(ch);
	tty_batch_end();
}

void
tty_wrap_selftest(void)
{
	uint16_t	want;

	/*
	 * 1. Eighty characters fill a row and leave the cursor ON it.
	 *
	 *    The old blitter advanced the column past the eightieth
	 *    character and wrapped there and then, so the cursor was
	 *    already on the next row with nothing written to it.  That is
	 *    the bug: the row had been spent before anything asked for it.
	 */
	tty_fill_row(5, TTY_COLS, 'x');
	want = 5 * TTY_COLS + (TTY_COLS - 1);
	if (!tty_cursor_is("tty-wrap", want, "after filling a row exactly"))
		return;

	/*
	 * 2. And the newline after it costs ONE row, not two.  This is the
	 *    check the shell's splash was failing: an eighty-column status
	 *    line plus a newline landed two rows down, so the rule that
	 *    followed wiped the text a row below where it belonged.
	 */
	tty_putc('\n');
	if (!tty_cursor_is("tty-wrap", 6 * TTY_COLS,
	    "after the newline that follows it"))
		return;

	/*
	 * 3. The eighty-first character does wrap -- deferring it must not
	 *    mean losing it.  Check the glyph, not the cursor: a blitter
	 *    that overwrote column eighty instead of wrapping would leave
	 *    the cursor in a defensible place having eaten a character.
	 */
	tty_fill_row(8, TTY_COLS, 'y');
	tty_puts("Z");
	if (tty_glyph_at(8, TTY_COLS - 1) != 'y') {
		tty_clear();
		kprintf("tty-wrap: FAIL the eighty-first character overwrote "
		    "the eightieth instead of wrapping\n");
		return;
	}
	if (tty_glyph_at(9, 0) != 'Z') {
		tty_clear();
		kprintf("tty-wrap: FAIL the eighty-first character did not "
		    "land on the next row (found '%c' there)\n",
		    tty_glyph_at(9, 0));
		return;
	}
	if (!tty_cursor_is("tty-wrap", 9 * TTY_COLS + 1,
	    "after the character that wrapped"))
		return;

	/*
	 * 4. A carriage return disarms the wrap rather than paying it: the
	 *    next character belongs at column one of the SAME row.  This
	 *    is what a shell does when it repaints a line it has just
	 *    filled to the margin.
	 */
	tty_fill_row(11, TTY_COLS, 'w');
	tty_puts("\rW");
	if (tty_glyph_at(11, 0) != 'W' || tty_glyph_at(12, 0) == 'W') {
		tty_clear();
		kprintf("tty-wrap: FAIL a carriage return after a full row "
		    "did not cancel the pending wrap\n");
		return;
	}

	/*
	 * 5. And filling the bottom row does not scroll by itself -- the
	 *    screen moves when the next character needs somewhere to go,
	 *    not a character earlier.  Getting this wrong loses the last
	 *    line of any output that happens to be exactly full width.
	 */
	tty_fill_row(TTY_ROWS - 1, TTY_COLS, 'b');
	if (tty_glyph_at(TTY_ROWS - 1, 0) != 'b') {
		tty_clear();
		kprintf("tty-wrap: FAIL filling the bottom row scrolled the "
		    "screen before anything needed the next one\n");
		return;
	}

	tty_clear();
	kprintf("tty-wrap: PASS -- a full row leaves the cursor in the last "
	    "column, the newline after it costs one row, the character after "
	    "it wraps without being lost, a carriage return cancels the wrap, "
	    "and a full bottom row does not scroll on its own\n");
}

/*
 * Push the screen up by `n` rows from inside the region, the way a
 * command's output does: sit on the last row and emit newlines.
 */
static void
tty_push(uint16_t n)
{
	uint16_t	i;

	kprintf("\x1b[%u;1H", (unsigned)TTY_ROWS);
	tty_batch_begin();
	for (i = 0; i < n; i++)
		tty_putc('\n');
	tty_batch_end();
}

void
tty_region_selftest(void)
{
	uint8_t	reg;

	/*
	 * 1. A header above the top margin survives a screenful of
	 *    scrolling.
	 *
	 *    This is the claim that matters, and it is the exact shape of
	 *    the bug that started this: sh.elf painted a status bar on row
	 *    zero, and the first command whose output reached the bottom
	 *    of the screen carried the bar off the top.  Thirty newlines
	 *    is more than a screenful, so a console that scrolls
	 *    everything cannot possibly still have the header.
	 */
	tty_clear();
	tty_fill_row(0, 4, 'H');
	tty_puts("\x1b[3;25r");
	tty_push(TTY_ROWS + 5);
	if (tty_glyph_at(0, 0) != 'H' || tty_glyph_at(0, 3) != 'H') {
		tty_puts("\x1b[r");
		tty_clear();
		kprintf("tty-region: FAIL the header above the top margin was "
		    "scrolled away\n");
		return;
	}

	/*
	 * 2. And the region really did scroll -- a header that survives
	 *    because nothing moved at all would pass the check above and
	 *    mean the opposite of what it claims.
	 */
	tty_puts("\x1b[3;1Hmark");
	tty_push(3);
	if (tty_glyph_at(2, 0) == 'm') {
		tty_puts("\x1b[r");
		tty_clear();
		kprintf("tty-region: FAIL nothing scrolled inside the region "
		    "either -- the header survived a console that had "
		    "stopped moving\n");
		return;
	}

	/*
	 * 3. Row two -- the last row above the margin -- is untouched too,
	 *    so the frozen area is the whole area asked for and not just
	 *    its first row.
	 */
	tty_fill_row(1, 4, 'G');
	tty_push(TTY_ROWS + 5);
	if (tty_glyph_at(1, 0) != 'G') {
		tty_puts("\x1b[r");
		tty_clear();
		kprintf("tty-region: FAIL only the first frozen row was "
		    "actually frozen\n");
		return;
	}

	/*
	 * 4. Giving the region back makes the top rows ordinary again.  A
	 *    console that froze rows and could not thaw them would be a
	 *    worse bargain than one that never froze any.
	 */
	tty_puts("\x1b[r");
	tty_push(TTY_ROWS + 5);
	if (tty_glyph_at(0, 0) == 'H') {
		tty_clear();
		kprintf("tty-region: FAIL the top rows stayed frozen after "
		    "the region was reset\n");
		return;
	}

	/*
	 * 5. DECTCEM reaches the hardware.  Read the cursor-start register
	 *    back and look at the disable bit -- asking the driver whether
	 *    it thinks the cursor is hidden would pass with the port write
	 *    missing entirely.
	 */
	tty_puts("\x1b[?25l");
	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_START);
	reg = inb(VGA_CRTC_DATA);
	if ((reg & VGA_CURSOR_DISABLE) == 0) {
		tty_puts("\x1b[?25h");
		tty_clear();
		kprintf("tty-region: FAIL hiding the cursor left the CRTC "
		    "disable bit clear (register 10 = 0x%x)\n", (unsigned)reg);
		return;
	}
	tty_puts("\x1b[?25h");
	outb(VGA_CRTC_INDEX, VGA_CRTC_CURSOR_START);
	reg = inb(VGA_CRTC_DATA);
	if ((reg & VGA_CURSOR_DISABLE) != 0) {
		tty_clear();
		kprintf("tty-region: FAIL showing the cursor again left it "
		    "disabled (register 10 = 0x%x)\n", (unsigned)reg);
		return;
	}

	tty_clear();
	kprintf("tty-region: PASS -- rows above the top margin survived two "
	    "screenfuls of scrolling while the region underneath moved, "
	    "resetting the margins thawed them, and DECTCEM reached the "
	    "CRTC disable bit\n");
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

	/*
	 * Only the rows between the margins move.  With the margins at
	 * their defaults that is the whole screen, so this is the same
	 * scroll it always was -- until a program asks for a header.
	 */
	top = (size_t)tty_scroll_top * TTY_COLS;
	bot = (size_t)tty_scroll_bot * TTY_COLS;

	for (i = top; i < bot; i++)
		VGA_BASE[i] = VGA_BASE[i + TTY_COLS];

	blank = tty_cell(' ');
	for (i = bot; i < bot + TTY_COLS; i++)
		VGA_BASE[i] = blank;

	tty_row = tty_scroll_bot;
}

/*
 * Move down one row, scrolling only if the cursor is standing on the
 * bottom margin.
 *
 * A cursor below the region -- which can happen, since CUP addresses
 * the screen rather than the region -- walks down to the last row and
 * stops there.  That is what a VT does, and it means a program that
 * paints outside the region cannot scroll the region by accident.
 */
static void
tty_linefeed(void)
{

	if (tty_row == tty_scroll_bot)
		tty_scroll();
	else if (tty_row + 1 < TTY_ROWS)
		tty_row++;
}

static uint16_t
tty_cell(char ch)
{

	return ((uint16_t)((uint16_t)tty_attr << 8) | (uint8_t)ch);
}
