/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddb.h"
#include "kprintf.h"
#include "ksym.h"
#include "panic.h"
#include "tty.h"

/*
 * Bounds for the backtrace walk: the 1 GiB boot identity map, which holds
 * every kernel stack (the boot stack in .bss, kstacks from kmalloc).  An
 * RBP outside it is corrupt; stop rather than follow it into a fault.
 */
#define	BT_MIN_RBP	0x000000000000FFFFULL
#define	BT_MAX_RBP	0x0000000040000000ULL

volatile bool	panic_in_progress;

static void	panic_halt(void) __attribute__((noreturn));

void
panic(const char *fmt, ...)
{
	va_list		ap;
	uintptr_t	rbp;
	void		*caller;

	/* No interrupt may touch the console or preempt the autopsy. */
	__asm__ __volatile__ ("cli");

	if (panic_in_progress) {
		/* Recursive panic; we cannot trust kprintf any more. */
		panic_halt();
	}
	panic_in_progress = true;

	caller = __builtin_return_address(0);

	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	tty_puts("\n*** kernel panic: ");

	va_start(ap, fmt);
	kvprintf(fmt, ap);
	va_end(ap);

	tty_puts("\n");

	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	kprintf("called from ");
	ksym_print((uint64_t)(uintptr_t)caller);
	kprintf("\n");

	__asm__ __volatile__ ("movq %%rbp, %0" : "=r"(rbp));
	backtrace_print(rbp, 16);

	ddb_enter((uint64_t)rbp, NULL);
	/* NOTREACHED -- ddb_enter is __dead. */
}

/*
 * KASSERT failure.  panic's "called from" names kassert_fail, but the
 * message carries file:line and the backtrace the calling chain.
 */
void
kassert_fail(const char *cond, const char *file, int line, const char *msg)
{

	panic("KASSERT(%s) at %s:%d: %s", cond, file, line, msg);
}

void
backtrace_print(uintptr_t rbp, int max_frames)
{
	uint64_t	*frame;
	uint64_t	 ret_addr;
	int		 i;

	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	kprintf("backtrace (frame pointer chain):\n");

	frame = (uint64_t *)rbp;
	for (i = 0; i < max_frames; i++) {
		if (frame == NULL)
			break;
		if ((uint64_t)frame < BT_MIN_RBP ||
		    (uint64_t)frame >= BT_MAX_RBP) {
			kprintf("  rbp=0x%016lx (out of range, stopping)\n",
			    (uint64_t)frame);
			break;
		}

		ret_addr = frame[1];
		if (ret_addr == 0)
			break;

		kprintf("  #%-2d  rip=", i);
		ksym_print(ret_addr);
		kprintf("  rbp=0x%016lx\n", (uint64_t)frame);

		frame = (uint64_t *)frame[0];
	}
}

static void
panic_halt(void)
{

	tty_set_attr(TTY_ATTR(TTY_YELLOW, TTY_BLACK));
	tty_puts("system halted.\n");

	for (;;)
		__asm__ __volatile__ ("cli; hlt");
}
