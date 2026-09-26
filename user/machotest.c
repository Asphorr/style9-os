/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * machotest -- an ordinary style9 program (libstyle9 crt0, SYS_* numbers)
 * run from a Mach-O container.  The Makefile builds it as an ELF, then
 * tools/elf2macho rewraps it as a thin x86-64 Mach-O ("machotest") and a
 * one-slice fat archive ("machotest_fat").  The launcher sniffs the magic
 * and hands the image to kern/macho.c rather than elf.c.
 *
 * Printing argc/argv proves the Mach-O ran and that it reached
 * main(argc, argv) through the same initial-stack frame and crt0 path as
 * an ELF.  The boot hello demo greps the serial log for this banner.
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
