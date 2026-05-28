/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * argecho -- echo the argument vector this program was spawned with.
 * It exists to prove the command-line path end to end: SYS_SPAWN_ARGS
 * copies the caller's argv into the kernel, the launcher
 * (build_user_arg_stack) lays it out on the child's initial stack in
 * the SysV layout, and crt0.S forwards argc/argv to main.  The boot
 * hello demo spawns this with a known vector and greps the serial log
 * for the echoed strings.
 */

#include "style9.h"

int
main(int argc, char *argv[])
{
	int	i;

	printf("argecho: argc=%d\n", argc);
	for (i = 0; i < argc; i++)
		printf("  argv[%d]=\"%s\"\n", i, argv[i]);
	return (0);
}
