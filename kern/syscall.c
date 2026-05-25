/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "sched.h"
#include "syscall.h"
#include "thread.h"
#include "tty.h"

#define	MSR_EFER	0xC0000080u
#define	MSR_STAR	0xC0000081u
#define	MSR_LSTAR	0xC0000082u
#define	MSR_FMASK	0xC0000084u

#define	EFER_SCE	(1u << 0)

#define	RFLAGS_IF	(1u << 9)
#define	RFLAGS_DF	(1u << 10)

extern void	syscall_entry(void);

static long	sys_print(const char *buf, size_t len);
static long	sys_exit(int code) __attribute__((noreturn));
static long	sys_yield(void);

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t	lo;
	uint32_t	hi;

	__asm__ __volatile__("rdmsr"
	    : "=a"(lo), "=d"(hi)
	    : "c"(msr));
	return (((uint64_t)hi << 32) | lo);
}

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
	uint32_t	lo;
	uint32_t	hi;

	lo = (uint32_t)(val & 0xFFFFFFFFu);
	hi = (uint32_t)(val >> 32);
	__asm__ __volatile__("wrmsr"
	    :
	    : "c"(msr), "a"(lo), "d"(hi));
}

/*
 * Enable SYSCALL/SYSRET and wire it to our entry stub.
 *
 * STAR encodes:
 *	[47:32]	kernel selector base -- CPU loads CS=that+0, SS=+8 on
 *		SYSCALL.  We set 0x08 so CS=0x08, SS=0x10.
 *	[63:48]	user selector base -- CPU loads CS=that+16, SS=+8 on
 *		SYSRETQ (64-bit).  We set 0x18 so CS=0x28|3, SS=0x20|3.
 *
 * LSTAR is the 64-bit entry RIP.  FMASK clears IF + DF on entry so
 * the kernel side starts with interrupts disabled and a known
 * direction.
 */
void
syscall_init(void)
{
	uint64_t	star;

	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

	star  = ((uint64_t)0x08u) << 32;
	star |= ((uint64_t)0x18u) << 48;
	wrmsr(MSR_STAR, star);

	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);
	wrmsr(MSR_FMASK, RFLAGS_IF | RFLAGS_DF);

	kprintf("syscall: enabled, entry=%p\n",
	    (void *)(uintptr_t)&syscall_entry);
}

long
syscall_dispatch(struct syscall_frame *f)
{

	switch (f->sf_nr) {
	case SYS_PRINT:
		return (sys_print((const char *)f->sf_arg0,
		    (size_t)f->sf_arg1));
	case SYS_EXIT:
		sys_exit((int)f->sf_arg0);
		/* NOTREACHED */
	case SYS_YIELD:
		return (sys_yield());
	default:
		return (SYS_E_NOSYS);
	}
}

/*
 * sys_print: write `len` bytes from the user buffer to the kernel
 * console.  No copy-in yet -- the kernel reads the user VA directly
 * (shared PML4, US=1 on the leaf).  When SMAP lands the touch will
 * be bracketed by stac/clac.
 */
static long
sys_print(const char *buf, size_t len)
{
	size_t	i;

	if (len > 4096u)
		len = 4096u;

	for (i = 0; i < len; i++)
		tty_putc(buf[i]);

	return ((long)len);
}

static long
sys_exit(int code)
{

	kprintf("[user thread exited, code=%d]\n", code);
	thread_exit();
	/* NOTREACHED */
}

static long
sys_yield(void)
{

	thread_yield();
	return (0);
}
