/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "dbgcon.h"
#include "intr.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "tty.h"
#include "uart.h"

#define	VGA_BASE	((volatile uint16_t *)0x000B8000)
#define	VGA_CELLS	((size_t)TTY_COLS * TTY_ROWS)

/*
 * The hardware text cursor.  The CRT controller draws the underline at a
 * 16-bit cell offset held in two indexed registers (write the register
 * number to VGA_CRTC_INDEX, then the byte to VGA_CRTC_DATA), and it stays
 * where it was last told.  The offset is the blitter's own cell index.
 */
#define	VGA_CRTC_INDEX		0x3D4
#define	VGA_CRTC_DATA		0x3D5
#define	VGA_CRTC_CURSOR_START	10	/* bit 5 turns the cursor off */
#define	VGA_CRTC_CURSOR_HI	14
#define	VGA_CRTC_CURSOR_LO	15
#define	VGA_CURSOR_DISABLE	0x20u

/*
 * The palette.  An attribute nibble names a slot, not a colour: it indexes
 * the attribute controller's palette registers, which index the DAC,
 * which holds six-bit-per-channel RGB.  Both levels are written at init,
 * replacing the CGA defaults.
 *
 * The attribute controller's mode register has a blink-enable bit, set at
 * reset, that makes the background's high bit mean blink rather than
 * bright; clearing it makes all sixteen backgrounds reachable.
 *
 * Writes to attribute registers 0x00-0x0F are ignored while the
 * palette-address-source bit is set, and that bit also keeps video on, so
 * the palette is written with video off, once, before anything is shown.
 */
#define	VGA_DAC_WRITE_INDEX	0x3C8
#define	VGA_DAC_READ_INDEX	0x3C7
#define	VGA_DAC_DATA		0x3C9
#define	VGA_AC_INDEX		0x3C0
#define	VGA_AC_DATA_READ	0x3C1
#define	VGA_INPUT_STATUS_1	0x3DA
#define	VGA_AC_MODE		0x10
#define	VGA_AC_MODE_BLINK	0x08u
#define	VGA_AC_PALETTE_ENABLE	0x20u

/*
 * A calm, low-contrast set on near-black; body gray (7) and chrome gray
 * (8) sit a clear step apart.  Eight-bit values, cut to the DAC's six
 * bits on the way in.
 */
static const uint8_t	tty_rgb[16][3] = {
	{ 0x0d, 0x0d, 0x0f },	/*  0 black          */
	{ 0x3b, 0x6e, 0xa5 },	/*  1 blue           */
	{ 0x5a, 0x9e, 0x5a },	/*  2 green          */
	{ 0x3f, 0x9c, 0x99 },	/*  3 cyan           */
	{ 0xb0, 0x52, 0x52 },	/*  4 red            */
	{ 0x8e, 0x6b, 0xb0 },	/*  5 magenta        */
	{ 0xa8, 0x82, 0x3c },	/*  6 brown          */
	{ 0xb4, 0xb6, 0xba },	/*  7 light gray     */
	{ 0x4a, 0x4d, 0x52 },	/*  8 dark gray      */
	{ 0x6f, 0xa8, 0xdc },	/*  9 bright blue    */
	{ 0x86, 0xc4, 0x6e },	/* 10 bright green   */
	{ 0x63, 0xc9, 0xc3 },	/* 11 bright cyan    */
	{ 0xe0, 0x8b, 0x7f },	/* 12 bright red     */
	{ 0xb9, 0x8f, 0xd9 },	/* 13 bright magenta */
	{ 0xdf, 0xc0, 0x7a },	/* 14 bright yellow  */
	{ 0xee, 0xf0, 0xf2 },	/* 15 white          */
};

/*
 * ANSI/VT CSI state machine:
 *
 *	GROUND	printable bytes go to the cell grid; ESC starts a sequence.
 *	ESC	saw 0x1B.  "ESC [" enters CSI and "ESC c" (RIS) resets;
 *		anything else returns to GROUND.
 *	CSI	accumulating parameters (digits and ';'), with an optional
 *		leading '?' for DEC private sequences.  A final byte
 *		(0x40..0x7E) dispatches and returns to GROUND.
 *
 * The mirrors (uart, dbgcon) get the raw bytes, so a terminal on COM1
 * interprets the same sequences; only the VGA blitter parses.
 *
 * Final bytes implemented:
 *	H / f	CUP -- cursor position, 1-based (row;col)
 *	A B C D	CUU/CUD/CUF/CUB -- cursor up/down/right/left N
 *	J	ED  -- erase in display: 0=>EOS, 1=>start->cur, 2=screen
 *	K	EL  -- erase in line:    same parameter scheme
 *	m	SGR -- 0 1 2 7 22 27 39 49, 30..37 40..47 90..97 100..107,
 *		38/48 with ;5;N or ;2;R;G;B
 *	r	DECSTBM -- top and bottom margins of the scrolling region
 *	s / u	save / restore cursor
 *	?25 h/l	DECTCEM -- show / hide the cursor
 *
 * Unknown finals are dropped silently.  There is no DECOM (origin mode):
 * CUP always addresses the screen, not the scrolling region.
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
 * DEC's last-column flag: a character written to the last column leaves
 * the cursor there and arms this, and only the next printable character
 * moves to the next row.  Anything that positions the cursor (CR, LF,
 * BS, tab, the moving and erasing CSIs) disarms it without wrapping.  So
 * a full-width line followed by a newline costs one row, not two.
 */
static bool		tty_wrap_pending;

/*
 * The scrolling region (DECSTBM): only the rows between the margins
 * scroll, so a program can keep a status bar above the top margin while
 * its output scrolls below.  Inclusive, zero-based, the whole screen by
 * default.
 */
static uint16_t		tty_scroll_top;
static uint16_t		tty_scroll_bot;

/*
 * Whether the underline is drawn (DECTCEM).  A full-screen program hides
 * it across a repaint.
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

/*
 * The console lock.  The unit of exclusion is a write, not a character,
 * so that lines from different CPUs do not interleave byte by byte.
 * kprintf brackets a run and calls tty_putc for every byte inside it, and
 * a kprintf nested in another is ordinary, so the lock is recursive per
 * CPU -- an owner and a depth, which struct spinlock (panicking on a
 * same-CPU re-acquire) cannot be.  Another CPU still waits.
 *
 * Interrupts are off while it is held, as for a spinlock: a handler that
 * printed while its own CPU held the console would wait forever.
 *
 * The cost: interrupts stay off for a whole line, up to eighty of
 * uart_send_raw's polled waits.  Free under an emulator; on a real
 * 115200-baud line about 7 ms per line.  The remedy would be an output
 * ring drained by the UART's own interrupt, not built.
 */
#define	CONS_NOBODY		0xFFFFFFFFu

static volatile uint32_t	cons_owner = CONS_NOBODY;	/* (a) cpu id  */
static uint32_t			cons_depth;	/* (cons) nested brackets  */
static bool			cons_saved_if;	/* (cons) IF at the outer  */
static uint64_t			cons_waits;	/* (a) found it held       */

static void	cons_enter(void);
static void	cons_exit(void);

/*
 * Take the console, wait for it, or find that this CPU already has it.
 * During a panic nothing is taken: the panicking code may be the holder.
 * cons_exit releases only what this CPU owns, so the bypass needs no flag.
 */
static void
cons_enter(void)
{
	uint32_t	me;
	uint32_t	vacant;
	bool		was_on;

	if (panic_in_progress)
		return;

	was_on = intr_save_disable();
	me = (uint32_t)cpu_id();

	if (__atomic_load_n(&cons_owner, __ATOMIC_RELAXED) == me) {
		cons_depth++;
		return;
	}

	vacant = CONS_NOBODY;
	if (!__atomic_compare_exchange_n(&cons_owner, &vacant, me, false,
	    __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) {
		__atomic_fetch_add(&cons_waits, 1, __ATOMIC_RELAXED);
		do {
			/*
			 * Interrupts are off, so answer TLB-shootdown requests
			 * from inside the wait, as spin_lock does; the sender
			 * is waiting for us.
			 */
			pmap_tlb_poll();
			__asm__ __volatile__ ("pause");
			vacant = CONS_NOBODY;
		} while (!__atomic_compare_exchange_n(&cons_owner, &vacant, me,
		    false, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED));
	}

	cons_depth    = 1;
	cons_saved_if = was_on;
}

static void
cons_exit(void)
{
	bool	was_on;

	/*
	 * Not ours means nothing to release (the panic bypass, or an
	 * unbalanced end).  The unlocked read is safe: only this CPU can
	 * have written its own id there.
	 */
	if (__atomic_load_n(&cons_owner, __ATOMIC_RELAXED) != (uint32_t)cpu_id())
		return;
	if (--cons_depth != 0)
		return;

	was_on = cons_saved_if;
	__atomic_store_n(&cons_owner, CONS_NOBODY, __ATOMIC_RELEASE);
	intr_restore(was_on);
}

/*
 * What the CRTC was last told, and how often.  Programming the cursor is
 * four ISA-timed port writes (about 8 us), and doing it per character
 * cost 420 ms of a 4.4 s boot.  So it is done once per write: tty_puts,
 * tty_write and kvprintf bracket themselves with tty_batch_begin/end, a
 * move inside a batch only marks the cursor dirty, and the outermost
 * bracket programs it once.  A move to where the CRTC already points costs
 * nothing, which covers the bytes of a CSI sequence.  tty_stats reports
 * the counts.
 */
static uint16_t		tty_cursor_at;		/* (cons) offset in the CRTC */
static bool		tty_cursor_dirty;	/* (cons) moved in a batch   */
static uint32_t		tty_batch_depth;	/* (cons) open brackets      */
static uint64_t		tty_cursor_pokes;	/* (cons) */
static uint64_t		tty_batches;		/* (cons) */
static uint64_t		tty_chars;		/* (cons) */

static void	tty_putcell(uint16_t, uint16_t, char);
static void	tty_cursor_sync(void);
static void	tty_cursor_program(uint16_t);
static void	tty_cursor_show(bool);
static void	tty_palette_install(void);
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
	/* Before the first cell is written. */
	tty_palette_install();
	csi_state        = TTY_S_GROUND;
	tty_csi_reset();
	/* Force the write, whatever the firmware left in the register. */
	tty_cursor_visible = false;
	tty_cursor_show(true);
	/*
	 * 0xFFFF is no cell of an 80x25 screen, so tty_clear's move home
	 * cannot be skipped as "already there".
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

	/*
	 * A lone character is its own write; inside a batch the console is
	 * already this CPU's and this costs a compare and a counter.
	 */
	cons_enter();

	/* The mirrors get the raw bytes, escape sequences intact. */
	dbgcon_putc(ch);
	uart_putc(ch);

	tty_putc_vga(ch);
	tty_chars++;
	tty_cursor_sync();

	cons_exit();
}

/* ---- hardware cursor ------------------------------------------------- */

/*
 * Tell the CRTC where the cursor is, if it does not already know.  Called
 * at the tail of every tty_putc (and from tty_clear) rather than from each
 * place that moves tty_row / tty_col; the cached offset makes redundant
 * calls free.
 */
static void
tty_cursor_sync(void)
{
	uint16_t	off;

	off = (uint16_t)((size_t)tty_row * TTY_COLS + tty_col);
	if (off == tty_cursor_at) {
		/*
		 * Back where the hardware points (a save/restore pair, a
		 * backspace and reprint): nothing left to program.
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
 * Open and close a run of output, holding the console for its duration:
 * the cursor is programmed once at the end, and every tty_putc inside
 * finds the console already held by this CPU.  Everything the blitter
 * keeps -- cursor, parser state, attribute, counters -- is under the
 * console lock, so two CPUs cannot interleave inside one escape sequence.
 *
 * A leaked depth only leaves the underline stale; a leaked cons_enter
 * would be worse, so the two are opened and closed together and nowhere
 * else.
 */
void
tty_batch_begin(void)
{

	cons_enter();
	tty_batch_depth++;
	tty_batches++;
}

void
tty_batch_end(void)
{

	if (tty_batch_depth > 0)
		tty_batch_depth--;
	if (tty_batch_depth == 0 && tty_cursor_dirty)
		tty_cursor_program((uint16_t)((size_t)tty_row * TTY_COLS +
		    tty_col));
	cons_exit();
}

/* ---- palette --------------------------------------------------------- */

/*
 * Attribute-controller access.  Each first resets the shared index/data
 * flip-flop by reading input status 1: another writer may have left it in
 * the data position, and a write to the wrong half programs the wrong
 * register.
 */
static void
ac_write(uint8_t index, uint8_t value)
{

	(void)inb(VGA_INPUT_STATUS_1);
	outb(VGA_AC_INDEX, index);	/* enable bit clear: 0x00-0x0F writable */
	outb(VGA_AC_INDEX, value);
}

static uint8_t
ac_read(uint8_t index)
{

	(void)inb(VGA_INPUT_STATUS_1);
	outb(VGA_AC_INDEX, (uint8_t)(index | VGA_AC_PALETTE_ENABLE));
	return (inb(VGA_AC_DATA_READ));
}

static void
tty_palette_install(void)
{
	uint8_t	i;
	uint8_t	mode;

	/*
	 * Make attribute N select DAC entry N.  The reset mapping sends the
	 * sixteen text colours to DAC entries 0-7 and 0x38-0x3F.
	 */
	for (i = 0; i < 16; i++)
		ac_write(i, i);

	mode = ac_read(VGA_AC_MODE);
	ac_write(VGA_AC_MODE, (uint8_t)(mode & ~VGA_AC_MODE_BLINK));

	/* Video on again: the writes above were made with it off. */
	(void)inb(VGA_INPUT_STATUS_1);
	outb(VGA_AC_INDEX, VGA_AC_PALETTE_ENABLE);

	/*
	 * The DAC auto-increments: write the index once, then three
	 * channels per entry, six bits each.
	 */
	outb(VGA_DAC_WRITE_INDEX, 0);
	for (i = 0; i < 16; i++) {
		outb(VGA_DAC_DATA, (uint8_t)(tty_rgb[i][0] >> 2));
		outb(VGA_DAC_DATA, (uint8_t)(tty_rgb[i][1] >> 2));
		outb(VGA_DAC_DATA, (uint8_t)(tty_rgb[i][2] >> 2));
	}
}

/*
 * Show or hide the underline.  The cursor-start register holds the
 * glyph's top scan line in its low bits and the disable bit at 0x20, so
 * this is a read-modify-write that keeps the shape.
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
 * Read the offset back from the hardware, for the selftests: the cached
 * tty_cursor_at is what the driver believes it wrote, which a test of the
 * write must not consult.  Leaves the CRTC index at the low byte, which
 * is harmless since every writer sets the index first.
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
	/* How often a write had to wait for another CPU's. */
	kprintf("tty: %llu write(s) waited for the console -- %llu per 1000\n",
	    (unsigned long long)cons_waits,
	    (unsigned long long)(tty_batches == 0 ? 0 :
	    cons_waits * 1000 / tty_batches));
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
 * Blank cells lo..hi inclusive, clamped to the framebuffer so a malformed
 * CSI cannot write past it.
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
		/* ESC c -- terminal reset (RIS). */
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
		 * A tab never wraps: it stops at the last column, as on a VT,
		 * and landing there arms the wrap as printing there would.
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
			/* The wrap owed by the previous character, paid now. */
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

/*
 * ANSI counts its colours red-first, VGA blue-first:
 *
 *	ANSI	0 black 1 red  2 green 3 yellow 4 blue 5 magenta 6 cyan
 *	VGA	0 black 1 blue 2 green 3 cyan   4 red  5 magenta 6 brown
 *
 * The orders differ only in bits 0 and 2 being swapped.
 */
static uint8_t
tty_ansi_to_vga(uint8_t n)
{

	return ((uint8_t)((n & 0x02u) | ((n & 0x01u) << 2) |
	    ((n & 0x04u) >> 2)));
}

/*
 * The nearest of our sixteen to an arbitrary RGB, for 24-bit and
 * 256-colour SGR, which would otherwise leave whatever colour was set.
 * Squared RGB distance is not perceptual, but it keeps a red red.
 */
static uint8_t
tty_nearest(uint8_t r, uint8_t g, uint8_t b)
{
	uint32_t	best;
	uint8_t		best_i;
	uint8_t		i;

	best   = 0xFFFFFFFFu;
	best_i = 7;
	for (i = 0; i < 16; i++) {
		int32_t		dr, dg, db;
		uint32_t	d;

		dr = (int32_t)r - (int32_t)tty_rgb[i][0];
		dg = (int32_t)g - (int32_t)tty_rgb[i][1];
		db = (int32_t)b - (int32_t)tty_rgb[i][2];
		d  = (uint32_t)(dr * dr + dg * dg + db * db);
		if (d < best) {
			best   = d;
			best_i = i;
		}
	}
	return (best_i);
}

/*
 * xterm's 256 colours, unpacked to RGB for tty_nearest: 0-15 are the
 * sixteen we have, 16-231 a 6x6x6 cube with xterm's non-linear levels,
 * 232-255 a grey ramp.  Anything reading a modern terminfo speaks it.
 */
static uint8_t
tty_from_256(uint16_t n)
{
	static const uint8_t	cube[6] = {
		0x00, 0x5f, 0x87, 0xaf, 0xd7, 0xff
	};
	uint8_t			v;

	if (n < 8)
		return (tty_ansi_to_vga((uint8_t)n));
	if (n < 16)
		return ((uint8_t)(tty_ansi_to_vga((uint8_t)(n - 8)) | 0x08u));
	if (n < 232) {
		n -= 16;
		return (tty_nearest(cube[(n / 36) % 6], cube[(n / 6) % 6],
		    cube[n % 6]));
	}
	if (n > 255)
		return (7);
	v = (uint8_t)(8 + (n - 232) * 10);
	return (tty_nearest(v, v, v));
}

static void
csi_apply_sgr(void)
{
	uint8_t	fg;
	uint8_t	bg;
	bool	reverse;
	uint8_t	i;

	/*
	 * Split the attribute into fg and bg, four bits each (blink is off,
	 * so the background's high bit is a colour, not blink).
	 */
	fg      = (uint8_t)(tty_attr & 0x0Fu);
	bg      = (uint8_t)((tty_attr >> 4) & 0x0Fu);
	reverse = false;

	if (csi_nparam == 0) {
		/* CSI m == CSI 0 m -- reset to default. */
		tty_attr = tty_attr_default;
		return;
	}

	for (i = 0; i < csi_nparam; i++) {
		uint16_t	p = csi_params[i];

		if (p == 0) {
			fg      = (uint8_t)(tty_attr_default & 0x0Fu);
			bg      = (uint8_t)((tty_attr_default >> 4) & 0x0Fu);
			reverse = false;
			continue;
		}
		if (p == 1) {
			/* Bold == bright bit on VGA fg. */
			fg = (uint8_t)(fg | 0x08u);
			continue;
		}
		if (p == 2) {
			/* Faint: no half intensity here, so "not bold". */
			fg = (uint8_t)(fg & 0x07u);
			continue;
		}
		if (p == 7) {
			reverse = true;
			continue;
		}
		if (p == 22) {
			fg = (uint8_t)(fg & 0x07u);
			continue;
		}
		if (p == 27) {
			reverse = false;
			continue;
		}
		if (p == 39) {
			fg = (uint8_t)(tty_attr_default & 0x0Fu);
			continue;
		}
		if (p == 49) {
			bg = (uint8_t)((tty_attr_default >> 4) & 0x0Fu);
			continue;
		}
		if (p >= 30 && p <= 37) {
			fg = (uint8_t)((fg & 0x08u) |
			    tty_ansi_to_vga((uint8_t)(p - 30u)));
			continue;
		}
		if (p >= 40 && p <= 47) {
			bg = (uint8_t)((bg & 0x08u) |
			    tty_ansi_to_vga((uint8_t)(p - 40u)));
			continue;
		}
		if (p >= 90 && p <= 97) {
			fg = (uint8_t)(tty_ansi_to_vga((uint8_t)(p - 90u)) |
			    0x08u);
			continue;
		}
		if (p >= 100 && p <= 107) {
			bg = (uint8_t)(tty_ansi_to_vga((uint8_t)(p - 100u)) |
			    0x08u);
			continue;
		}
		/*
		 * Extended colour: "38;5;N" / "48;5;N" (256 colours) and
		 * "38;2;R;G;B" / "48;2;R;G;B" (24-bit).  The arguments are
		 * consumed here, not left to the loop, which would read
		 * "48;2;0;0;0" as a 0 (reset) and more.
		 */
		if (p == 38 || p == 48) {
			bool	is_fg = (p == 38);
			uint8_t	c;

			if (i + 2 < csi_nparam && csi_params[i + 1] == 5) {
				c = tty_from_256(csi_params[i + 2]);
				i = (uint8_t)(i + 2);
			} else if (i + 4 < csi_nparam &&
			    csi_params[i + 1] == 2) {
				c = tty_nearest((uint8_t)csi_params[i + 2],
				    (uint8_t)csi_params[i + 3],
				    (uint8_t)csi_params[i + 4]);
				i = (uint8_t)(i + 4);
			} else {
				/*
				 * Malformed: drop the rest of the list,
				 * whose values are this sequence's
				 * arguments, not commands.
				 */
				break;
			}
			if (is_fg)
				fg = c;
			else
				bg = c;
			continue;
		}
		/* Unknown -- drop. */
	}

	/*
	 * Reverse is applied once, to the result: a colour set after the
	 * 7 in the same run is still the one swapped.
	 */
	if (reverse)
		tty_attr = TTY_ATTR(bg, fg);
	else
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
	 * DEC private sequences: only DECTCEM is honoured; the rest (keypad
	 * mode, bracketed paste probes) are dropped.
	 */
	if (csi_private) {
		if ((final == 'h' || final == 'l') && csi_nparam == 1 &&
		    csi_params[0] == 25)
			tty_cursor_show(final == 'h');
		return;
	}

	/*
	 * Cursor moves and erases disarm the deferred wrap.  SGR and
	 * save-cursor do not: a colour change between the 80th and 81st
	 * characters must not turn the wrap into an overwrite.
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
		 *        3: as 2 (there is no scrollback).
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
		 * DECSTBM, one-based and inclusive.  An empty or inverted
		 * region means the whole screen, as on a VT.  The cursor goes
		 * to the screen's top-left (no origin mode).
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
		 * The armed wrap is part of the position: in the last column
		 * it decides which row the next character goes to.
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
 * Every check compares tty_cursor_hw(), read from the CRTC, with a
 * position worked out by hand -- never with tty_cursor_cell(), which would
 * pass with the CRTC never written at all.
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

	/* 1. Absolute positioning (CUP); nothing else holds if this fails. */
	tty_puts("\x1b[8;13H");
	if (!tty_cursor_is("tty-cursor", 7 * TTY_COLS + 12,
	    "after CUP to row 8 column 13"))
		return;

	/* 2. Ordinary text: five printable bytes move it five cells. */
	tty_puts("style");
	if (!tty_cursor_is("tty-cursor", 7 * TTY_COLS + 17,
	    "after five printable bytes"))
		return;

	/* 3. A newline returns to column zero of the next row. */
	tty_putc('\n');
	if (!tty_cursor_is("tty-cursor", 8 * TTY_COLS, "after a newline"))
		return;

	/*
	 * 4. A scroll leaves the cursor on the bottom row (tty_scroll, a
	 *    separate path from the character one).
	 */
	tty_puts("\x1b[25;1H");
	if (!tty_cursor_is("tty-cursor", (TTY_ROWS - 1) * TTY_COLS,
	    "after CUP to the last row"))
		return;
	tty_putc('\n');
	if (!tty_cursor_is("tty-cursor", (TTY_ROWS - 1) * TTY_COLS,
	    "after scrolling"))
		return;

	/* 5. A batched write programs the CRTC once, not per character. */
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

	/* 6. A move to where the hardware already points costs nothing. */
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
 * Read a cell back off the screen: the wrap test needs where a character
 * landed, which the cursor alone cannot say.
 */
static char
tty_glyph_at(uint16_t row, uint16_t col)
{

	return ((char)(VGA_BASE[(size_t)row * TTY_COLS + col] & 0xFFu));
}

static uint8_t
tty_attr_at(uint16_t row, uint16_t col)
{

	return ((uint8_t)(VGA_BASE[(size_t)row * TTY_COLS + col] >> 8));
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

	/* 1. Eighty characters fill a row and leave the cursor on it. */
	tty_fill_row(5, TTY_COLS, 'x');
	want = 5 * TTY_COLS + (TTY_COLS - 1);
	if (!tty_cursor_is("tty-wrap", want, "after filling a row exactly"))
		return;

	/* 2. The newline after it costs one row, not two. */
	tty_putc('\n');
	if (!tty_cursor_is("tty-wrap", 6 * TTY_COLS,
	    "after the newline that follows it"))
		return;

	/*
	 * 3. The 81st character does wrap.  Checked by glyph: overwriting
	 *    column 80 could leave the cursor in a plausible place.
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
	 * 4. A carriage return disarms the wrap: the next character goes to
	 *    column one of the same row, as when a shell repaints a full line.
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
	 * 5. Filling the bottom row does not scroll by itself; the screen
	 *    moves only when the next character needs the room.
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

/* Scroll by `n' rows as command output does: newlines on the last row. */
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
	 * 1. A header above the top margin survives more than a screenful
	 *    of scrolling.
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

	/* 2. And the region really did scroll, or check 1 proves nothing. */
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

	/* 3. Row two, the last above the margin, is frozen too. */
	tty_fill_row(1, 4, 'G');
	tty_push(TTY_ROWS + 5);
	if (tty_glyph_at(1, 0) != 'G') {
		tty_puts("\x1b[r");
		tty_clear();
		kprintf("tty-region: FAIL only the first frozen row was "
		    "actually frozen\n");
		return;
	}

	/* 4. Resetting the region makes the top rows scroll again. */
	tty_puts("\x1b[r");
	tty_push(TTY_ROWS + 5);
	if (tty_glyph_at(0, 0) == 'H') {
		tty_clear();
		kprintf("tty-region: FAIL the top rows stayed frozen after "
		    "the region was reset\n");
		return;
	}

	/*
	 * 5. DECTCEM reaches the hardware: read the disable bit back from
	 *    the cursor-start register, not from the driver.
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

/*
 * Write one character at a known cell under the given SGR run and return
 * the attribute byte actually stored.
 */
static uint8_t
tty_sgr_probe(const char *sgr)
{

	tty_puts("\x1b[20;1H");
	tty_puts(sgr);
	tty_puts("#");
	tty_puts("\x1b[0m");
	return (tty_attr_at(19, 0));
}

void
tty_colour_selftest(void)
{
	uint8_t	got;
	uint8_t	want;
	uint8_t	i;

	/* 1. The DAC holds our sixteen colours, read back off the card. */
	outb(VGA_DAC_READ_INDEX, 0);
	for (i = 0; i < 16; i++) {
		uint8_t	c;
		uint8_t	ch;

		for (c = 0; c < 3; c++) {
			ch   = inb(VGA_DAC_DATA);
			want = (uint8_t)(tty_rgb[i][c] >> 2);
			if (ch != want) {
				tty_clear();
				kprintf("tty-colour: FAIL DAC entry %u channel "
				    "%u reads 0x%x, the palette says 0x%x\n",
				    (unsigned)i, (unsigned)c,
				    (unsigned)ch, (unsigned)want);
				return;
			}
		}
	}

	/* 2. Blink is off, so the high background bit is a colour. */
	got = ac_read(VGA_AC_MODE);
	if ((got & VGA_AC_MODE_BLINK) != 0) {
		tty_clear();
		kprintf("tty-colour: FAIL the attribute controller still has "
		    "blink enabled (mode register = 0x%x)\n", (unsigned)got);
		return;
	}

	/* 3. And the attribute-to-DAC mapping is the identity. */
	for (i = 0; i < 16; i++) {
		got = (uint8_t)(ac_read(i) & 0x3Fu);
		if (got != i) {
			tty_clear();
			kprintf("tty-colour: FAIL attribute %u maps to DAC "
			    "entry %u, not to itself\n",
			    (unsigned)i, (unsigned)got);
			return;
		}
	}

	/* 4. SGR 107 reaches a bright background through the parser. */
	got = tty_sgr_probe("\x1b[107m");
	if (((got >> 4) & 0x0Fu) != 0x0Fu) {
		tty_clear();
		kprintf("tty-colour: FAIL SGR 107 left background nibble %u, "
		    "not 15\n", (unsigned)((got >> 4) & 0x0Fu));
		return;
	}

	/* 4b. Red is red and blue is blue, though ANSI and VGA swap them. */
	got = tty_sgr_probe("\x1b[31m");
	if ((got & 0x0Fu) != 4) {
		tty_clear();
		kprintf("tty-colour: FAIL SGR 31 (red) set VGA colour %u; red "
		    "is 4 here and %u is %s\n",
		    (unsigned)(got & 0x0Fu), (unsigned)(got & 0x0Fu),
		    (got & 0x0Fu) == 1 ? "blue -- ANSI order leaked through" :
		    "something else again");
		return;
	}
	got = tty_sgr_probe("\x1b[34m");
	if ((got & 0x0Fu) != 1) {
		tty_clear();
		kprintf("tty-colour: FAIL SGR 34 (blue) set VGA colour %u, "
		    "not 1\n", (unsigned)(got & 0x0Fu));
		return;
	}

	/* 5. Reverse video (SGR 7) swaps fg and bg. */
	got = tty_sgr_probe("\x1b[7m");
	if (got != TTY_ATTR((tty_attr_default >> 4) & 0x0Fu,
	    tty_attr_default & 0x0Fu)) {
		tty_clear();
		kprintf("tty-colour: FAIL SGR 7 gave attribute 0x%x; reversing "
		    "the default 0x%x should give 0x%x\n",
		    (unsigned)got, (unsigned)tty_attr_default,
		    (unsigned)TTY_ATTR((tty_attr_default >> 4) & 0x0Fu,
		    tty_attr_default & 0x0Fu));
		return;
	}

	/*
	 * 6. An extended-colour sequence is consumed whole: "48;2;0;0;0"
	 *    read one parameter at a time contains a 0 (reset).  It must
	 *    give background 0 with the requested foreground kept.
	 */
	got = tty_sgr_probe("\x1b[91;48;2;0;0;0m");
	if (((got >> 4) & 0x0Fu) != 0 || (got & 0x0Fu) != 0x0Cu) {
		tty_clear();
		kprintf("tty-colour: FAIL '91;48;2;0;0;0' gave fg %u bg %u, "
		    "not fg 12 bg 0\n",
		    (unsigned)(got & 0x0Fu), (unsigned)((got >> 4) & 0x0Fu));
		return;
	}

	/* 7. The 256-colour cube resolves to something sane: 196 is red. */
	got = tty_sgr_probe("\x1b[38;5;196m");
	if ((got & 0x0Fu) != 0x0Cu && (got & 0x0Fu) != 0x04u) {
		tty_clear();
		kprintf("tty-colour: FAIL 256-colour 196 (pure red) mapped to "
		    "palette entry %u\n", (unsigned)(got & 0x0Fu));
		return;
	}

	tty_clear();
	kprintf("tty-colour: PASS -- the DAC holds this kernel's sixteen "
	    "colours, blink is off so all sixteen backgrounds are colours, "
	    "reverse video swaps, and 256-colour and 24-bit sequences are "
	    "consumed whole instead of read as commands\n");
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

	/* Only the rows between the margins move (by default, all). */
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
 * Move down one row, scrolling only if the cursor is on the bottom margin.
 * A cursor below the region (CUP addresses the screen) walks down to the
 * last row and stops, as on a VT, so painting outside the region never
 * scrolls it.
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
