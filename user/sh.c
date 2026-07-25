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

/*
 * Child registry.  Every fg/bg job the shell spawns lands here as a
 * (task_id, taskport_name) pair so the shell holds the capability
 * needed to kill the child via SYS_TASK_KILL.  Entries are written
 * once at spawn time and never explicitly evicted -- the kernel
 * recycles task_ids and port names lazily; on the next collision the
 * shell will simply overwrite the stale row.  16 slots is well in
 * excess of any plausible in-flight job count for this kernel.
 */
#define	SH_CHILD_MAX	16
struct sh_child {
	uint64_t		c_task_id;
	mach_port_name_t	c_taskport;
};
static struct sh_child	sh_children[SH_CHILD_MAX];

/*
 * Foreground job state, set by dispatch() right before wait_child(),
 * cleared after the wait returns.  wait_child uses fg_taskport to
 * deliver a kill when the user hits Ctrl-C.  Single-job-at-a-time
 * model (no real job-control + & yet); the slot is enough.
 */
static long			fg_task_id;
static mach_port_name_t		fg_taskport;

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
#define	ESC_HIDE_CUR	"\x1b[?25l"
#define	ESC_SHOW_CUR	"\x1b[?25h"

/*
 * The chrome, and the two rows it owns.
 *
 * Rows one and two are the status line and the rule under it; rows
 * three to twenty-five are where everything else happens.  The shell
 * says so once, with DECSTBM, and the terminal keeps the promise from
 * then on -- output that reaches the bottom of the screen scrolls the
 * region and leaves the header where it is.
 *
 * Before this, the bar was repainted at every prompt and scrolled off
 * the top by the first command that filled the screen, leaving its rule
 * behind as a stray line in the middle of the session.  Repainting more
 * often could never have fixed that: between two prompts a foreground
 * job is free to print, and the bar has to survive it.
 */
#define	ESC_REGION_BODY	"\x1b[3;25r"
#define	ESC_REGION_ALL	"\x1b[r"
#define	ESC_BODY_HOME	"\x1b[3;1H"

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

/* ---- keys, not bytes ---------------------------------------------- */

/*
 * A keypress, decoded.
 *
 * dev/kbd.c has been turning Up into the three bytes "\x1b[A" since it
 * was written, and the pager below has had a state machine to take them
 * apart for just as long.  The line editor did not: it dropped the
 * escape as unprintable and then typed the '[' and the 'A' into the
 * command line, so pressing Up at the prompt inserted "[A".  The decoder
 * existed; it was just in the wrong place, doing it for one of the two
 * things that reads the keyboard.
 *
 * So it lives here now and both of them use it.  A key that this
 * terminal does not produce comes back as SH_K_UNKNOWN rather than as
 * its bytes -- an unrecognised sequence must not degrade into typing
 * itself, which is the whole bug.
 *
 * A lone Escape is indistinguishable from the start of a sequence
 * without a timer, so read_key blocks after one until the next byte
 * arrives, exactly as the pager's own machine used to.  Every consumer
 * here reaches Escape through a key that follows it.
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
	 * Parameters, then a final byte.  Only the first parameter is
	 * kept: every sequence this keyboard emits has at most one, and a
	 * consumer that needed more would be parsing a terminal reply
	 * rather than a keypress.
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
 * paint_status_bar: lift the cursor to (1,1) and paint the two-row
 * header -- app-name + right-aligned uptime on row 1, a thin horizontal
 * rule on row 2.  No reverse-video bar; the chrome here is meant to
 * read as quiet typography rather than a 1990s curses banner.
 *
 * The rows are outside the scrolling region, so this is a refresh of
 * the clock rather than a rescue of a bar that has been carried away.
 * The cursor is hidden across the trip: it is programmed once per
 * write, and without this the underline is seen jumping to the top of
 * the screen and back on every prompt.
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

	puts(ESC_HIDE_CUR);
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
	puts(ESC_SHOW_CUR);
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

	/*
	 * One blank row under the rule for air.  It used to be two, plus a
	 * third the terminal ate by wrapping the eighty-column status line
	 * into a row of its own -- which is what put the rule on top of
	 * the NAME section and made this splash a section shorter than it
	 * reads here.
	 */
	puts("\n");

	puts(ESC_FG_WHITE); puts("  NAME           ");
	puts(ESC_FG_GRAY);  puts("style9-os -- BSD-flavoured x86_64 "
	    "kernel with Mach IPC\n");
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

/* ---- the prompt, as bytes and as a width -------------------------- */

/*
 * The prompt has to be two things at once now.
 *
 * It is a string to emit -- with the SGR sequences that colour it --
 * and it is a number of columns, because the line editor repaints the
 * whole line on every keystroke and then has to put the cursor back at
 * a column it can only work out by counting.  The escape sequences are
 * in the first and not in the second, which is exactly the distinction
 * that gets lost when a prompt is just printed.
 *
 * On success it is a plain bold-white '$ '; on failure a subdued 'err N'
 * in light red comes first (no brackets, no shouting) -- closer to how a
 * modern shell surfaces $? than to a permanent status indicator.
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
	puts(ESC_FG_WHITE); puts("  kill     ");
	puts(ESC_FG_GRAY);  puts("kill <task_id> -- terminate a child of this shell\n");

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

	/*
	 * ED 2 homes the cursor to the top-left of the SCREEN, which is
	 * inside the chrome.  Step back into the region explicitly rather
	 * than trusting the erase to know about it -- the region is a
	 * property of scrolling, not of addressing.
	 */
	puts(ESC_CLR_SCR);
	puts(ESC_BODY_HOME);
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
 * scribbles past the static buffers.  The longest page in docs/man is
 * port.9 at 561 lines -- the comment here used to say ~300, which was
 * wrong and would have made the cap look roomier than it is -- so 4096
 * is a little over seven times the worst case.
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
	 * The pager owns the whole screen while it runs, so it takes the
	 * scrolling region back and hides the cursor -- a manual page has
	 * no use for the shell's status bar, and an underline parked
	 * wherever the last line ended is just a distraction on a page
	 * that is repainted whole.  Both are handed back on the way out.
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

	/*
	 * Release the OOL-installed range so repeated `man` invocations do
	 * not leak one anonymous mapping each.  man_release is best-effort
	 * -- a failure (range no longer matches an entry) is harmless and
	 * silently absorbed; the buffer stays around until task exit.
	 */
	(void)man_release(text, len);
}

/* ---- spawn + yield-spin wait ------------------------------------- */

/*
 * Drop the (task_id, taskport) pair into the first empty slot, or
 * overwrite slot 0 if every slot is full (lossy oldest-first; we
 * don't have a real LRU and don't need one for this kernel).
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
 * Look up the saved taskport for a task_id.  Returns MACH_PORT_NULL
 * if no matching entry -- the user typed `kill 999` on an unknown id,
 * or the row aged out.
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
 * Foreground-wait with Ctrl-C handling.  Replaces the bare yield-spin
 * with a kbd-stream peek every 50 ms: any byte that arrives during
 * the wait is checked for ASCII 0x03 (Ctrl-C, courtesy of kbd.c's
 * Ctrl-folding).  On 0x03 we issue SYS_TASK_KILL against the fg
 * job's taskport and keep spinning until task_alive returns false --
 * the kill is async, so the task may take a yield or two to actually
 * retire (see detection-site model in kern/task.h).
 *
 * Non-^C bytes are discarded silently.  A real shell would re-route
 * them into a type-ahead buffer the next prompt consumes; today the
 * kbd ring is the buffer of record and the user just loses those
 * bytes if they typed during a foreground job.  Cheap to fix later
 * (drain the kbd into a local scratch + flush on prompt) and not
 * worth the surface here.
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
 * atou64: tiny stdtoul.  Returns 0 on any junk (sh `kill 0` then
 * gets the proper "task 0 not found" error from the registry lookup).
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
 * `kill <task_id>` builtin: look up the saved taskport for the named
 * task_id + call SYS_TASK_KILL with it.  Capability-based, so
 * `kill 1` (init) returns MACH_E_RIGHT cleanly (sh never had the
 * taskport for kernel-side tasks).  Async: by the time `kill` returns
 * the target may not have fully retired yet; user can probe with
 * the `tasks` program if they care about the exact moment.
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
 * dispatch: route the argv to a builtin or hand to SYS_SPAWN.  Returns
 * the status to propagate into last_status: 0 for builtin / successful
 * spawn, the negative SYS_E_* code if spawn returned an error.
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
	 * Not a builtin -- hand to SYS_SPAWN_ARGS so the child both
	 * receives the full command line (argv[0..argc-1]) AND we get a
	 * SEND right on its task-self port for Ctrl-C / the kill builtin.
	 * argv[0] is the program name the kernel resolves in progreg.
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
 * The line being typed, and where in it the cursor is.
 *
 * The old editor had only a length: characters went on the end,
 * backspace took one off the end, and there was nowhere else to be.
 * Every editing key the keyboard sends -- and it has been sending all
 * of them -- either did nothing or typed its own escape sequence into
 * the buffer.
 *
 * With a position, editing is ordinary insert-and-delete at a point,
 * and the only real work is drawing: the line is repainted whole on
 * every keystroke rather than patched incrementally, which is what
 * makes an insert in the middle cost the same code as an append.  One
 * write(2) per keystroke, and the terminal programs its cursor once for
 * it -- so the repaint is cheaper than the character-at-a-time echo it
 * replaces, which cost one write per byte.
 */
struct sh_line {
	char	l_buf[SH_LINE_MAX];
	size_t	l_len;
	size_t	l_pos;
};

/*
 * The console is eighty columns and says so in dev/tty.h; there is no
 * window-size ioctl to ask, and inventing one to tell the shell a
 * constant would be ceremony.  When a resizable terminal exists this
 * becomes a query.
 */
#define	SH_COLS		80

/*
 * History.  A plain array, oldest first, that shifts when it fills --
 * sixteen entries is more than a session of this shell holds and the
 * shift costs a memmove of something nobody is waiting on.
 */
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
 * Repaint the line.
 *
 * The window slides so the cursor is always on screen: a line longer
 * than the terminal scrolls sideways rather than wrapping, because a
 * wrapped line cannot be repainted from a single carriage return and
 * every edit would leave the rows below it wrong.  This is the same
 * trick every one-line editor uses, and it is why the buffer may be
 * 256 bytes on an 80-column screen without the two numbers having to
 * agree.
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
 * A fresh prompt: refresh the chrome, leave a blank row, then draw an
 * empty line.  The prompt is emitted BY line_refresh rather than
 * printed here, so there is exactly one piece of code that knows where
 * the text of a line begins.
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

/* Remove the character AT the cursor -- Delete, and Ctrl-D mid-line. */
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

/* Remove the character BEFORE the cursor -- Backspace. */
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
			 * Stepping off the live line saves it, so walking
			 * back down to the bottom returns what was being
			 * typed rather than an empty prompt.
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
			case 0x0C:	/* ^L -- clear and start again */
				builtin_clear();
				prompt();
				continue;

			/*
			 * Ctrl-C at the prompt with no foreground job:
			 * abandon the line and reprint.  wait_child's ^C
			 * path cannot be reached in this window, since
			 * dispatch has not been called yet.
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
			 * A key with no meaning here -- Insert, a page
			 * key, an escape.  Dropped, and pointedly NOT
			 * echoed: typing its bytes into the command line
			 * is the bug this editor was written to fix.
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

	/*
	 * Claim the screen: erase it, hand rows three to twenty-five to
	 * the scrolling region, and start writing inside it.  Everything
	 * after this line -- the splash, every prompt, every program this
	 * shell spawns -- lives in the region, and the two rows above it
	 * belong to paint_status_bar alone.
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
