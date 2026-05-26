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
 *	clear		ANSI clear-screen
 *	about		single-line banner
 *
 * Anything else gets handed straight to SYS_SPAWN; the kernel's
 * progreg either resolves it (returns task_id) or returns SYS_E_INVAL.
 *
 * Wait-for-child is a yield-spin against SYS_TASK_ALIVE.  Cheap and
 * adequate for a cooperative scheduler; a real exit-notification port
 * is a phase-3 conversation.
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

/* ---- single-byte read from the kbd stream port ------------------- */

static mach_port_name_t	g_kbd_stream;	/* SEND right naming dev/kbd's RX */

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
	(void)write("\b \b", 3);
}

static void
prompt(void)
{

	puts("$ ");
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

static void
builtin_help(void)
{
	const char	*p;
	size_t		 i;

	puts("style9-os shell -- builtins:\n");
	puts("  help        show this list\n");
	puts("  echo ARGS   print arguments\n");
	puts("  clear       clear the screen\n");
	puts("  about       print version banner\n");
	puts("\nspawnable programs:\n");
	for (i = 0; known_progs[i] != NULL; i++) {
		p = known_progs[i];
		puts("  ");
		puts(p);
		putchar('\n');
	}
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

static void
builtin_clear(void)
{

	/* ANSI ESC [ H == cursor home, ESC [ 2 J == erase screen. */
	(void)write("\x1b[H\x1b[2J", 7);
}

static void
builtin_about(void)
{

	puts("style9-os: BSD-style microkernel, x86_64\n");
	puts("  sh.elf (ring 3, libstyle9, phase 2)\n");
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

static void
dispatch(int argc, char *argv[])
{
	long	rv;

	if (argc <= 0)
		return;

	if (streq(argv[0], "help")) {
		builtin_help();
		return;
	}
	if (streq(argv[0], "echo")) {
		builtin_echo(argc, argv);
		return;
	}
	if (streq(argv[0], "clear")) {
		builtin_clear();
		return;
	}
	if (streq(argv[0], "about")) {
		builtin_about();
		return;
	}

	/*
	 * Not a builtin -- hand to SYS_SPAWN.  argv[1..] are ignored for
	 * now; the kernel's progreg_spawn takes only a name.  Real argv
	 * delivery to the child waits on a stack-loader update.
	 */
	rv = spawn(argv[0]);
	if (rv <= 0) {
		puts(argv[0]);
		puts(": not found\n");
		return;
	}
	wait_child(rv);
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
			puts("sh: read failed, exiting\n");
			return;
		}

		if (c == '\r' || c == '\n') {
			putchar('\n');
			line[len] = '\0';
			argc = split_argv(line, argv, SH_ARGC_MAX);
			if (argc > 0)
				dispatch(argc, argv);
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

	puts("style9-os: sh.elf up (libstyle9, ring 3)\n");

	g_kbd_stream = dev_open_stream("kbd");
	if (g_kbd_stream == MACH_PORT_NULL) {
		puts("sh: dev_open_stream('kbd') failed\n");
		return (1);
	}
	puts("sh: dev/kbd opened.  type 'help' for commands.\n");

	repl();

	(void)mach_port_deallocate(g_kbd_stream);
	return (0);
}
