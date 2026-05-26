/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * First standalone ring-3 program for style9-os.  Built as a static
 * freestanding ELF64 by user/Makefile (or the top-level Makefile's
 * user-mode rules), linked at VA USER_LOAD_BASE (0x40000000) just
 * past the kernel's 1 GiB identity map.  No libc, just inline
 * syscalls.
 */

typedef unsigned long	size_t;
typedef long		ssize_t;

/* Must agree with kern/syscall.h. */
#define	SYS_PRINT	0
#define	SYS_EXIT	1
#define	SYS_YIELD	2

static long
syscall1(long nr, long a0)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0)
	    : "rcx", "r11", "memory");
	return (ret);
}

static long
syscall2(long nr, long a0, long a1)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1)
	    : "rcx", "r11", "memory");
	return (ret);
}

static long
write1(const char *s, size_t n)
{

	return (syscall2(SYS_PRINT, (long)s, (long)n));
}

static size_t
strlen(const char *s)
{
	size_t	n;

	n = 0;
	while (s[n] != '\0')
		n++;
	return (n);
}

static const char	banner[] =
    "hello from hello.elf (loaded by kernel ELF parser, ring 3)\n";
static const char	follow[] = "  -- second syscall round-trip\n";

__attribute__((noreturn))
void
_start(void)
{

	write1(banner, sizeof(banner) - 1);
	write1(follow, strlen(follow));

	syscall1(SYS_EXIT, 0);
	for (;;)
		;
}
