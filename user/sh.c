/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * sh.elf -- the ring-3 shell style9-os boots into (kern/shell.c, the
 * in-kernel REPL, is only a fallback in kmain).
 *
 *	1. bootstrap_lookup("dev/kbd") -> control port
 *	2. RPC DEV_OP_OPEN_STREAM       -> stream SEND right (kbd_input_port)
 *	3. for (;;) {
 *		recv one mach_msg from the stream, msgh_id == one byte;
 *		line-edit; on '\n' split argv, run a builtin (sh_builtins[])
 *		or SYS_SPAWN_ARGS, and wait for the child in wait_child.
 *	   }
 *
 * Rows 1-2 are a fixed status bar (name, task count, uptime) and a rule,
 * outside the scrolling region.  The session opens with a man-page-style
 * splash; the prompt is '$ ', preceded by 'err N' in red when the last
 * spawn failed.
 *
 * wait_child polls task_alive between 50 ms keyboard waits; there is no
 * exit-notification port and no $? from the child.
 */

#define	SH_LINE_MAX	256
#define	SH_ARGC_MAX	8

/*
 * What can be spawned, asked of the progreg service once at startup: a
 * packed NUL-separated blob, indexed in place.  A program added to
 * kern/progreg.c needs nothing here.
 */
static char	sh_progs[SVC_PROGREG_BYTES];
static uint32_t	sh_progs_n;	/* names packed into sh_progs      */
static uint32_t	sh_progs_total;	/* names the registry actually has */
static uint64_t	sh_progs_macho;	/* bit per name: a real Darwin one */

/*
 * The idx'th packed name, or NULL past the end.  O(n) per lookup with n
 * under forty; an index array would cost more than it saves.
 */
static const char *
prog_at(uint32_t idx)
{
	const char	*p;
	uint32_t	 i;

	p = sh_progs;
	for (i = 0; i < idx; i++) {
		while (*p != '\0')
			p++;
		p++;
		if (p >= sh_progs + sizeof(sh_progs))
			return (NULL);
	}
	return (i < sh_progs_n ? p : NULL);
}

/*
 * Service ports, looked up once at startup for the splash and status
 * bar.  MACH_PORT_NULL if the lookup failed; the fetch then zeroes the
 * reply.
 */
static mach_port_name_t	g_kbd_stream;
static mach_port_name_t	g_clock_port;
static mach_port_name_t	g_stats_port;

/*
 * Last command's status: 0, or the negative SYS_E_* a failed spawn
 * returned.  The prompt shows 'err N' when non-zero.
 */
static int	last_status;

/*
 * Child registry: a (task_id, task port) pair per spawned job, so the
 * shell holds the right SYS_TASK_KILL needs.  Rows are never evicted;
 * once all are used, slot 0 is overwritten (sh_child_remember).
 */
#define	SH_CHILD_MAX	16
struct sh_child {
	uint64_t		c_task_id;
	mach_port_name_t	c_taskport;
};
static struct sh_child	sh_children[SH_CHILD_MAX];

/*
 * The foreground job, set by dispatch() around wait_child().  One job at
 * a time: there is no job control.
 */
static long			fg_task_id;
static mach_port_name_t		fg_taskport;

/* ---- ANSI escape constants --------------------------------------- */

/*
 * Palette: bold white for emphasis, gray for body, dark gray for chrome,
 * light red for errors.
 */
#define	ESC_RESET	"\x1b[0m"
#define	ESC_FG_RED	"\x1b[0;91m"	/* light red, not bold */
#define	ESC_FG_WHITE	"\x1b[1;37m"	/* bold white -- headers, prompt */
#define	ESC_FG_GRAY	"\x1b[0;37m"	/* default body colour          */
#define	ESC_FG_DGRAY	"\x1b[1;30m"	/* dark gray -- rules, chrome   */
#define	ESC_FG_CYAN	"\x1b[0;36m"	/* Mach-O, in the program list  */
#define	ESC_CLR_SCR	"\x1b[2J\x1b[H"
#define	ESC_SAVE_CUR	"\x1b[s"
#define	ESC_REST_CUR	"\x1b[u"
#define	ESC_HOME	"\x1b[1;1H"
#define	ESC_HIDE_CUR	"\x1b[?25l"
#define	ESC_SHOW_CUR	"\x1b[?25h"

/*
 * The title bar: white on a dark-gray field, and a quieter foreground
 * for the counters.  Bright-background SGR codes, which need the tty not
 * to spend the high background bit on blink.
 */
#define	ESC_BAR		"\x1b[100;97m"
#define	ESC_BAR_DIM	"\x1b[100;37m"

/*
 * Rows 1-2 are the status line and its rule; rows 3-25 scroll.  Set once
 * with DECSTBM, so output from a foreground job between prompts scrolls
 * the region and cannot carry the header away.
 */
#define	ESC_REGION_BODY	"\x1b[3;25r"
#define	ESC_REGION_ALL	"\x1b[r"
#define	ESC_BODY_HOME	"\x1b[3;1H"

/* ---- single-byte read from the kbd stream port ------------------- */

/*
 * read_byte: block on the kbd stream port and return the byte the driver
 * put in msgh_id, or -1 on error.  The driver holds its send right for
 * the life of the system, so an error means the stream is gone.
 */
static int
read_byte(void)
{
	struct mach_msg_header	hdr;
	int			rv;

	rv = mach_msg_recv(g_kbd_stream, &hdr, sizeof(hdr));
	if (rv != MACH_MSG_OK)
		return (-1);
	return ((int)(unsigned char)hdr.msgh_id);
}

/* ---- keys, not bytes ---------------------------------------------- */

/*
 * A keypress, decoded from dev/kbd.c's CSI sequences ("\x1b[A" for Up),
 * shared by the line editor and the pager.  An unrecognised sequence
 * comes back as SH_K_UNKNOWN, never as its bytes, so it cannot type
 * itself into the line.
 *
 * Without a timer a lone Escape looks like the start of a sequence, so
 * read_key blocks for the byte after it; consumers see Escape only
 * through the key that follows.
 */
enum sh_key {
	SH_K_CHAR = 0,		/* an ordinary byte, in kp_ch */
	SH_K_UP,
	SH_K_DOWN,
	SH_K_LEFT,
	SH_K_RIGHT,
	SH_K_HOME,
	SH_K_END,
	SH_K_DELETE,
	SH_K_INSERT,
	SH_K_PGUP,
	SH_K_PGDN,
	SH_K_ESC,		/* Escape, followed by something not a CSI */
	SH_K_UNKNOWN,		/* a well-formed sequence we have no name for */
	SH_K_EOF,		/* the keyboard stream died */
};

struct sh_keypress {
	enum sh_key	kp_key;
	char		kp_ch;
};

static void
read_key(struct sh_keypress *out)
{
	int	c;
	int	arg;

	out->kp_key = SH_K_EOF;
	out->kp_ch  = 0;

	c = read_byte();
	if (c < 0)
		return;
	if (c != 0x1b) {
		out->kp_key = SH_K_CHAR;
		out->kp_ch  = (char)c;
		return;
	}

	c = read_byte();
	if (c < 0)
		return;
	if (c != '[') {
		out->kp_key = SH_K_ESC;
		return;
	}

	/*
	 * Parameters, then a final byte.  No key sequence has more than one
	 * parameter; digits accumulate into it.
	 */
	arg = 0;
	for (;;) {
		c = read_byte();
		if (c < 0)
			return;
		if (c >= '0' && c <= '9') {
			arg = arg * 10 + (c - '0');
			continue;
		}
		break;
	}

	out->kp_key = SH_K_UNKNOWN;
	switch (c) {
	case 'A': out->kp_key = SH_K_UP;     break;
	case 'B': out->kp_key = SH_K_DOWN;   break;
	case 'C': out->kp_key = SH_K_RIGHT;  break;
	case 'D': out->kp_key = SH_K_LEFT;   break;
	case 'H': out->kp_key = SH_K_HOME;   break;
	case 'F': out->kp_key = SH_K_END;    break;
	case '~':
		switch (arg) {
		case 1: case 7: out->kp_key = SH_K_HOME;   break;
		case 2:         out->kp_key = SH_K_INSERT; break;
		case 3:         out->kp_key = SH_K_DELETE; break;
		case 4: case 8: out->kp_key = SH_K_END;    break;
		case 5:         out->kp_key = SH_K_PGUP;   break;
		case 6:         out->kp_key = SH_K_PGDN;   break;
		}
		break;
	}
}

/* ---- service-query helpers --------------------------------------- */

/*
 * fetch_clock / fetch_stats: one RPC each on the cached port.  0 on
 * success, -1 on failure with the reply zeroed, so callers can print it
 * either way.
 */
static int
fetch_clock(struct svc_clock_reply *out)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header	hdr;
		struct svc_clock_reply	body;
	} reply;
	int			rv;

	memset(out, 0, sizeof(*out));
	if (g_clock_port == MACH_PORT_NULL)
		return (-1);

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = g_clock_port;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = CLOCK_OP_GET;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (-1);
	*out = reply.body;
	return (0);
}

/*
 * fetch_progs: one RPC into the progreg service, at startup.  The reply
 * is nearly 800 bytes, so it lands in a static rather than on the one
 * page of stack a ring-3 task gets.
 */
static void
fetch_progs(void)
{
	struct mach_msg_header	req;
	static struct {
		struct mach_msg_header		hdr;
		struct svc_progreg_reply	body;
	} reply;
	mach_port_name_t	svc;
	uint32_t		i;
	int			rv;

	svc = bootstrap_lookup(SVC_PROGREG_NAME);
	if (svc == MACH_PORT_NULL)
		return;

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = PROGREG_OP_LIST;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK)
		return;

	for (i = 0; i < SVC_PROGREG_BYTES; i++)
		sh_progs[i] = reply.body.pr_names[i];
	sh_progs_n     = reply.body.pr_count;
	sh_progs_total = reply.body.pr_total;
	sh_progs_macho = reply.body.pr_macho;
}

static int
fetch_stats(struct svc_stats_reply *out)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header	hdr;
		struct svc_stats_reply	body;
	} reply;
	int			rv;

	memset(out, 0, sizeof(*out));
	if (g_stats_port == MACH_PORT_NULL)
		return (-1);

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = g_stats_port;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = STATS_OP_GET;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (-1);
	*out = reply.body;
	return (0);
}

/* ---- TUI surface ------------------------------------------------- */

/*
 * Box drawing.  The VGA font has the CP437 line-drawing glyphs and the tty
 * passes every byte >= 0x20 to the cell grid, so a frame costs what the
 * same number of letters would.  Byte constants, not characters: the
 * console is CP437 and nothing here is UTF-8.
 */
#define	BOX_H		'\xc4'		/* horizontal            */
#define	BOX_V		'\xb3'		/* vertical              */
#define	BOX_TL		'\xda'		/* top left corner       */
#define	BOX_TR		'\xbf'		/* top right corner      */
#define	BOX_BL		'\xc0'		/* bottom left corner    */
#define	BOX_BR		'\xd9'		/* bottom right corner   */
#define	BOX_LT		'\xc3'		/* left tee              */
#define	BOX_RT		'\xb4'		/* right tee             */
#define	SHADE_LIGHT	'\xb0'
#define	SHADE_MED	'\xb1'
#define	SHADE_DARK	'\xb2'
#define	BLOCK_FULL	'\xdb'

/*
 * A panel is a frame two columns in from each margin: columns 3..78,
 * seventy-six wide with both borders.  Each row is built in one buffer
 * and emitted with one write: one syscall, one tty batch, one cursor
 * update.
 *
 * The console is TTY_COLS (80) wide (dev/tty.h) and the native ABI has
 * no window-size query, so the width is a constant here.
 */
#define	SH_COLS		80

#define	PANEL_LEFT	2
#define	PANEL_W		76

/*
 * Geometry is settable because the pager wants the whole screen and the
 * splash and help an inset card.  One panel is drawn at a time, so two
 * variables suffice.
 */
static size_t	panel_left = PANEL_LEFT;
static size_t	panel_width = PANEL_W;

static size_t
panel_inner(void)
{

	return (panel_width - 2);
}

static void
panel_set(size_t left, size_t width)
{

	panel_left  = left;
	panel_width = width;
}

/* Buffer builders, defined with the prompt they were written for. */
static size_t	sh_append(char *, size_t, size_t, const char *);
static size_t	sh_append_uint(char *, size_t, size_t, unsigned);

static size_t
sh_pad(char *dst, size_t off, size_t cap, char ch, size_t n)
{

	while (n-- > 0 && off + 1 < cap)
		dst[off++] = ch;
	return (off);
}

/* Visible width of a string: its length less SGR escape bytes. */
static size_t
sh_visible(const char *s)
{
	size_t	n;

	n = 0;
	while (*s != '\0') {
		if (*s == 0x1b) {
			while (*s != '\0' && *s != 'm')
				s++;
			if (*s != '\0')
				s++;
			continue;
		}
		s++;
		n++;
	}
	return (n);
}

/*
 * One framed row.  `body' may carry colour; padding follows its visible
 * width, so the right border lands in place.
 */
static void
panel_row(const char *body)
{
	char	out[512];
	size_t	off;
	size_t	vis;

	off = 0;
	off = sh_pad(out, off, sizeof(out), ' ', panel_left);
	off = sh_append(out, off, sizeof(out), ESC_FG_DGRAY);
	if (off + 1 < sizeof(out))
		out[off++] = BOX_V;
	off = sh_append(out, off, sizeof(out), ESC_RESET);

	/*
	 * Copy a column at a time so the body is clipped at the right
	 * border (the pager passes whole manual-page lines).  Escape
	 * sequences pass through uncounted.
	 */
	vis = 0;
	while (*body != '\0' && vis < panel_inner()) {
		if (*body == 0x1b) {
			while (*body != '\0' && *body != 'm') {
				if (off + 1 < sizeof(out))
					out[off++] = *body;
				body++;
			}
			if (*body != '\0') {
				if (off + 1 < sizeof(out))
					out[off++] = *body;
				body++;
			}
			continue;
		}
		if (*body == '\t') {
			/*
			 * Expand tabs (one byte, up to eight columns) to the
			 * panel's own stops, so the width stays right and
			 * the tty never sees one.
			 */
			size_t	stop;

			stop = (vis + 8) & ~(size_t)7;
			if (stop > panel_inner())
				stop = panel_inner();
			while (vis < stop) {
				if (off + 1 < sizeof(out))
					out[off++] = ' ';
				vis++;
			}
			body++;
			continue;
		}
		/*
		 * Any other control byte becomes a space; a stray carriage
		 * return would paint over the left border.
		 */
		if (off + 1 < sizeof(out))
			out[off++] = (*body >= 0 && *body < 0x20) ? ' ' : *body;
		body++;
		vis++;
	}

	off = sh_pad(out, off, sizeof(out), ' ', panel_inner() - vis);
	off = sh_append(out, off, sizeof(out), ESC_FG_DGRAY);
	if (off + 1 < sizeof(out))
		out[off++] = BOX_V;
	off = sh_append(out, off, sizeof(out), ESC_RESET);
	if (off + 1 < sizeof(out))
		out[off++] = '\n';
	(void)write(out, off);
}

/*
 * An edge of the frame, with `title' inlaid (NULL for a plain edge).
 * `left' and `right' pick the corners, so this draws the top, the bottom
 * and section separators.
 */
static void
panel_edge(char left, char right, const char *title)
{
	char	out[512];
	size_t	off;
	size_t	used;

	off = 0;
	off = sh_pad(out, off, sizeof(out), ' ', panel_left);
	off = sh_append(out, off, sizeof(out), ESC_FG_DGRAY);
	if (off + 1 < sizeof(out))
		out[off++] = left;

	used = 0;
	if (title != NULL) {
		off = sh_pad(out, off, sizeof(out), BOX_H, 2);
		if (off + 1 < sizeof(out))
			out[off++] = ' ';
		off = sh_append(out, off, sizeof(out), ESC_FG_WHITE);
		off = sh_append(out, off, sizeof(out), title);
		off = sh_append(out, off, sizeof(out), ESC_FG_DGRAY);
		if (off + 1 < sizeof(out))
			out[off++] = ' ';
		used = 4 + sh_visible(title);
		if (used > panel_inner())
			used = panel_inner();
	}
	off = sh_pad(out, off, sizeof(out), BOX_H, panel_inner() - used);
	if (off + 1 < sizeof(out))
		out[off++] = right;
	off = sh_append(out, off, sizeof(out), ESC_RESET);
	if (off + 1 < sizeof(out))
		out[off++] = '\n';
	(void)write(out, off);
}

static void
panel_top(const char *title)
{

	panel_edge(BOX_TL, BOX_TR, title);
}

static void
panel_sep(void)
{

	panel_edge(BOX_LT, BOX_RT, NULL);
}

static void
panel_bottom(void)
{

	panel_edge(BOX_BL, BOX_BR, NULL);
}

/*
 * A bottom edge with a caption, so the pager's position and key legend
 * cost no row of their own.
 */
static void
panel_bottom_captioned(const char *caption)
{

	panel_edge(BOX_BL, BOX_BR, caption);
}

/*
 * A gauge: dark shade for the fill, light shade for the track, so a
 * nearly empty gauge still reads as one.
 */
static size_t
sh_gauge(char *dst, size_t off, size_t cap, uint64_t used, uint64_t total,
    size_t width)
{
	size_t	filled;

	filled = total == 0 ? 0 : (size_t)((used * width) / total);
	if (filled > width)
		filled = width;
	if (filled == 0 && used > 0)
		filled = 1;	/* "some" must not draw as "none" */

	off = sh_append(dst, off, cap, ESC_FG_WHITE);
	off = sh_pad(dst, off, cap, SHADE_DARK, filled);
	off = sh_append(dst, off, cap, ESC_FG_DGRAY);
	off = sh_pad(dst, off, cap, SHADE_LIGHT, width - filled);
	off = sh_append(dst, off, cap, ESC_RESET);
	return (off);
}

/*
 * paint_status_bar: the two rows above the scrolling region, a title bar
 * and a rule.  They never scroll; this refreshes the counters.  The
 * cursor is hidden for the trip, or it is seen jumping to the top and
 * back at every prompt.
 */
static void
paint_status_bar(void)
{
	struct svc_clock_reply	ck;
	struct svc_stats_reply	st;
	char			out[256];
	size_t			off;
	size_t			vis;
	uint64_t		s, m, h;

	(void)fetch_clock(&ck);
	(void)fetch_stats(&st);
	s = ck.cr_uptime_ms / 1000ull;
	h = s / 3600ull;
	s = s - h * 3600ull;
	m = s / 60ull;
	s = s - m * 60ull;

	puts(ESC_HIDE_CUR);
	puts(ESC_SAVE_CUR);
	puts(ESC_HOME);

	/*
	 * Left: the name.  Right: task count and uptime, padded by visible
	 * width so the clock ends in column 79.
	 */
	off = 0;
	off = sh_append(out, off, sizeof(out), ESC_BAR);
	off = sh_append(out, off, sizeof(out), " style9-os(9)");
	vis = 13;

	off = sh_append(out, off, sizeof(out), ESC_BAR_DIM);
	{
		char	right[48];
		size_t	r;

		r = 0;
		r = sh_append_uint(right, r, sizeof(right),
		    (unsigned)st.sr_task_count);
		r = sh_append(right, r, sizeof(right), " tasks   ");
		r = sh_append_uint(right, r, sizeof(right), (unsigned)h);
		right[r++] = ':';
		right[r++] = (char)('0' + ((unsigned)(m / 10) % 10));
		right[r++] = (char)('0' + ((unsigned)m % 10));
		right[r++] = ':';
		right[r++] = (char)('0' + ((unsigned)(s / 10) % 10));
		right[r++] = (char)('0' + ((unsigned)s % 10));
		right[r]   = '\0';

		off = sh_pad(out, off, sizeof(out), ' ',
		    79 - vis - sh_visible(right));
		off = sh_append(out, off, sizeof(out), right);
	}
	off = sh_pad(out, off, sizeof(out), ' ', 1);
	off = sh_append(out, off, sizeof(out), ESC_RESET);
	if (off + 1 < sizeof(out))
		out[off++] = '\n';
	(void)write(out, off);

	off = 0;
	off = sh_append(out, off, sizeof(out), ESC_FG_DGRAY);
	off = sh_pad(out, off, sizeof(out), BOX_H, 80);
	off = sh_append(out, off, sizeof(out), ESC_RESET);
	(void)write(out, off);

	puts(ESC_REST_CUR);
	puts(ESC_SHOW_CUR);
}

/*
 * paint_splash: a manpage-shaped welcome, in a frame.
 *
 *	white		section labels and the frame's title
 *	gray		body text and value columns
 *	dark gray	the frame itself, and the empty half of the gauge
 */
static void
paint_splash(void)
{
	struct svc_stats_reply	st;
	char			row[256];
	size_t			off;
	uint64_t		used_kib;
	uint64_t		total_kib;

	(void)fetch_stats(&st);
	used_kib  = st.sr_pmm_used_pages * 4ull;
	total_kib = st.sr_pmm_total_pages * 4ull;

	panel_top("style9-os(9)");
	panel_row("");

	off = 0;
	off = sh_append(row, off, sizeof(row), "   ");
	off = sh_append(row, off, sizeof(row), ESC_FG_WHITE);
	off = sh_append(row, off, sizeof(row), "NAME       ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row),
	    "style9-os -- BSD-flavoured x86_64 kernel");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);

	off = 0;
	off = sh_append(row, off, sizeof(row), "              ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "with Mach IPC");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);
	panel_row("");

	off = 0;
	off = sh_append(row, off, sizeof(row), "   ");
	off = sh_append(row, off, sizeof(row), ESC_FG_WHITE);
	off = sh_append(row, off, sizeof(row), "SYSTEM     ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "arch     x86_64");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);

	off = 0;
	off = sh_append(row, off, sizeof(row), "              ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "memory   ");
	off = sh_gauge(row, off, sizeof(row), used_kib, total_kib, 20);
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "  ");
	off = sh_append_uint(row, off, sizeof(row), (unsigned)(used_kib / 1024));
	off = sh_append(row, off, sizeof(row), " / ");
	off = sh_append_uint(row, off, sizeof(row),
	    (unsigned)(total_kib / 1024));
	off = sh_append(row, off, sizeof(row), " MiB");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);

	off = 0;
	off = sh_append(row, off, sizeof(row), "              ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "tasks    ");
	off = sh_append_uint(row, off, sizeof(row),
	    (unsigned)st.sr_task_count);
	off = sh_append(row, off, sizeof(row), " live, ");
	off = sh_append_uint(row, off, sizeof(row),
	    (unsigned)st.sr_thread_count);
	off = sh_append(row, off, sizeof(row), " threads");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);

	off = 0;
	off = sh_append(row, off, sizeof(row), "              ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "programs ");
	off = sh_append_uint(row, off, sizeof(row), sh_progs_total);
	off = sh_append(row, off, sizeof(row), " in the registry");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);
	panel_row("");

	off = 0;
	off = sh_append(row, off, sizeof(row), "   ");
	off = sh_append(row, off, sizeof(row), ESC_FG_WHITE);
	off = sh_append(row, off, sizeof(row), "SEE ALSO   ");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row), "style(9), help(1)");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);
	panel_row("");
	panel_bottom();
}

/* ---- the prompt, as bytes and as a width -------------------------- */

/*
 * The prompt is both bytes to emit (with SGR colour) and a width in
 * columns, which the line editor needs to put the cursor back after
 * repainting the line.  It is a bold white '$ ', preceded by 'err N' in
 * light red after a failure.
 */
#define	SH_PROMPT_MAX	64

static char	sh_prompt[SH_PROMPT_MAX];
static size_t	sh_prompt_cols;

static size_t
sh_append(char *dst, size_t off, size_t cap, const char *s)
{

	while (*s != '\0' && off + 1 < cap)
		dst[off++] = *s++;
	return (off);
}

static size_t
sh_append_uint(char *dst, size_t off, size_t cap, unsigned v)
{
	char	tmp[12];
	int	n;

	n = 0;
	if (v == 0)
		tmp[n++] = '0';
	while (v > 0 && n < (int)sizeof(tmp)) {
		tmp[n++] = (char)('0' + (v % 10u));
		v /= 10u;
	}
	while (n > 0 && off + 1 < cap)
		dst[off++] = tmp[--n];
	return (off);
}

static void
prompt_build(void)
{
	size_t	off;
	int	v;
	int	digits;

	off = 0;
	sh_prompt_cols = 0;

	if (last_status != 0) {
		off = sh_append(sh_prompt, off, sizeof(sh_prompt), ESC_FG_RED);
		off = sh_append(sh_prompt, off, sizeof(sh_prompt), "err ");
		sh_prompt_cols += 4;
		v = last_status;
		if (v < 0) {
			off = sh_append(sh_prompt, off, sizeof(sh_prompt), "-");
			sh_prompt_cols++;
			v = -v;
		}
		for (digits = 1; v >= 10; digits++)
			v /= 10;
		v = last_status < 0 ? -last_status : last_status;
		off = sh_append_uint(sh_prompt, off, sizeof(sh_prompt),
		    (unsigned)v);
		sh_prompt_cols += (size_t)digits;
		off = sh_append(sh_prompt, off, sizeof(sh_prompt), "  ");
		sh_prompt_cols += 2;
	}
	off = sh_append(sh_prompt, off, sizeof(sh_prompt), ESC_FG_WHITE);
	off = sh_append(sh_prompt, off, sizeof(sh_prompt), "$ ");
	sh_prompt_cols += 2;
	off = sh_append(sh_prompt, off, sizeof(sh_prompt), ESC_RESET);
	sh_prompt[off] = '\0';
}

/* ---- argv tokenizer (in-place) ----------------------------------- */

static int
is_blank(char c)
{

	return (c == ' ' || c == '\t');
}

static int
split_argv(char *line, char *argv[], int max)
{
	char	*p;
	int	 argc;

	argc = 0;
	p = line;
	while (*p != '\0' && argc < max) {
		while (*p != '\0' && is_blank(*p))
			p++;
		if (*p == '\0')
			break;
		argv[argc++] = p;
		while (*p != '\0' && !is_blank(*p))
			p++;
		if (*p == '\0')
			break;
		*p++ = '\0';
	}
	return (argc);
}

/* ---- builtins ----------------------------------------------------- */

/* The builtins, as data, for help and tab completion. */
struct sh_builtin {
	const char	*b_name;
	const char	*b_help;
};

static const struct sh_builtin sh_builtins[] = {
	{ "help",  "show this list" },
	{ "echo",  "print arguments" },
	{ "clear", "clear screen and repaint the splash" },
	{ "about", "version banner + live counters" },
	{ "ool",   "OOL Mach IPC round-trip via svc/echool" },
	{ "man",   "show a manual page (try: man port)" },
	{ "kill",  "kill <task_id> -- terminate a child of this shell" },
	{ NULL,    NULL },
};

/*
 * builtin_help: one panel, builtins above a separator and the spawnable
 * programs below in a four-column grid.
 */
static void
builtin_help(void)
{
	const char	*name;
	char		 row[256];
	size_t		 off;
	size_t		 col;
	uint32_t	 i;

	panel_top("help(1)");
	for (i = 0; sh_builtins[i].b_name != NULL; i++) {
		off = 0;
		off = sh_append(row, off, sizeof(row), "   ");
		off = sh_append(row, off, sizeof(row), ESC_FG_WHITE);
		off = sh_append(row, off, sizeof(row), sh_builtins[i].b_name);
		off = sh_pad(row, off, sizeof(row), ' ',
		    9 - sh_visible(sh_builtins[i].b_name));
		off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
		off = sh_append(row, off, sizeof(row), sh_builtins[i].b_help);
		off = sh_append(row, off, sizeof(row), ESC_RESET);
		row[off] = '\0';
		panel_row(row);
	}

	panel_sep();

	if (sh_progs_n == 0) {
		panel_row("   the progreg service did not answer");
		panel_bottom();
		return;
	}

	/*
	 * Cyan is a Mach-O run under the clean-room dyld, gray a native
	 * ELF.  The kernel sets pr_macho from the image's magic, the same
	 * sniff the loader makes, so the two cannot drift apart.
	 */
	col = 0;
	off = 0;
	for (i = 0; i < sh_progs_n; i++) {
		name = prog_at(i);
		if (name == NULL)
			break;
		if (col == 0)
			off = sh_append(row, off, sizeof(row), "   ");
		off = sh_append(row, off, sizeof(row),
		    (sh_progs_macho & (1ull << i)) != 0 ?
		    ESC_FG_CYAN : ESC_FG_GRAY);
		off = sh_append(row, off, sizeof(row), name);
		off = sh_pad(row, off, sizeof(row), ' ',
		    17 - sh_visible(name));
		if (++col == 4) {
			off = sh_append(row, off, sizeof(row), ESC_RESET);
			row[off] = '\0';
			panel_row(row);
			col = 0;
			off = 0;
		}
	}
	if (col != 0) {
		off = sh_append(row, off, sizeof(row), ESC_RESET);
		row[off] = '\0';
		panel_row(row);
	}
	panel_sep();
	off = 0;
	off = sh_append(row, off, sizeof(row), "   ");
	off = sh_append(row, off, sizeof(row), ESC_FG_CYAN);
	off = sh_append(row, off, sizeof(row), "cyan");
	off = sh_append(row, off, sizeof(row), ESC_FG_GRAY);
	off = sh_append(row, off, sizeof(row),
	    " comes up under the clean-room dyld as a Mach-O;  gray does not");
	off = sh_append(row, off, sizeof(row), ESC_RESET);
	row[off] = '\0';
	panel_row(row);

	if (sh_progs_total > sh_progs_n) {
		off = 0;
		off = sh_append(row, off, sizeof(row), "   ");
		off = sh_append(row, off, sizeof(row), ESC_FG_RED);
		off = sh_append_uint(row, off, sizeof(row),
		    sh_progs_total - sh_progs_n);
		off = sh_append(row, off, sizeof(row),
		    " more the reply had no room for");
		off = sh_append(row, off, sizeof(row), ESC_RESET);
		row[off] = '\0';
		panel_row(row);
	}
	panel_bottom();
}

static void
builtin_echo(int argc, char *argv[])
{
	int	i;

	for (i = 1; i < argc; i++) {
		if (i > 1)
			putchar(' ');
		puts(argv[i]);
	}
	putchar('\n');
}

/*
 * builtin_clear: erase the screen and redraw the splash.  The status bar
 * is repainted at the next prompt.
 */
static void
builtin_clear(void)
{

	/*
	 * The clear homes the cursor to the screen's top-left, inside the
	 * chrome; the scroll region does not affect addressing, so move
	 * back into it explicitly.
	 */
	puts(ESC_CLR_SCR);
	puts(ESC_BODY_HOME);
	paint_splash();
}

/*
 * builtin_ool: send a small buffer to the kernel's echool service as one
 * OOL descriptor and compare its FNV-1a with ours.  Proves a ring-3 OOL
 * message is well formed, the kernel parses the descriptor area, and the
 * special-port dispatcher can reach the sender's pages.
 */
static uint32_t
ool_fnv1a(const uint8_t *buf, uint32_t size)
{
	uint32_t	h, i;

	h = 0x811C9DC5u;
	for (i = 0; i < size; i++) {
		h ^= (uint32_t)buf[i];
		h *= 0x01000193u;
	}
	return (h);
}

static void
builtin_ool(void)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	} req;
	struct mach_msg_header	reply;
	uint8_t			buf[256];
	mach_port_name_t	svc;
	uint32_t		i, expected;
	int			rv;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)((i * 31u + 7u) & 0xFFu);
	expected = ool_fnv1a(buf, sizeof(buf));

	svc = bootstrap_lookup(SVC_ECHOOL_NAME);
	if (svc == MACH_PORT_NULL) {
		puts(ESC_FG_GRAY);
		puts("  echool: ");
		puts(ESC_FG_WHITE);
		puts("service lookup failed\n");
		puts(ESC_RESET);
		return;
	}

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0)
	    | MACH_MSGH_BITS_COMPLEX;
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = ECHOOL_OP_CHECKSUM;

	req.body.msgh_descriptor_count = 1;

	req.ool.type       = MACH_MSG_OOL_DESCRIPTOR;
	req.ool.copy       = MACH_MSG_PHYSICAL_COPY;
	req.ool.deallocate = 0;
	req.ool.pad        = 0;
	req.ool.size       = (uint32_t)sizeof(buf);
	req.ool.address    = (uint64_t)(uintptr_t)buf;

	rv = mach_msg_rpc(&req.hdr, &reply, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);

	puts(ESC_FG_GRAY);
	puts("  ool ");
	puts(ESC_FG_WHITE);
	if (rv != MACH_MSG_OK) {
		printf("rpc failed rv=%d\n", rv);
	} else if (reply.msgh_id == expected) {
		printf("OK  %u bytes  fnv1a=0x%x\n",
		    (unsigned)sizeof(buf), (unsigned)expected);
	} else {
		printf("MISMATCH client=0x%x kernel=0x%x\n",
		    (unsigned)expected, (unsigned)reply.msgh_id);
	}
	puts(ESC_RESET);
}

static void
builtin_about(void)
{
	struct svc_clock_reply	ck;
	struct svc_stats_reply	st;
	uint64_t		s, m, h;

	(void)fetch_clock(&ck);
	(void)fetch_stats(&st);
	s = ck.cr_uptime_ms / 1000ull;
	h = s / 3600ull;
	s = s - h * 3600ull;
	m = s / 60ull;
	s = s - m * 60ull;

	puts(ESC_FG_GRAY);
	puts("  style9-os -- a BSD-flavoured x86_64 kernel with Mach IPC.\n");
	puts("  monolithic in the XNU sense: services and drivers live in\n");
	puts("  ring 0, Mach msg is the inter-component messaging surface.\n");
	puts("  written end-to-end in the style(9) BSD KNF convention,\n");
	puts("  hence the name.\n");
	puts("\n");
	printf("  uptime %llu:%02llu:%02llu   |   %llu tasks, %llu threads   |"
	    "   %llu ctx switches\n",
	    (unsigned long long)h,
	    (unsigned long long)m,
	    (unsigned long long)s,
	    (unsigned long long)st.sr_task_count,
	    (unsigned long long)st.sr_thread_count,
	    (unsigned long long)st.sr_ctx_switches);
	puts(ESC_RESET);
}

/* ---- pager + man builtin ----------------------------------------- */

/*
 * A small pager after less(1).  Paints PAGER_SCREEN_ROWS lines of a text
 * buffer at a time:
 *
 *	Space, PgDn, Ctrl-F	page down
 *	b, PgUp, Ctrl-B		page up
 *	j, Enter, Down arrow	line down
 *	k, Up arrow		line up
 *	g, Home			top
 *	G, End			bottom
 *	q, Esc			quit
 *
 * Keys come through read_key.  Each line's (offset, length) is indexed
 * up front, capped at PAGER_MAX_LINES; the longest page in docs/man
 * (port.9) is 561 lines.
 */

#define	PAGER_MAX_LINES		4096
#define	PAGER_SCREEN_ROWS	22

static uint32_t	pager_line_off[PAGER_MAX_LINES];
static uint32_t	pager_line_len[PAGER_MAX_LINES];

static size_t
pager_index_lines(const char *text, size_t len)
{
	size_t	i;
	size_t	lines;
	size_t	line_start;

	lines = 0;
	line_start = 0;
	for (i = 0; i < len && lines < PAGER_MAX_LINES; i++) {
		if (text[i] == '\n') {
			pager_line_off[lines] = (uint32_t)line_start;
			pager_line_len[lines] = (uint32_t)(i - line_start);
			lines++;
			line_start = i + 1;
		}
	}
	if (line_start < len && lines < PAGER_MAX_LINES) {
		pager_line_off[lines] = (uint32_t)line_start;
		pager_line_len[lines] = (uint32_t)(len - line_start);
		lines++;
	}
	return (lines);
}

static void
pager_repaint(const char *text, size_t total_lines, size_t top,
    const char *title)
{
	char	row[512];
	char	caption[128];
	size_t	end;
	size_t	off;
	size_t	i;
	size_t	n;

	puts(ESC_CLR_SCR);
	panel_set(0, SH_COLS);
	panel_top(title);

	end = top + PAGER_SCREEN_ROWS;
	if (end > total_lines)
		end = total_lines;

	/*
	 * Copy each line into a NUL-terminated buffer for panel_row, which
	 * clips it and draws the right border.
	 */
	for (i = top; i < end; i++) {
		const char	*src;
		size_t		 lead;

		src  = text + pager_line_off[i];
		n    = pager_line_len[i];
		lead = sizeof(ESC_FG_GRAY) - 1;

		/*
		 * Section headers are the only lines starting in column zero
		 * with a capital (mandoc indents the rest): draw them bold.
		 */
		if (n > 0 && src[0] >= 'A' && src[0] <= 'Z') {
			for (off = 0; off < lead; off++)
				row[off] = ESC_FG_WHITE[off];
			lead = sizeof(ESC_FG_WHITE) - 1;
		} else {
			for (off = 0; off < lead; off++)
				row[off] = ESC_FG_GRAY[off];
		}

		if (n > sizeof(row) - lead - 8)
			n = sizeof(row) - lead - 8;
		for (off = 0; off < n; off++)
			row[lead + off] = src[off];
		row[lead + n] = '\0';
		panel_row(row);
	}
	for (i = end - top; i < PAGER_SCREEN_ROWS; i++)
		panel_row("");

	/* Position and keys go in the bottom edge. */
	off = 0;
	off = sh_append_uint(caption, off, sizeof(caption),
	    (unsigned)(top + 1));
	off = sh_append(caption, off, sizeof(caption), "-");
	off = sh_append_uint(caption, off, sizeof(caption), (unsigned)end);
	off = sh_append(caption, off, sizeof(caption), "/");
	off = sh_append_uint(caption, off, sizeof(caption),
	    (unsigned)total_lines);
	off = sh_append(caption, off, sizeof(caption), ESC_FG_GRAY);
	off = sh_append(caption, off, sizeof(caption),
	    "   space/b page   j/k line   g/G ends   q quit");
	caption[off] = '\0';
	panel_bottom_captioned(caption);

	panel_set(PANEL_LEFT, PANEL_W);
}

static void
pager_show(const char *text, size_t len, const char *title)
{
	struct sh_keypress	kp;
	size_t			max_top;
	size_t			top;
	size_t			total_lines;
	int			act;
	int			quit;

	if (text == NULL || len == 0)
		return;
	quit = 0;

	total_lines = pager_index_lines(text, len);
	if (total_lines == 0)
		return;

	/*
	 * The pager owns the whole screen: drop the scrolling region and
	 * hide the cursor, restoring both on the way out.
	 */
	puts(ESC_REGION_ALL);
	puts(ESC_HIDE_CUR);

	max_top = total_lines > PAGER_SCREEN_ROWS ?
	    total_lines - PAGER_SCREEN_ROWS : 0;
	top      = 0;

	pager_repaint(text, total_lines, top, title);

	while (!quit) {
		read_key(&kp);
		if (kp.kp_key == SH_K_EOF || kp.kp_key == SH_K_ESC)
			break;

		act = 1;
		switch (kp.kp_key) {
		case SH_K_DOWN:
			top++;
			break;
		case SH_K_UP:
			if (top > 0)
				top--;
			break;
		case SH_K_PGDN:
			top += PAGER_SCREEN_ROWS;
			break;
		case SH_K_PGUP:
			top = top >= PAGER_SCREEN_ROWS ?
			    top - PAGER_SCREEN_ROWS : 0;
			break;
		case SH_K_HOME:
			top = 0;
			break;
		case SH_K_END:
			top = max_top;
			break;
		case SH_K_CHAR:
			switch (kp.kp_ch) {
			case 'q':
				quit = 1;
				act  = 0;
				break;
			case ' ':
			case 0x06:	/* ^F */
				top += PAGER_SCREEN_ROWS;
				break;
			case 'b':
			case 0x02:	/* ^B */
				top = top >= PAGER_SCREEN_ROWS ?
				    top - PAGER_SCREEN_ROWS : 0;
				break;
			case 'j':
			case '\n':
			case '\r':
				top++;
				break;
			case 'k':
				if (top > 0)
					top--;
				break;
			case 'g':
				top = 0;
				break;
			case 'G':
				top = max_top;
				break;
			default:
				act = 0;
				break;
			}
			break;
		default:
			act = 0;
			break;
		}

		if (act) {
			if (top > max_top)
				top = max_top;
			pager_repaint(text, total_lines, top, title);
		}
	}

	puts(ESC_SHOW_CUR);
	puts(ESC_CLR_SCR);
	puts(ESC_REGION_BODY);
	puts(ESC_BODY_HOME);
}

/*
 * builtin_man: fetch the page from the "man" service (man_fetch) and
 * page the OOL-installed text, titled "<name>(9)".  Prints a short error
 * if the page is missing or the RPC fails.
 */
static void
builtin_man(int argc, char *argv[])
{
	const char	*name;
	const char	*text;
	char		 title[MAN_NAME_MAX + 4];
	size_t		 i;
	size_t		 len;
	int		 rv;

	if (argc < 2) {
		puts("usage: man <topic>   (try: man port)\n");
		return;
	}
	name = argv[1];

	rv = man_fetch(name, &text, &len);
	if (rv != MACH_MSG_OK) {
		if (rv == MACH_E_NAME) {
			puts("no man page for '");
			puts(name);
			puts("'\n");
		} else {
			printf("man: fetch failed, rv=%d\n", rv);
		}
		return;
	}

	for (i = 0;
	    i < sizeof(title) - 4 && name[i] != '\0';
	    i++)
		title[i] = name[i];
	title[i++] = '(';
	title[i++] = '9';
	title[i++] = ')';
	title[i]   = '\0';

	pager_show(text, len, title);

	/*
	 * Release the OOL range so each `man' does not leak a mapping.  A
	 * failure is harmless: the buffer stays until task exit.
	 */
	(void)man_release(text, len);
}

/* ---- spawn + wait ------------------------------------------------ */

/*
 * Store the (task_id, taskport) pair in the first empty slot, or in
 * slot 0 when all are full.
 */
static void
sh_child_remember(uint64_t task_id, mach_port_name_t taskport)
{
	size_t	i;

	for (i = 0; i < SH_CHILD_MAX; i++) {
		if (sh_children[i].c_task_id == 0) {
			sh_children[i].c_task_id  = task_id;
			sh_children[i].c_taskport = taskport;
			return;
		}
	}
	sh_children[0].c_task_id  = task_id;
	sh_children[0].c_taskport = taskport;
}

/*
 * The saved taskport for a task_id, or MACH_PORT_NULL if unknown or
 * overwritten.
 */
static mach_port_name_t
sh_child_lookup(uint64_t task_id)
{
	size_t	i;

	for (i = 0; i < SH_CHILD_MAX; i++) {
		if (sh_children[i].c_task_id == task_id)
			return (sh_children[i].c_taskport);
	}
	return (MACH_PORT_NULL);
}

/*
 * Foreground wait with Ctrl-C: wait up to 50 ms for a keyboard byte, then
 * re-check task_alive.  On 0x03 (kbd.c folds Ctrl-C to it) issue
 * SYS_TASK_KILL on the job's taskport and keep waiting: the kill is
 * asynchronous (see t_killed in kern/task.h).
 *
 * Other bytes typed during the job are dropped; there is no type-ahead
 * buffer.
 */
static void
wait_child(long task_id, mach_port_name_t taskport)
{
	struct mach_msg_header	hdr;
	int			rv;
	int			c;

	if (task_id <= 0)
		return;
	while (task_alive((uint64_t)task_id)) {
		rv = mach_msg_recv_timed(g_kbd_stream, &hdr, sizeof(hdr), 50);
		if (rv != MACH_MSG_OK)
			continue;
		c = (int)(unsigned char)hdr.msgh_id;
		if (c == 0x03 && taskport != MACH_PORT_NULL) {
			puts(ESC_FG_RED);
			puts("^C\n");
			puts(ESC_RESET);
			(void)task_kill(taskport);
			/* loop again -- kernel retires the task asynchronously */
		}
	}
}

static int
streq(const char *a, const char *b)
{
	size_t	i;

	for (i = 0; ; i++) {
		if (a[i] != b[i])
			return (0);
		if (a[i] == '\0')
			return (1);
	}
}

/*
 * atou64: leading decimal digits, 0 if none; `kill' then reports no
 * taskport for task 0.
 */
static uint64_t
atou64(const char *s)
{
	uint64_t	v;

	v = 0;
	if (s == NULL)
		return (0);
	while (*s >= '0' && *s <= '9') {
		v = v * 10 + (uint64_t)(*s - '0');
		s++;
	}
	return (v);
}

/*
 * `kill <task_id>': SYS_TASK_KILL with the saved taskport.  Capability
 * based, so only children this shell spawned can be killed.  The kill is
 * asynchronous; the target may not have retired when this returns.
 */
static int
builtin_kill(int argc, char *argv[])
{
	mach_port_name_t	taskport;
	uint64_t		task_id;
	int			rv;

	if (argc < 2) {
		puts("  usage: kill <task_id>\n");
		return (0);
	}
	task_id  = atou64(argv[1]);
	taskport = sh_child_lookup(task_id);
	if (taskport == MACH_PORT_NULL) {
		puts(ESC_FG_GRAY);
		puts("  kill: no taskport on file for task_id=");
		puts(argv[1]);
		puts(" (sh only knows children it spawned)\n");
		puts(ESC_RESET);
		return (0);
	}
	rv = task_kill(taskport);
	if (rv != MACH_MSG_OK) {
		puts(ESC_FG_RED);
		puts("  kill: SYS_TASK_KILL returned ");
		puts(ESC_RESET);
		/* tiny integer-to-string for one-digit MACH_E_* codes */
		{
			char	buf[16];
			int	n;
			int	v;
			int	i;
			v = rv < 0 ? -rv : rv;
			n = 0;
			if (rv < 0)
				buf[n++] = '-';
			if (v == 0)
				buf[n++] = '0';
			else {
				int start = n;
				while (v > 0) {
					buf[n++] = (char)('0' + (v % 10));
					v /= 10;
				}
				/* reverse the digits in place */
				for (i = 0; i < (n - start) / 2; i++) {
					char tmp = buf[start + i];
					buf[start + i] = buf[n - 1 - i];
					buf[n - 1 - i] = tmp;
				}
			}
			buf[n] = '\0';
			puts(buf);
		}
		puts("\n");
		return (0);
	}
	return (0);
}

/*
 * dispatch: run a builtin or spawn the program.  Returns the new
 * last_status: 0, or the negative SYS_E_* of a failed spawn.
 */
static int
dispatch(int argc, char *argv[])
{
	mach_port_name_t	taskport;
	long			rv;

	if (argc <= 0)
		return (0);

	if (streq(argv[0], "help")) {
		builtin_help();
		return (0);
	}
	if (streq(argv[0], "echo")) {
		builtin_echo(argc, argv);
		return (0);
	}
	if (streq(argv[0], "clear")) {
		builtin_clear();
		return (0);
	}
	if (streq(argv[0], "about")) {
		builtin_about();
		return (0);
	}
	if (streq(argv[0], "ool")) {
		builtin_ool();
		return (0);
	}
	if (streq(argv[0], "man")) {
		builtin_man(argc, argv);
		return (0);
	}
	if (streq(argv[0], "kill"))
		return (builtin_kill(argc, argv));

	/*
	 * SYS_SPAWN_ARGS: the child gets the whole command line and we get
	 * a send right on its task port for Ctrl-C and `kill'.  argv[0] is
	 * resolved in progreg.
	 */
	taskport = MACH_PORT_NULL;
	rv = spawn_args(argv[0], argc, argv, &taskport);
	if (rv <= 0) {
		puts(ESC_FG_GRAY);
		puts("  ");
		puts(ESC_FG_WHITE);
		puts(argv[0]);
		puts(ESC_FG_GRAY);
		puts(": not found\n");
		puts(ESC_RESET);
		return ((int)rv);
	}
	sh_child_remember((uint64_t)rv, taskport);

	fg_task_id  = rv;
	fg_taskport = taskport;
	wait_child(rv, taskport);
	fg_task_id  = 0;
	fg_taskport = MACH_PORT_NULL;
	return (0);
}

/* ---- line editor ------------------------------------------------- */

/*
 * The line being typed and the cursor position in it.  Editing is insert
 * and delete at the cursor; the line is repainted whole on every
 * keystroke, one write(2) each, so a mid-line insert is no different
 * from an append.
 */
struct sh_line {
	char	l_buf[SH_LINE_MAX];
	size_t	l_len;
	size_t	l_pos;
};

/* History: a plain array, oldest first, shifted down when full. */
#define	SH_HIST_MAX	16

static char	sh_hist[SH_HIST_MAX][SH_LINE_MAX];
static int	sh_hist_n;

static void
line_clear(struct sh_line *ln)
{

	ln->l_len = 0;
	ln->l_pos = 0;
	ln->l_buf[0] = '\0';
}

static void
line_set(struct sh_line *ln, const char *s)
{
	size_t	i;

	for (i = 0; i + 1 < SH_LINE_MAX && s[i] != '\0'; i++)
		ln->l_buf[i] = s[i];
	ln->l_buf[i] = '\0';
	ln->l_len = i;
	ln->l_pos = i;
}

/*
 * Repaint the line.  A line wider than the screen scrolls sideways to
 * keep the cursor visible rather than wrapping, since a wrapped line
 * cannot be repainted from one carriage return.  So SH_LINE_MAX need not
 * fit in SH_COLS.
 */
static void
line_refresh(struct sh_line *ln)
{
	char	out[SH_LINE_MAX + 64];
	size_t	off;
	size_t	start;
	size_t	shown;
	size_t	at;
	size_t	i;

	start = 0;
	shown = ln->l_len;
	at    = ln->l_pos;

	while (sh_prompt_cols + at >= SH_COLS) {
		start++;
		shown--;
		at--;
	}
	while (sh_prompt_cols + shown > SH_COLS)
		shown--;

	off = 0;
	out[off++] = '\r';
	off = sh_append(out, off, sizeof(out), sh_prompt);
	for (i = 0; i < shown && off + 1 < sizeof(out); i++)
		out[off++] = ln->l_buf[start + i];
	off = sh_append(out, off, sizeof(out), "\x1b[K");
	if (off + 1 < sizeof(out))
		out[off++] = '\r';
	if (sh_prompt_cols + at > 0) {
		off = sh_append(out, off, sizeof(out), "\x1b[");
		off = sh_append_uint(out, off, sizeof(out),
		    (unsigned)(sh_prompt_cols + at));
		if (off + 1 < sizeof(out))
			out[off++] = 'C';
	}
	(void)write(out, off);
}

/*
 * A fresh prompt: refresh the chrome, leave a blank row, draw an empty
 * line.  line_refresh emits the prompt, so only it knows where the text
 * of a line begins.
 */
static void
prompt(void)
{
	struct sh_line	empty;

	paint_status_bar();
	puts("\n");
	prompt_build();
	line_clear(&empty);
	line_refresh(&empty);
}

static void
line_insert(struct sh_line *ln, char c)
{
	size_t	i;

	if (ln->l_len + 1 >= SH_LINE_MAX)
		return;
	for (i = ln->l_len; i > ln->l_pos; i--)
		ln->l_buf[i] = ln->l_buf[i - 1];
	ln->l_buf[ln->l_pos] = c;
	ln->l_len++;
	ln->l_pos++;
	ln->l_buf[ln->l_len] = '\0';
}

/* Remove the character at the cursor: Delete, and Ctrl-D. */
static void
line_delete(struct sh_line *ln)
{
	size_t	i;

	if (ln->l_pos >= ln->l_len)
		return;
	for (i = ln->l_pos; i + 1 <= ln->l_len; i++)
		ln->l_buf[i] = ln->l_buf[i + 1];
	ln->l_len--;
}

/* Remove the character before the cursor: Backspace. */
static void
line_erase(struct sh_line *ln)
{

	if (ln->l_pos == 0)
		return;
	ln->l_pos--;
	line_delete(ln);
}

/* Ctrl-W: back over any blanks, then back over the word behind them. */
static void
line_erase_word(struct sh_line *ln)
{

	while (ln->l_pos > 0 && is_blank(ln->l_buf[ln->l_pos - 1]))
		line_erase(ln);
	while (ln->l_pos > 0 && !is_blank(ln->l_buf[ln->l_pos - 1]))
		line_erase(ln);
}

/* ---- completion --------------------------------------------------- */

/*
 * Complete the first word of the line against the builtins and the
 * program registry; arguments are not completed.  One candidate: finish
 * it and add a space (a whole name still gains its space).  Several:
 * extend as far as they agree, or list them if that adds nothing.
 */
static const char *
complete_candidate(uint32_t idx)
{
	uint32_t	n;

	for (n = 0; sh_builtins[n].b_name != NULL; n++)
		continue;
	if (idx < n)
		return (sh_builtins[idx].b_name);
	return (prog_at(idx - n));
}

static int
complete_prefix_match(const char *cand, const char *pfx, size_t pfx_len)
{
	size_t	i;

	for (i = 0; i < pfx_len; i++) {
		if (cand[i] == '\0' || cand[i] != pfx[i])
			return (0);
	}
	return (1);
}

static void
line_complete(struct sh_line *ln)
{
	const char	*cand;
	const char	*first;
	size_t		 pfx_len;
	size_t		 agree;
	uint32_t	 i;
	uint32_t	 nmatch;
	uint32_t	 nbuiltin;

	/*
	 * Only in the first word (nothing blank before the cursor); Tab in
	 * an argument does nothing.
	 */
	for (pfx_len = 0; pfx_len < ln->l_pos; pfx_len++) {
		if (is_blank(ln->l_buf[pfx_len]))
			return;
	}

	for (nbuiltin = 0; sh_builtins[nbuiltin].b_name != NULL; nbuiltin++)
		continue;

	first  = NULL;
	nmatch = 0;
	agree  = 0;
	for (i = 0; i < nbuiltin + sh_progs_n; i++) {
		cand = complete_candidate(i);
		if (cand == NULL)
			break;
		if (!complete_prefix_match(cand, ln->l_buf, pfx_len))
			continue;
		nmatch++;
		if (first == NULL) {
			first = cand;
			while (first[agree] != '\0')
				agree++;
			continue;
		}
		while (agree > pfx_len &&
		    !complete_prefix_match(cand, first, agree))
			agree--;
	}

	if (nmatch == 0)
		return;

	if (agree > pfx_len) {
		ln->l_pos = pfx_len;
		while (pfx_len < agree)
			line_insert(ln, first[pfx_len++]);
		if (nmatch == 1)
			line_insert(ln, ' ');
		return;
	}

	/*
	 * Nothing to extend: list the candidates; the caller redraws the
	 * prompt below.
	 */
	puts("\n");
	puts(ESC_FG_GRAY);
	for (i = 0; i < nbuiltin + sh_progs_n; i++) {
		size_t	w;

		cand = complete_candidate(i);
		if (cand == NULL)
			break;
		if (!complete_prefix_match(cand, ln->l_buf, pfx_len))
			continue;
		puts(cand);
		for (w = 0; cand[w] != '\0'; w++)
			continue;
		while (w < 16) {
			putchar(' ');
			w++;
		}
	}
	puts("\n");
	puts(ESC_RESET);
	prompt_build();
}

static void
hist_add(const char *s)
{
	int	i;

	if (s[0] == '\0')
		return;
	if (sh_hist_n > 0 && streq(sh_hist[sh_hist_n - 1], s))
		return;	/* the same command twice is one thing to recall */

	if (sh_hist_n == SH_HIST_MAX) {
		for (i = 0; i + 1 < SH_HIST_MAX; i++) {
			size_t	j;

			for (j = 0; j < SH_LINE_MAX; j++)
				sh_hist[i][j] = sh_hist[i + 1][j];
		}
		sh_hist_n--;
	}
	for (i = 0; i + 1 < (int)SH_LINE_MAX && s[i] != '\0'; i++)
		sh_hist[sh_hist_n][i] = s[i];
	sh_hist[sh_hist_n][i] = '\0';
	sh_hist_n++;
}

static void
repl(void)
{
	struct sh_keypress	kp;
	struct sh_line		ln;
	char			 pending[SH_LINE_MAX];
	char			*argv[SH_ARGC_MAX];
	int			 argc;
	int			 browse;	/* -1 = editing the live line */

	line_clear(&ln);
	browse = -1;
	pending[0] = '\0';
	prompt();

	for (;;) {
		read_key(&kp);

		switch (kp.kp_key) {
		case SH_K_EOF:
			puts(ESC_FG_RED);
			puts("sh: read failed, exiting\n");
			puts(ESC_RESET);
			return;

		case SH_K_LEFT:
			if (ln.l_pos > 0)
				ln.l_pos--;
			break;
		case SH_K_RIGHT:
			if (ln.l_pos < ln.l_len)
				ln.l_pos++;
			break;
		case SH_K_HOME:
			ln.l_pos = 0;
			break;
		case SH_K_END:
			ln.l_pos = ln.l_len;
			break;
		case SH_K_DELETE:
			line_delete(&ln);
			break;

		case SH_K_UP:
			/*
			 * Leaving the live line saves it in `pending', so
			 * coming back down restores what was being typed.
			 */
			if (browse + 1 < sh_hist_n) {
				if (browse < 0) {
					size_t	i;

					for (i = 0; i <= ln.l_len; i++)
						pending[i] = ln.l_buf[i];
				}
				browse++;
				line_set(&ln, sh_hist[sh_hist_n - 1 - browse]);
			}
			break;
		case SH_K_DOWN:
			if (browse >= 0) {
				browse--;
				if (browse < 0)
					line_set(&ln, pending);
				else
					line_set(&ln,
					    sh_hist[sh_hist_n - 1 - browse]);
			}
			break;

		case SH_K_CHAR:
			switch (kp.kp_ch) {
			case '\r':
			case '\n':
				puts("\n");
				ln.l_buf[ln.l_len] = '\0';
				hist_add(ln.l_buf);
				argc = split_argv(ln.l_buf, argv,
				    SH_ARGC_MAX);
				if (argc > 0)
					last_status = dispatch(argc, argv);
				line_clear(&ln);
				browse = -1;
				pending[0] = '\0';
				prompt();
				continue;

			case 0x08:	/* Backspace */
			case 0x7F:
				line_erase(&ln);
				break;

			case 0x01:	/* ^A */
				ln.l_pos = 0;
				break;
			case 0x05:	/* ^E */
				ln.l_pos = ln.l_len;
				break;
			case 0x02:	/* ^B */
				if (ln.l_pos > 0)
					ln.l_pos--;
				break;
			case 0x06:	/* ^F */
				if (ln.l_pos < ln.l_len)
					ln.l_pos++;
				break;
			case 0x04:	/* ^D -- delete, never exit */
				line_delete(&ln);
				break;
			case 0x0B:	/* ^K -- kill to end of line */
				ln.l_len = ln.l_pos;
				ln.l_buf[ln.l_len] = '\0';
				break;
			case 0x15:	/* ^U -- kill to start of line */
				while (ln.l_pos > 0)
					line_erase(&ln);
				break;
			case 0x17:	/* ^W -- kill the word behind */
				line_erase_word(&ln);
				break;
			case '\t':
				line_complete(&ln);
				break;
			case 0x0C:	/* ^L -- clear and start again */
				builtin_clear();
				prompt();
				continue;

			/*
			 * Ctrl-C at the prompt (no foreground job): abandon
			 * the line and reprint.
			 */
			case 0x03:
				puts(ESC_FG_RED);
				puts("^C\n");
				puts(ESC_RESET);
				line_clear(&ln);
				browse = -1;
				pending[0] = '\0';
				prompt();
				continue;

			default:
				if ((unsigned char)kp.kp_ch >= 0x20 &&
				    (unsigned char)kp.kp_ch <= 0x7E)
					line_insert(&ln, kp.kp_ch);
				break;
			}
			break;

		default:
			/*
			 * A key with no meaning here (Insert, a page key,
			 * Escape) is dropped, never typed into the line.
			 */
			break;
		}

		line_refresh(&ln);
	}
}

int
main(void)
{

	g_kbd_stream = dev_open_stream("kbd");
	if (g_kbd_stream == MACH_PORT_NULL) {
		puts("sh: dev_open_stream('kbd') failed\n");
		return (1);
	}
	g_clock_port = bootstrap_lookup(SVC_CLOCK_NAME);
	g_stats_port = bootstrap_lookup(SVC_STATS_NAME);
	fetch_progs();

	/*
	 * Claim the screen: erase it, make rows 3-25 the scrolling region
	 * and start inside it.  Everything from here on, spawned programs
	 * included, lives in the region; rows 1-2 are paint_status_bar's.
	 */
	puts(ESC_CLR_SCR);
	puts(ESC_REGION_BODY);
	puts(ESC_BODY_HOME);
	paint_splash();
	repl();

	(void)mach_port_deallocate(g_kbd_stream);
	if (g_clock_port != MACH_PORT_NULL)
		(void)mach_port_deallocate(g_clock_port);
	if (g_stats_port != MACH_PORT_NULL)
		(void)mach_port_deallocate(g_stats_port);
	return (0);
}
