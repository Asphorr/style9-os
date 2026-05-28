/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * sh.elf -- the first real ring-3 shell for style9-os.
 *
 * Boot-time init spawns this as the user-facing surface (kern/shell.c
 * is the old in-kernel REPL; it stays in the tree as a fallback but is
 * no longer wired into kmain).  The interaction shape is:
 *
 *	1. bootstrap_lookup("dev/kbd") -> control port
 *	2. RPC DEV_OP_OPEN_STREAM       -> stream SEND right (kbd_input_port)
 *	3. for (;;) {
 *		recv one mach_msg from the stream, msgh_id == one byte;
 *		line-edit; on '\n' split argv, dispatch builtin or SYS_SPAWN,
 *		yield-spin until the spawned child drops off the live list.
 *	   }
 *
 * Built-ins:
 *	help		list builtins + known spawnable programs
 *	echo ARGS...	print arguments separated by spaces
 *	clear		ANSI clear-screen + repaint splash
 *	about		multi-line banner + system info
 *
 * Anything else gets handed straight to SYS_SPAWN; the kernel's
 * progreg either resolves it (returns task_id) or returns SYS_E_INVAL.
 *
 * TUI surface
 * ---
 * Row 0 is a persistent reverse-video status bar (style9-os(9) + live
 * task count + ram + uptime), repainted on every prompt.  The shell
 * opens with a man-page-style splash -- big ASCII '9' + NAME / SYSTEM
 * / SEE ALSO blocks -- which scrolls off naturally as the user works.
 * The prompt itself is colour-coded: bright green '$' on success,
 * '[err N]' in red when the last spawn failed.
 *
 * Wait-for-child is a yield-spin against SYS_TASK_ALIVE.  Cheap and
 * adequate for a cooperative scheduler; a real exit-notification port
 * (and proper $? carry-through) is a phase-3 conversation.
 */

#define	SH_LINE_MAX	256
#define	SH_ARGC_MAX	8

/*
 * Hard-coded program list.  Mirrors the registrations in
 * kern/progreg.c.  Phase 3 replaces this with a Mach "progreg" service
 * the way "tasks" works today; until then keep the list in sync by
 * hand when adding a user program.
 */
static const char *known_progs[] = {
	"hello",
	"clock",
	"tasks",
	"sh",
	NULL,
};

/*
 * Cached service ports.  Looked up once at startup; the splash and
 * the per-prompt status bar both pull from them.  MACH_PORT_NULL if
 * the lookup failed -- the bar then displays "?" in place of the data.
 */
static mach_port_name_t	g_kbd_stream;
static mach_port_name_t	g_clock_port;
static mach_port_name_t	g_stats_port;

/*
 * Last-command return status.  spawn() returns task_id > 0 on success
 * or a negative SYS_E_* on failure; the prompt paints '[err N]' when
 * non-zero.  Builtins always succeed.
 */
static int	last_status;

/* ---- ANSI escape constants --------------------------------------- */

/*
 * Restrained palette: white-bold for emphasis, gray for body,
 * dark-gray for the chrome rules, light-red for the rare error.
 * The 'green prompt + yellow keywords' style from v1 is gone --
 * the aesthetic here is closer to a modern Terminal.app prompt
 * than to a hobby-OS splash.
 */
#define	ESC_RESET	"\x1b[0m"
#define	ESC_FG_RED	"\x1b[0;91m"	/* light red, not bold */
#define	ESC_FG_WHITE	"\x1b[1;37m"	/* bold white -- headers, prompt */
#define	ESC_FG_GRAY	"\x1b[0;37m"	/* default body colour          */
#define	ESC_FG_DGRAY	"\x1b[1;30m"	/* dark gray -- rules, chrome   */
#define	ESC_CLR_SCR	"\x1b[2J\x1b[H"
#define	ESC_SAVE_CUR	"\x1b[s"
#define	ESC_REST_CUR	"\x1b[u"
#define	ESC_HOME	"\x1b[1;1H"

/* ---- single-byte read from the kbd stream port ------------------- */

/*
 * read_byte: park in mach_msg_recv on the kbd stream port and return
 * the byte the driver thread tagged into msgh_id.  Negative on error,
 * which in normal operation cannot happen: the driver is the kernel
 * itself and the port has SEND held by the driver thread for the
 * lifetime of the system, so we treat any failure as fatal.
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

/* ---- service-query helpers --------------------------------------- */

/*
 * fetch_clock / fetch_stats: one RPC each into the cached service
 * port.  Return 0 on success and -1 on any failure; on failure the
 * reply struct is zeroed so the caller can render "?" without an
 * explicit branch on every field.
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
 * paint_hr: emit a full-width horizontal rule at the cursor's current
 * row.  Uses CP437 0xC4 single-line glyph (─); the VGA text-mode font
 * has it natively at that codepoint, and our tty state machine passes
 * non-ESC bytes straight through.  Built as a single 80-byte buffer +
 * trailing newline so it's one write() not eighty putchar()s.
 */
static void
paint_hr(void)
{
	char	line[81];
	int	i;

	line[0]  = ' ';
	for (i = 1; i < 79; i++)
		line[i] = (char)0xc4;
	line[79] = ' ';
	line[80] = '\n';
	(void)write(line, 81);
}

/*
 * paint_status_bar: lift the cursor to (1,1) and paint a two-row
 * header -- app-name + right-aligned uptime on row 0, a thin horizontal
 * rule on row 1.  No reverse-video bar; the chrome here is meant to
 * read as quiet typography rather than a 1990s curses banner.
 *
 * Both rows are repainted on every prompt so they survive scrolling:
 * since QEMU's VGA scroll moves rows up and row 0 drops off the top,
 * the next paint sees an empty top row to overwrite cleanly.
 */
static void
paint_status_bar(void)
{
	struct svc_clock_reply	ck;
	uint64_t		s, m, h;

	(void)fetch_clock(&ck);
	s = ck.cr_uptime_ms / 1000ull;
	h = s / 3600ull;
	s = s - h * 3600ull;
	m = s / 60ull;
	s = s - m * 60ull;

	puts(ESC_SAVE_CUR);
	puts(ESC_HOME);
	puts(ESC_FG_GRAY);
	/*
	 * 80 columns total: 1sp + "style9-os(9)" (12) + 57sp + uptime
	 * (8) + 1sp + nl + 1.  "%58s" right-aligns the 8-char uptime to
	 * column 79, leaving column 80 as a trailing space.
	 */
	{
		char	tbuf[16];
		tbuf[0] = (char)('0' + ((unsigned)(h / 10) % 10));
		tbuf[1] = (char)('0' + ((unsigned)h % 10));
		tbuf[2] = ':';
		tbuf[3] = (char)('0' + ((unsigned)(m / 10) % 10));
		tbuf[4] = (char)('0' + ((unsigned)m % 10));
		tbuf[5] = ':';
		tbuf[6] = (char)('0' + ((unsigned)(s / 10) % 10));
		tbuf[7] = (char)('0' + ((unsigned)s % 10));
		tbuf[8] = '\0';
		printf(" style9-os(9)%66s \n", tbuf);
	}
	puts(ESC_FG_DGRAY);
	paint_hr();
	puts(ESC_RESET);
	puts(ESC_REST_CUR);
}

/*
 * paint_splash: a quiet manpage-style welcome.  No ASCII '9' glyph;
 * the section-9 reference lives in the title and in 'SEE ALSO' --
 * the typography itself is the logo.  Lots of vertical whitespace
 * so the eye can rest between sections; left margin two columns,
 * label gutter at column three, content gutter at column eighteen.
 *
 * Colour usage:
 *	white-bold	section headers (NAME, SYSTEM, SEE ALSO)
 *	gray		body text + value columns
 *	dark gray	horizontal rules
 */
static void
paint_splash(void)
{
	struct svc_stats_reply	st;
	uint64_t		used_kib;
	uint64_t		total_kib;

	(void)fetch_stats(&st);
	used_kib  = st.sr_pmm_used_pages * 4ull;
	total_kib = st.sr_pmm_total_pages * 4ull;

	/* Two blank rows after the status header for breathing room. */
	puts("\n\n");

	puts(ESC_FG_WHITE); puts("  NAME           ");
	puts(ESC_FG_GRAY);  puts("style9-os -- BSD-flavoured x86_64 "
	    "microkernel\n");
	puts("\n");

	puts(ESC_FG_WHITE); puts("  SYSTEM         ");
	puts(ESC_FG_GRAY);  puts("arch     x86_64\n");
	printf("                 ram      %llu / %llu KiB\n",
	    (unsigned long long)used_kib,
	    (unsigned long long)total_kib);
	printf("                 tasks    %llu live\n",
	    (unsigned long long)st.sr_task_count);
	puts("                 shell    sh.elf\n");
	puts("\n");

	puts(ESC_FG_WHITE); puts("  SEE ALSO       ");
	puts(ESC_FG_GRAY);  puts("style(9), help(1)\n");
	puts("\n");

	puts(ESC_FG_DGRAY);
	paint_hr();
	puts(ESC_RESET);
	puts("\n");
}

/* ---- echo helpers ------------------------------------------------- */

static void
echo_char(char c)
{

	putchar(c);
}

static void
echo_backspace(void)
{

	/* \b moves cursor left, ' ' wipes the glyph, \b moves left again. */
	puts("\b \b");
}

/*
 * prompt: repaint the persistent status bar, then emit the prompt at
 * the cursor's current row.  On success the prompt is a plain bold-
 * white '$ '; on failure we prepend a subdued 'err N ' in light-red
 * (no brackets, no shouting) and then the same plain '$ '.  Closer to
 * how a modern shell surfaces $? than to a permanent status indicator.
 */
static void
prompt(void)
{

	paint_status_bar();
	puts("\n");

	if (last_status != 0) {
		puts(ESC_FG_RED);
		printf("err %d  ", last_status);
	}
	puts(ESC_FG_WHITE);
	puts("$ ");
	puts(ESC_RESET);
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

/*
 * builtin_help: terse "two-column form" -- command on the left, one-
 * line description on the right.  No section headers; spawnables get
 * one inline row separated by two spaces, the way a tab-completion
 * preview reads.  Bold-white commands, gray descriptions.
 */
static void
builtin_help(void)
{
	size_t	i;

	puts(ESC_FG_WHITE); puts("  help     ");
	puts(ESC_FG_GRAY);  puts("show this list\n");
	puts(ESC_FG_WHITE); puts("  echo     ");
	puts(ESC_FG_GRAY);  puts("print arguments\n");
	puts(ESC_FG_WHITE); puts("  clear    ");
	puts(ESC_FG_GRAY);  puts("clear screen and repaint the splash\n");
	puts(ESC_FG_WHITE); puts("  about    ");
	puts(ESC_FG_GRAY);  puts("version banner + live counters\n");
	puts(ESC_FG_WHITE); puts("  ool      ");
	puts(ESC_FG_GRAY);  puts("OOL Mach IPC round-trip via svc/echool\n");
	puts(ESC_FG_WHITE); puts("  man      ");
	puts(ESC_FG_GRAY);  puts("show a manual page (try: man port)\n");

	puts("\n");
	puts(ESC_FG_WHITE); puts("  spawn    ");
	puts(ESC_FG_GRAY);
	for (i = 0; known_progs[i] != NULL; i++) {
		if (i > 0)
			puts("    ");
		puts(known_progs[i]);
	}
	puts("\n");
	puts(ESC_RESET);
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
 * builtin_clear: erase the screen and redraw the splash so the freshly
 * cleared screen still has the welcome banner visible.  The status
 * bar repaint happens at the next prompt() call, so we don't need to
 * touch it here.
 */
static void
builtin_clear(void)
{

	puts(ESC_CLR_SCR);
	paint_splash();
}

/*
 * builtin_ool: round-trip a small buffer through the kernel's echool
 * service as a single OOL descriptor, verify the kernel-computed
 * FNV-1a matches the client-computed one byte-for-byte.  Proves that
 * userspace can construct a valid OOL wire-format from its own VA
 * space, that the kernel parses the variable-stride descriptor area
 * correctly, and that the sender's pages are reachable from the
 * special-port dispatcher.
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
	puts("  style9-os -- a BSD-flavoured x86_64 microkernel.\n");
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
 * Tiny in-shell pager modelled on less(1).
 *
 * Takes a flat text buffer plus a title, paints PAGER_ROWS lines at a
 * time, and lets the user scroll with:
 *
 *	Space, PgDn, Ctrl-F	page down
 *	b, PgUp, Ctrl-B		page up
 *	j, Enter, Down arrow	line down
 *	k, Up arrow		line up
 *	g			top
 *	G			bottom
 *	q, ESC ESC		quit
 *
 * Arrow keys arrive as multi-byte CSI sequences (\x1b[A etc.), so the
 * read loop runs a three-state machine (NORMAL -> ESC -> CSI) and
 * collects an optional numeric argument before the final letter.
 *
 * Line metadata is cached up front: an array of (offset, length) tuples
 * per source line.  Capped at PAGER_MAX_LINES so a runaway input never
 * scribbles past the static buffers.  port.9 rendered is ~300 lines;
 * 4096 leaves ten-fold headroom.
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
	size_t	end;
	size_t	i;

	puts(ESC_CLR_SCR);

	end = top + PAGER_SCREEN_ROWS;
	if (end > total_lines)
		end = total_lines;

	for (i = top; i < end; i++) {
		(void)write(text + pager_line_off[i], pager_line_len[i]);
		putchar('\n');
	}
	for (i = end - top; i < PAGER_SCREEN_ROWS; i++)
		putchar('\n');

	puts("\x1b[7m");	/* inverse video for the status bar */
	printf(" %s  %llu-%llu/%llu  "
	    "[Space/b page  j/k line  g/G top/bot  q quit] ",
	    title,
	    (unsigned long long)(top + 1),
	    (unsigned long long)end,
	    (unsigned long long)total_lines);
	puts(ESC_RESET);
}

static void
pager_show(const char *text, size_t len, const char *title)
{
	size_t	max_top;
	size_t	top;
	size_t	total_lines;
	int	act;
	int	c;
	int	csi_arg;
	int	state;	/* 0 = normal, 1 = saw ESC, 2 = inside CSI */

	if (text == NULL || len == 0)
		return;

	total_lines = pager_index_lines(text, len);
	if (total_lines == 0)
		return;

	max_top = total_lines > PAGER_SCREEN_ROWS ?
	    total_lines - PAGER_SCREEN_ROWS : 0;
	top      = 0;
	state    = 0;
	csi_arg  = 0;

	pager_repaint(text, total_lines, top, title);

	for (;;) {
		c = read_byte();
		if (c < 0)
			break;

		act = 0;

		if (state == 0) {
			if (c == 0x1b) {
				state = 1;
				continue;
			}
			if (c == 'q')
				break;
			if (c == ' ' || c == 0x06) {
				top += PAGER_SCREEN_ROWS;
				act = 1;
			} else if (c == 'b' || c == 0x02) {
				top = top >= PAGER_SCREEN_ROWS ?
				    top - PAGER_SCREEN_ROWS : 0;
				act = 1;
			} else if (c == 'j' || c == '\n' || c == '\r') {
				top++;
				act = 1;
			} else if (c == 'k') {
				if (top > 0)
					top--;
				act = 1;
			} else if (c == 'g') {
				top = 0;
				act = 1;
			} else if (c == 'G') {
				top = max_top;
				act = 1;
			}
		} else if (state == 1) {
			if (c == '[') {
				state   = 2;
				csi_arg = 0;
				continue;
			}
			/* bare ESC followed by anything (incl. ESC) quits */
			if (c == 0x1b)
				break;
			state = 0;
			continue;
		} else {
			if (c >= '0' && c <= '9') {
				csi_arg = csi_arg * 10 + (c - '0');
				continue;
			}
			state = 0;
			if (c == 'A') {
				if (top > 0)
					top--;
				act = 1;
			} else if (c == 'B') {
				top++;
				act = 1;
			} else if (c == '~' && csi_arg == 5) {
				top = top >= PAGER_SCREEN_ROWS ?
				    top - PAGER_SCREEN_ROWS : 0;
				act = 1;
			} else if (c == '~' && csi_arg == 6) {
				top += PAGER_SCREEN_ROWS;
				act = 1;
			}
		}

		if (act) {
			if (top > max_top)
				top = max_top;
			pager_repaint(text, total_lines, top, title);
		}
	}

	puts(ESC_CLR_SCR);
}

/*
 * builtin_man: bootstrap_lookup("man") + RPC for the requested page +
 * hand the OOL-installed text to the pager.  Title is "<name>(9)".
 * On not-found prints a short error to stdout and returns; on RPC
 * failure same shape but with the error code.
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
}

/* ---- spawn + yield-spin wait ------------------------------------- */

static void
wait_child(long task_id)
{

	if (task_id <= 0)
		return;
	while (task_alive((uint64_t)task_id))
		(void)yield();
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
 * dispatch: route the argv to a builtin or hand to SYS_SPAWN.  Returns
 * the status to propagate into last_status: 0 for builtin / successful
 * spawn, the negative SYS_E_* code if spawn returned an error.
 */
static int
dispatch(int argc, char *argv[])
{
	long	rv;

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

	/*
	 * Not a builtin -- hand to SYS_SPAWN.  argv[1..] are ignored for
	 * now; the kernel's progreg_spawn takes only a name.  Real argv
	 * delivery to the child waits on a stack-loader update.
	 */
	rv = spawn(argv[0]);
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
	wait_child(rv);
	return (0);
}

/* ---- line editor ------------------------------------------------- */

static void
repl(void)
{
	char	*argv[SH_ARGC_MAX];
	char	 line[SH_LINE_MAX];
	size_t	 len;
	int	 argc;
	int	 c;

	len = 0;
	prompt();

	for (;;) {
		c = read_byte();
		if (c < 0) {
			puts(ESC_FG_RED);
			puts("sh: read failed, exiting\n");
			puts(ESC_RESET);
			return;
		}

		if (c == '\r' || c == '\n') {
			putchar('\n');
			line[len] = '\0';
			argc = split_argv(line, argv, SH_ARGC_MAX);
			if (argc > 0)
				last_status = dispatch(argc, argv);
			len = 0;
			prompt();
			continue;
		}

		if (c == 0x08 || c == 0x7F) {
			if (len > 0) {
				len--;
				echo_backspace();
			}
			continue;
		}

		/* drop non-printables; tabs/CSI are not yet plumbed */
		if (c < 0x20 || c > 0x7E)
			continue;

		if (len + 1 >= SH_LINE_MAX)
			continue;	/* full -- backspace and retry */

		line[len++] = (char)c;
		echo_char((char)c);
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

	puts(ESC_CLR_SCR);
	paint_splash();
	repl();

	(void)mach_port_deallocate(g_kbd_stream);
	if (g_clock_port != MACH_PORT_NULL)
		(void)mach_port_deallocate(g_clock_port);
	if (g_stats_port != MACH_PORT_NULL)
		(void)mach_port_deallocate(g_stats_port);
	return (0);
}
