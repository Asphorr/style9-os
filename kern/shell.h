/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_SHELL_H_
#define	_SYS_SHELL_H_

#include <stddef.h>

/*
 * In-kernel line-oriented shell.
 *
 * shell_run reads characters from the keyboard ring buffer, builds a
 * line in a fixed-size buffer, and on '\n' splits the line into argv
 * tokens and dispatches through the command table in cmds.c.  The
 * buffer is static (kmem-free hot path) so we can stress-test the
 * memory subsystem from inside the shell without the parser itself
 * being a confounder.
 *
 * The command table is shell_cmds[] / shell_ncmds in cmds.c.  Each
 * command returns an int -- 0 == success, non-zero printed as "error
 * N" but otherwise unused for now.
 */

#define	SHELL_LINE_MAX	256
#define	SHELL_ARGC_MAX	16

typedef int (*shell_cmd_fn)(int argc, char *argv[]);

struct shell_cmd {
	const char	*sc_name;
	const char	*sc_help;
	shell_cmd_fn	 sc_fn;
};

extern const struct shell_cmd	shell_cmds[];
extern const size_t		shell_ncmds;

void	shell_run(void) __attribute__((noreturn));

#endif /* !_SYS_SHELL_H_ */
