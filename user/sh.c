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

#define	ESC_RESET	"\x1b[0m"
#define	ESC_REVERSE	"\x1b[7m"
#define	ESC_BOLD	"\x1b[1m"
#define	ESC_FG_RED	"\x1b[1;31m"
#define	ESC_FG_GREEN	"\x1b[1;32m"
#define	ESC_FG_YELLOW	"\x1b[1;33m"
#define	ESC_FG_BLUE	"\x1b[1;34m"
#define	ESC_FG_MAGENTA	"\x1b[1;35m"
#define	ESC_FG_CYAN	"\x1b[1;36m"
#define	ESC_FG_WHITE	"\x1b[1;37m"
#define	ESC_FG_GRAY	"\x1b[0;37m"
#define	ESC_CLR_SCR	"\x1b[2J\x1b[H"
#define	ESC_SAVE_CUR	"\x1b[s"
#define	ESC_REST_CUR	"\x1b[u"
#define	ESC_HOME	"\x1b[1;1H"
#define	ESC_EOL_CLR	"\x1b[K"

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
 * paint_status_bar: lift the cursor to (1,1), emit a reverse-video
 * one-line dashboard, then put the cursor back where the caller had
 * it.  Inherits the SGR attribute through the trailing ESC_EOL_CLR so
 * the bar's reverse-video background runs the full 80 columns even
 * though we only wrote a partial line of glyph text.
 *
 * Three live data fields: total ram + KiB used, live task count,
 * uptime in HH:MM:SS.  If a service query fails the values just show
 * as zero -- preferable to suppressing the bar entirely.
 */
static void
paint_status_bar(void)
{
	struct svc_clock_reply	ck;
	struct svc_stats_reply	st;
	uint64_t		used_kib;
	uint64_t		total_kib;
	uint64_t		s, m, h;

	(void)fetch_clock(&ck);
	(void)fetch_stats(&st);

	used_kib  = st.sr_pmm_used_pages * 4ull;
	total_kib = st.sr_pmm_total_pages * 4ull;

	s = ck.cr_uptime_ms / 1000ull;
	h = s / 3600ull;
	s = s - h * 3600ull;
	m = s / 60ull;
	s = s - m * 60ull;

	puts(ESC_SAVE_CUR);
	puts(ESC_HOME);
	puts(ESC_REVERSE);
	printf(" style9-os(9)     ram %llu/%lluK     tasks %llu     uptime "
	    "%02llu:%02llu:%02llu",
	    (unsigned long long)used_kib,
	    (unsigned long long)total_kib,
	    (unsigned long long)st.sr_task_count,
	    (unsigned long long)h,
	    (unsigned long long)m,
	    (unsigned long long)s);
	puts(ESC_EOL_CLR);
	puts(ESC_RESET);
	puts(ESC_REST_CUR);
}

/*
 * paint_splash: the manpage-style welcome.  Drawn once at startup;
 * scrolls off naturally as the user works.  Layout is a 5-line ASCII
 * '9' glyph on the left and NAME / SYSTEM / SEE ALSO blocks aligned
 * to the right, mirroring a real BSD manual page.
 *
 * Colour usage:
 *	cyan-bold	the '9' glyph and the manpage title bar
 *	white-bold	section headers (NAME, SYSTEM, SEE ALSO)
 *	gray		key labels (kernel, ram, ...)
 *	white-bold	values (so they pop against the gray)
 *	gray		the trailing hint line
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

	puts("\n");
	puts(ESC_FG_CYAN);
	puts("     ___\n");
	puts("    / _ \\      ");
	puts(ESC_FG_WHITE);
	puts("STYLE9-OS(9)");
	puts(ESC_FG_GRAY);
	puts("                          style9-os Manual\n");
	puts(ESC_FG_CYAN);
	puts("   | (_) |\n");
	puts("    \\__, |    ");
	puts(ESC_FG_WHITE);
	puts("NAME\n");
	puts(ESC_FG_CYAN);
	puts("      /_/");
	puts(ESC_FG_GRAY);
	puts("        style9-os -- BSD-flavoured x86_64 microkernel\n");
	puts("\n");

	puts("               ");
	puts(ESC_FG_WHITE);
	puts("SYSTEM\n");
	puts(ESC_FG_GRAY);
	puts("                  arch    : x86_64\n");
	printf("                  ram     : %llu / %llu KiB\n",
	    (unsigned long long)used_kib,
	    (unsigned long long)total_kib);
	printf("                  tasks   : %llu live\n",
	    (unsigned long long)st.sr_task_count);
	puts("                  shell   : sh.elf  (libstyle9, ring 3)\n");
	puts("\n");

	puts("               ");
	puts(ESC_FG_WHITE);
	puts("SEE ALSO\n");
	puts(ESC_FG_GRAY);
	puts("                  style(9), help(1)\n");
	puts("\n");

	puts("  type `help' to see available commands.\n");
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
 * prompt: repaint the persistent status bar, then emit the colour-
 * coded prompt at the cursor's current row.  '[ok]' / '[err N]' lights
 * green/red respectively; the '$' is always bright green so it stands
 * out as the input anchor.
 */
static void
prompt(void)
{

	paint_status_bar();

	if (last_status == 0) {
		puts(ESC_FG_GREEN);
		puts("[ok]");
	} else {
		puts(ESC_FG_RED);
		printf("[err %d]", last_status);
	}
	puts(ESC_FG_GREEN);
	puts(" $ ");
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
 * Coloured builtin/program listing.  Mirrors a manpage SYNOPSIS block:
 * section header in bold-white, command names in yellow, descriptions
 * in gray.  Spawnables get the same treatment so the visual rhythm
 * stays consistent regardless of whether the user reads down the
 * builtins or the spawnables.
 */
static void
builtin_help(void)
{
	const char	*p;
	size_t		 i;

	puts(ESC_FG_WHITE);
	puts("style9-os shell -- builtins:\n");
	puts(ESC_RESET);

	puts(ESC_FG_YELLOW); puts("  help        ");
	puts(ESC_FG_GRAY);   puts("show this list\n");
	puts(ESC_FG_YELLOW); puts("  echo ARGS   ");
	puts(ESC_FG_GRAY);   puts("print arguments\n");
	puts(ESC_FG_YELLOW); puts("  clear       ");
	puts(ESC_FG_GRAY);   puts("clear screen and repaint the splash\n");
	puts(ESC_FG_YELLOW); puts("  about       ");
	puts(ESC_FG_GRAY);   puts("print version banner\n");

	puts("\n");
	puts(ESC_FG_WHITE);
	puts("spawnable programs:\n");
	puts(ESC_FG_YELLOW);
	for (i = 0; known_progs[i] != NULL; i++) {
		p = known_progs[i];
		puts("  ");
		puts(p);
		puts("\n");
	}
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

static void
builtin_about(void)
{
	struct svc_clock_reply	ck;
	struct svc_stats_reply	st;
	uint64_t		s;

	(void)fetch_clock(&ck);
	(void)fetch_stats(&st);
	s = ck.cr_uptime_ms / 1000ull;

	puts(ESC_FG_WHITE);
	puts("style9-os");
	puts(ESC_FG_GRAY);
	puts(" -- a BSD-flavoured x86_64 microkernel.\n");
	puts("  written end-to-end in the style(9) BSD KNF convention, "
	    "hence the name.\n");
	puts("\n");
	puts(ESC_FG_WHITE); puts("  sh.elf");
	puts(ESC_FG_GRAY);
	puts(" -- libstyle9 ring-3 shell, phase 2.\n");
	printf("  uptime %llu.%03llu s, %llu task(s), %llu thread(s), "
	    "%llu ctx switches.\n",
	    (unsigned long long)s,
	    (unsigned long long)(ck.cr_uptime_ms % 1000ull),
	    (unsigned long long)st.sr_task_count,
	    (unsigned long long)st.sr_thread_count,
	    (unsigned long long)st.sr_ctx_switches);
	puts(ESC_RESET);
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

	/*
	 * Not a builtin -- hand to SYS_SPAWN.  argv[1..] are ignored for
	 * now; the kernel's progreg_spawn takes only a name.  Real argv
	 * delivery to the child waits on a stack-loader update.
	 */
	rv = spawn(argv[0]);
	if (rv <= 0) {
		puts(ESC_FG_RED);
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
