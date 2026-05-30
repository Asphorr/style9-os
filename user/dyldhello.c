/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * dyldhello -- the S4 dynamic-linking test program.  Ordinary compiler
 * output, NOT a hand stub: clang compiles it for the Darwin target and
 * ld64.lld links it as a real dynamic Mach-O that imports write/exit from
 * /usr/lib/libSystem.B.dylib via LC_DYLD_CHAINED_FIXUPS, with an
 * LC_LOAD_DYLINKER naming /usr/lib/dyld.  The kernel maps it plus our dyld
 * and hands off; our dyld (user/dyld.c) binds the imports against our
 * libSystem and jumps to the LC_MAIN entry here.  Entry is _entry (ld -e),
 * so no crt is required.
 *
 * Relinked low (-pagezero_size 0x50000000) into the style9 user-VA window
 * [0x40000000,0x80000000); a stock 4 GiB __TEXT base would be outside it.
 */

extern long	write(int fd, const void *buf, unsigned long n);
extern void	exit(int code);

static const char msg[] =
    "dyldhello: bound through our dyld + libSystem, ran my LC_MAIN entry\n";

int
entry(void)
{
	write(1, msg, sizeof(msg) - 1);
	exit(0);
	return (0);
}
