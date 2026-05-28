/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * machotest -- the first ring-3 program the kernel runs from a Mach-O
 * container instead of an ELF.  The source is an ordinary style9 binary
 * (libstyle9 crt0 + SYS_* numbers); the Makefile compiles it to an ELF
 * and then tools/elf2macho rewraps that ELF as a thin x86-64 Mach-O
 * (registered as "machotest") and a one-slice fat/universal archive
 * (registered as "machotest_fat").  At spawn time the launcher sniffs
 * the image magic and hands it to kern/macho.c rather than elf.c.
 *
 * Printing argc/argv does double duty: it proves the Mach-O actually
 * executed AND that the SysV initial-stack frame the launcher builds is
 * format-independent -- a Mach-O reaches main(argc, argv) through the
 * exact same crt0 path as an ELF.  The boot hello demo greps the serial
 * log for this banner.
 */

#include "style9.h"

int
main(int argc, char *argv[])
{
	int	i;

	printf("machotest: hello from a Mach-O binary "
	    "(loaded by macho_load, ran through crt0)\n");
	printf("machotest: argc=%d\n", argc);
	for (i = 0; i < argc; i++)
		printf("  argv[%d]=\"%s\"\n", i, argv[i]);
	return (0);
}
