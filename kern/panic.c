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
 * Bounds the backtrace walk to memory we know exists: the kernel sits
 * just above 1 MiB, the identity map covers 0 .. 1 GiB, and the boot
 * stack lives in .bss.  Anything outside the kernel image bounds is
 * almost certainly a corrupted RBP and we should stop rather than
 * follow it into a fault.
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

	/*
	 * Disable interrupts unconditionally; we are about to render and
	 * walk the frame chain in a context that cannot tolerate being
	 * pre-empted by an IRQ that might also touch the console.
	 */
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
 * KASSERT failure path.  file:line + the failed expression are baked
 * into the panic message; the "called from" line in panic() points
 * inside kassert_fail itself, but file:line is sufficient to find the
 * assertion site and the backtrace shows the calling chain.
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
