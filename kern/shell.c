/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kbd.h"
#include "kprintf.h"
#include "shell.h"
#include "tty.h"

static char	shell_line[SHELL_LINE_MAX];
static size_t	shell_len;

static void	prompt(void);
static bool	is_blank(char c);
static int	split_argv(char *line, char *argv[], int argv_max);
static int	dispatch(int argc, char *argv[]);
static int	streq(const char *a, const char *b);

void
shell_run(void)
{
	int	c;

	shell_len = 0;

	tty_set_attr(TTY_ATTR(TTY_WHITE, TTY_BLACK));
	tty_puts("\nstyle9-os shell.  type 'help' for commands.\n");
	prompt();

	for (;;) {
		__asm__ __volatile__ ("hlt");

		while ((c = kbd_getc()) >= 0) {
			if (c == '\n') {
				tty_putc('\n');
				shell_line[shell_len] = '\0';

				char *argv[SHELL_ARGC_MAX];
				int argc = split_argv(shell_line, argv,
				    SHELL_ARGC_MAX);
				if (argc > 0)
					(void)dispatch(argc, argv);

				shell_len = 0;
				prompt();
				continue;
			}

			if (c == '\b') {
				if (shell_len > 0) {
					shell_len--;
					tty_putc('\b');
				}
				continue;
			}

			/*
			 * Ignore characters that would overflow the
			 * buffer rather than truncating mid-line; the
			 * caller can backspace and retry.
			 */
			if (shell_len + 1 >= SHELL_LINE_MAX)
				continue;

			if ((unsigned char)c < 0x20 && c != '\t')
				continue;

			shell_line[shell_len++] = (char)c;
			tty_putc((char)c);
		}
	}
}

static void
prompt(void)
{

	tty_set_attr(TTY_ATTR(TTY_LIGHT_CYAN, TTY_BLACK));
	tty_puts("> ");
	tty_set_attr(TTY_ATTR(TTY_WHITE, TTY_BLACK));
}

static bool
is_blank(char c)
{

	return (c == ' ' || c == '\t');
}

/*
 * In-place argv split: walk the line, replace runs of blanks with
 * NULs, point argv[i] at each token's first non-blank.  Caller owns
 * the underlying buffer; no kmalloc here so this is safe to call from
 * any context.
 */
static int
split_argv(char *line, char *argv[], int argv_max)
{
	int	argc;
	char	*p;

	argc = 0;
	p = line;

	while (*p != '\0' && argc < argv_max) {
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

static int
dispatch(int argc, char *argv[])
{
	size_t	i;
	int	rv;

	for (i = 0; i < shell_ncmds; i++) {
		if (streq(argv[0], shell_cmds[i].sc_name)) {
			rv = shell_cmds[i].sc_fn(argc, argv);
			if (rv != 0) {
				tty_set_attr(TTY_ATTR(TTY_LIGHT_RED,
				    TTY_BLACK));
				kprintf("[error %d]\n", rv);
				tty_set_attr(TTY_ATTR(TTY_WHITE,
				    TTY_BLACK));
			}
			return (rv);
		}
	}

	kprintf("%s: not found\n", argv[0]);
	return (-1);
}

static int
streq(const char *a, const char *b)
{

	while (*a != '\0' && *b != '\0') {
		if (*a != *b)
			return (0);
		a++;
		b++;
	}
	return (*a == '\0' && *b == '\0');
}
