/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ddb.h"
#include "kbd.h"
#include "kmem.h"
#include "kprintf.h"
#include "memmap.h"
#include "panic.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "task.h"
#include "thread.h"
#include "tty.h"
#include "vm.h"

#define	DDB_LINEMAX	96

static int	ddb_readline(char *buf, int max);
static void	ddb_dispatch(const char *line, uint64_t entry_rbp,
		    const struct trapframe *tf);
static void	ddb_help(void);
static void	ddb_regs(const struct trapframe *tf);
static void	ddb_examine(const char *args);
static void	ddb_ps(void);
static void	ddb_show(const char *args);
static void	ddb_show_task(uint64_t id);
static void	ddb_show_thread(uint64_t id);
static const char *skip_ws(const char *s);
static int	parse_hex(const char *s, uint64_t *out);
static int	parse_uint(const char *s, uint64_t *out);

void
ddb_enter(uint64_t entry_rbp, const struct trapframe *tf)
{
	char	line[DDB_LINEMAX];

	tty_set_attr(TTY_ATTR(TTY_LIGHT_CYAN, TTY_BLACK));
	tty_puts("\nentering ddb.  type 'h' for help.\n");
	tty_set_attr(TTY_ATTR(TTY_WHITE, TTY_BLACK));

	for (;;) {
		tty_set_attr(TTY_ATTR(TTY_LIGHT_CYAN, TTY_BLACK));
		tty_puts("ddb> ");
		tty_set_attr(TTY_ATTR(TTY_WHITE, TTY_BLACK));

		if (ddb_readline(line, sizeof(line)) <= 0)
			continue;

		ddb_dispatch(line, entry_rbp, tf);
	}
}

/*
 * Read one '\n'-terminated line from the keyboard via the polled path,
 * echoing characters to the console as they arrive.  Returns the number
 * of characters in the buffer (excluding the terminating NUL); zero on
 * empty input.  Backspace removes one cell from both the buffer and the
 * screen; non-printable bytes are ignored.
 */
static int
ddb_readline(char *buf, int max)
{
	int	c, n;

	n = 0;
	for (;;) {
		c = kbd_poll_getc_block();

		if (c == '\n') {
			tty_putc('\n');
			buf[n] = '\0';
			return (n);
		}

		if (c == '\b') {
			if (n > 0) {
				n--;
				tty_putc('\b');
			}
			continue;
		}

		if (c < 0x20 || c >= 0x7F)
			continue;

		if (n < max - 1) {
			buf[n++] = (char)c;
			tty_putc((char)c);
		}
	}
}

static void
ddb_dispatch(const char *line, uint64_t entry_rbp, const struct trapframe *tf)
{

	line = skip_ws(line);

	if (*line == '\0')
		return;

	if (line[0] == 'h' || line[0] == '?') {
		ddb_help();
		return;
	}
	if (line[0] == 'r') {
		ddb_regs(tf);
		return;
	}
	if (line[0] == 'b' && line[1] == 't') {
		backtrace_print((uintptr_t)entry_rbp, 16);
		return;
	}
	if (line[0] == 'p' && (line[1] == '\0' || line[1] == 's' ||
	    line[1] == ' ' || line[1] == '\t')) {
		ddb_ps();
		return;
	}
	if (line[0] == 's' && (line[1] == ' ' || line[1] == '\t' ||
	    (line[1] == 'h' && line[2] == 'o' && line[3] == 'w'))) {
		const char *args = line + 1;
		if (line[1] == 'h')
			args = line + 4;	/* skip "how" */
		ddb_show(args);
		return;
	}
	if (line[0] == 'x') {
		ddb_examine(line + 1);
		return;
	}
	if (line[0] == 'q') {
		tty_set_attr(TTY_ATTR(TTY_YELLOW, TTY_BLACK));
		tty_puts("ddb: halt.\n");
		for (;;)
			__asm__ __volatile__ ("cli; hlt");
	}

	tty_set_attr(TTY_ATTR(TTY_LIGHT_RED, TTY_BLACK));
	kprintf("ddb: unknown command: %s\n", line);
	tty_set_attr(TTY_ATTR(TTY_WHITE, TTY_BLACK));
}

static void
ddb_help(void)
{

	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	tty_puts(
	    "  h, ?              show this help\n"
	    "  r                 print trap-context registers\n"
	    "  bt                print backtrace from panic entry frame\n"
	    "  x <addr> [<n>]    hex-dump n bytes (default 64) from addr\n"
	    "  ps                list every task with its threads\n"
	    "  s task <id>       show one task's full state\n"
	    "  s thread <id>     show one thread's full state\n"
	    "  s sched           runqueue + context-switch counter\n"
	    "  s ports           every kernel port_space entry\n"
	    "  s vm              kernel_task's vm_map\n"
	    "  s mem             memmap + pmm + kmem summary\n"
	    "  q                 halt the CPU\n");
	tty_set_attr(TTY_ATTR(TTY_WHITE, TTY_BLACK));
}

static void
ddb_regs(const struct trapframe *tf)
{

	if (tf == NULL) {
		kprintf("(no trap frame available; regs are inside the "
		    "debugger itself)\n");
		return;
	}

	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));
	kprintf("rip=0x%016lx  cs=0x%04lx  rflags=0x%016lx\n",
	    (unsigned long)tf->tf_rip, (unsigned long)tf->tf_cs,
	    (unsigned long)tf->tf_rflags);
	kprintf("rsp=0x%016lx  ss=0x%04lx\n",
	    (unsigned long)tf->tf_rsp, (unsigned long)tf->tf_ss);
	kprintf("rax=0x%016lx  rbx=0x%016lx\n",
	    (unsigned long)tf->tf_rax, (unsigned long)tf->tf_rbx);
	kprintf("rcx=0x%016lx  rdx=0x%016lx\n",
	    (unsigned long)tf->tf_rcx, (unsigned long)tf->tf_rdx);
	kprintf("rsi=0x%016lx  rdi=0x%016lx\n",
	    (unsigned long)tf->tf_rsi, (unsigned long)tf->tf_rdi);
	kprintf("rbp=0x%016lx  r8 =0x%016lx\n",
	    (unsigned long)tf->tf_rbp, (unsigned long)tf->tf_r8);
	kprintf("r9 =0x%016lx  r10=0x%016lx\n",
	    (unsigned long)tf->tf_r9,  (unsigned long)tf->tf_r10);
	kprintf("r11=0x%016lx  r12=0x%016lx\n",
	    (unsigned long)tf->tf_r11, (unsigned long)tf->tf_r12);
	kprintf("r13=0x%016lx  r14=0x%016lx  r15=0x%016lx\n",
	    (unsigned long)tf->tf_r13, (unsigned long)tf->tf_r14,
	    (unsigned long)tf->tf_r15);
}

/*
 * x <addr> [<count>] -- byte dump.
 *
 * Lazy sanity-check: refuse anything outside the identity-mapped low
 * 1 GiB (which is all we have right now).  Dumping uncaught wild VAs
 * would just fault inside the debugger.
 */
static void
ddb_examine(const char *args)
{
	uint64_t	addr, count;
	uint8_t		*p;
	size_t		i;

	args = skip_ws(args);
	if (parse_hex(args, &addr) <= 0) {
		kprintf("usage: x <addr> [<count>]\n");
		return;
	}

	/* Advance past the parsed hex digits. */
	while (*args != '\0' && *args != ' ' && *args != '\t')
		args++;
	args = skip_ws(args);

	count = 64;
	if (*args != '\0')
		(void)parse_hex(args, &count);
	if (count > 1024)
		count = 1024;

	if (addr + count > 0x40000000ULL) {
		kprintf("ddb: refusing to read past identity map (>1 GiB)\n");
		return;
	}

	p = (uint8_t *)(uintptr_t)addr;
	for (i = 0; i < count; i++) {
		if ((i % 16) == 0) {
			if (i != 0)
				tty_putc('\n');
			kprintf("  0x%016lx: ", (unsigned long)(addr + i));
		}
		kprintf("%02x ", p[i]);
	}
	tty_putc('\n');
}

/* ---- introspection commands -------------------------------------- */

/*
 * Walk every live task, then every thread attached to it.  No locks --
 * DDB runs with interrupts disabled and we accept the small chance of
 * a torn read for inspection convenience.  Same trade-off the existing
 * task_list_print / sched_print make.
 */
static void
ddb_ps(void)
{
	struct task		*tasks[16];
	struct thread		*th;
	size_t			 n;
	size_t			 i;

	tty_set_attr(TTY_ATTR(TTY_LIGHT_CYAN, TTY_BLACK));
	tty_puts("all live tasks + threads:\n");
	tty_set_attr(TTY_ATTR(TTY_LIGHT_GRAY, TTY_BLACK));

	n = task_snapshot(tasks, sizeof(tasks) / sizeof(tasks[0]));
	for (i = 0; i < n; i++) {
		task_print(tasks[i]);
		for (th = tasks[i]->t_threads; th != NULL;
		    th = th->th_task_link) {
			kprintf("    ");
			thread_print(th);
		}
	}
}

static void
ddb_show(const char *args)
{
	uint64_t	id;

	args = skip_ws(args);
	if (*args == '\0') {
		kprintf("ddb: usage: s {task|thread|sched|ports|vm|mem} ...\n");
		return;
	}

	if (args[0] == 't' && args[1] == 'a' && args[2] == 's' &&
	    args[3] == 'k') {
		args = skip_ws(args + 4);
		if (parse_uint(args, &id) <= 0) {
			kprintf("ddb: usage: s task <id>\n");
			return;
		}
		ddb_show_task(id);
		return;
	}
	if (args[0] == 't' && args[1] == 'h') {
		/* "thread" -- skip "thread" or just match prefix */
		args = skip_ws(args + 6);
		if (parse_uint(args, &id) <= 0) {
			kprintf("ddb: usage: s thread <id>\n");
			return;
		}
		ddb_show_thread(id);
		return;
	}
	if (args[0] == 's' && args[1] == 'c') {		/* sched */
		sched_print();
		return;
	}
	if (args[0] == 'p' && args[1] == 'o') {		/* ports */
		port_space_print(kernel_space);
		return;
	}
	if (args[0] == 'v' && args[1] == 'm') {		/* vm */
		vm_map_print(kernel_task->t_map);
		return;
	}
	if (args[0] == 'm' && args[1] == 'e') {		/* mem */
		memmap_print();
		pmm_stats();
		kmem_stats();
		return;
	}

	kprintf("ddb: unknown 'show' target: %s\n", args);
}

static void
ddb_show_task(uint64_t id)
{
	struct task	*tasks[16];
	size_t		 n;
	size_t		 i;

	n = task_snapshot(tasks, sizeof(tasks) / sizeof(tasks[0]));
	for (i = 0; i < n; i++) {
		if (tasks[i]->t_id == id) {
			task_print(tasks[i]);
			return;
		}
	}
	kprintf("ddb: no task with id=%llu\n", (unsigned long long)id);
}

/*
 * Threads are stored per-task, not in a global list, so walk every
 * task and then every thread attached to it.  No ref bumps -- DDB
 * holds the kernel still.
 */
static void
ddb_show_thread(uint64_t id)
{
	struct task	*tasks[16];
	struct thread	*th;
	size_t		 n;
	size_t		 i;

	n = task_snapshot(tasks, sizeof(tasks) / sizeof(tasks[0]));
	for (i = 0; i < n; i++) {
		for (th = tasks[i]->t_threads; th != NULL;
		    th = th->th_task_link) {
			if (th->th_id == id) {
				thread_print(th);
				return;
			}
		}
	}
	kprintf("ddb: no thread with id=%llu\n", (unsigned long long)id);
}

static const char *
skip_ws(const char *s)
{

	while (*s == ' ' || *s == '\t')
		s++;
	return (s);
}

/*
 * Parse an unsigned integer in either base.  "0x"/"0X" prefix goes
 * hex; anything else (incl. plain "12") is decimal.  Returns the
 * number of input characters consumed, or 0 on failure (no digits).
 */
static int
parse_uint(const char *s, uint64_t *out)
{
	uint64_t	v;
	int		consumed, digits;

	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
		return (parse_hex(s, out));

	v = 0;
	consumed = 0;
	digits = 0;
	for (;;) {
		char	c = s[consumed];

		if (c < '0' || c > '9')
			break;
		v = v * 10u + (uint64_t)(c - '0');
		consumed++;
		digits++;
	}
	if (digits == 0)
		return (0);
	*out = v;
	return (consumed);
}

/*
 * Parse a leading hexadecimal literal (with optional 0x prefix) from s.
 * On success, writes the value to *out and returns the number of input
 * characters consumed.  On failure (no hex digits found), returns 0
 * and leaves *out untouched.
 */
static int
parse_hex(const char *s, uint64_t *out)
{
	uint64_t	v;
	int		consumed, digits;
	char		c;

	consumed = 0;
	if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		consumed = 2;
	}

	v = 0;
	digits = 0;
	for (;;) {
		c = *s;
		if (c >= '0' && c <= '9')
			v = (v << 4) | (uint64_t)(c - '0');
		else if (c >= 'a' && c <= 'f')
			v = (v << 4) | (uint64_t)(c - 'a' + 10);
		else if (c >= 'A' && c <= 'F')
			v = (v << 4) | (uint64_t)(c - 'A' + 10);
		else
			break;
		s++;
		consumed++;
		digits++;
	}

	if (digits == 0)
		return (0);
	*out = v;
	return (consumed);
}
