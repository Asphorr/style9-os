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
 * In-kernel line-oriented shell, the fallback when sh.elf cannot be
 * spawned (see kmain).
 *
 * shell_run receives characters from the keyboard and serial input
 * ports, builds a line in a static buffer (no kmem use, so the allocator
 * can be stress-tested from the shell), and on '\n' splits it into argv
 * and dispatches through shell_cmds[] / shell_ncmds in cmds.c.  A
 * command returns 0 on success; anything else is printed as "[error N]".
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
