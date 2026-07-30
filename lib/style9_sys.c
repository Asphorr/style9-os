/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/*
 * Raw syscall stubs.
 *
 * On x86_64 the kernel ABI takes the syscall number in %rax and
 * arguments in %rdi, %rsi, %rdx, %r10, %r8, %r9.  rcx + r11 are
 * clobbered by the `syscall` instruction itself (CPU stashes %rip
 * into %rcx and %rflags into %r11).  Returns the kernel's %rax,
 * negative for kernel error codes (SYS_E_*).
 *
 * Five variants cover everything the kernel exposes today; if a
 * future syscall needs five or six arguments we can grow syscall5 /
 * syscall6 without touching anything else.
 */

long
syscall0(long nr)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr)
	    : "rcx", "r11", "memory");
	return (ret);
}

long
syscall1(long nr, long a0)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0)
	    : "rcx", "r11", "memory");
	return (ret);
}

long
syscall2(long nr, long a0, long a1)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1)
	    : "rcx", "r11", "memory");
	return (ret);
}

long
syscall3(long nr, long a0, long a1, long a2)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1), "d"(a2)
	    : "rcx", "r11", "memory");
	return (ret);
}

long
syscall4(long nr, long a0, long a1, long a2, long a3)
{
	register long	r10 __asm__("r10") = a3;
	long		ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
	    : "rcx", "r11", "memory");
	return (ret);
}

/* ---- process ------------------------------------------------------- */

void
exit(int code)
{

	(void)syscall1(SYS_EXIT, (long)code);
	/*
	 * Kernel's sys_exit calls thread_exit() and never returns; the
	 * loop is here for the compiler's benefit (we declared noreturn)
	 * and as a defensive landing pad if a future kernel build ever
	 * returned from the syscall by mistake.
	 */
	for (;;)
		;
}

long
yield(void)
{

	return (syscall0(SYS_YIELD));
}

/*
 * ONE TURN OF A POLL LOOP, AND WHY yield() ALONE STOPPED BEING ONE.
 *
 * Every "wait for the child to get somewhere" in this tree was written as a
 * bounded run of yields: give somebody else a turn, look again, and give up
 * after N.  That was a way of WAITING, because a yield with work queued
 * behind it does not come back until that work has had the CPU -- so N turns
 * bought N slices of real time.
 *
 * With four processors it buys nothing.  The task being waited for is not
 * queued behind this one, it is RUNNING on another processor, so the yield
 * finds an empty runqueue here and returns at once; sixty-four turns are
 * spent in microseconds and the loop reports a failure that is only the
 * budget being denominated in the wrong unit.  That is not a worry, it is the
 * first thing the first four-processor boot of this kernel got wrong:
 *
 *	loopchild.tport lookup failed after 64 yields
 *
 * ⚠ AND ASKING THE KERNEL WHETHER THE YIELD SWITCHED IS NOT ENOUGH, which
 * took a second four-processor boot to learn.  A yield that switches to
 * another poll-spinner comes straight back, so it reports "yes, somebody had
 * the CPU" and still buys almost no time -- the budgets ran out anyway, and
 * `dash /bin/demo.sh' was abandoned after 8192 turns that a single processor
 * spends in 33.
 *
 * So a turn is measured in TIME and in nothing else: park for about one tick
 * on a port nobody can send to.  That makes a turn mean the same thing on any
 * number of processors -- roughly one scheduling quantum -- which is what it
 * meant on one processor by accident, because a yield there did not come back
 * until the queue had gone round.  Every budget already written in this tree
 * keeps the meaning it was chosen with.
 *
 * The port is allocated on first use rather than at start-up, so a program
 * that never polls never spends a name on this, and the names printed by the
 * early scenes of the test programs do not shift.  A program that cannot get
 * one falls back to the bare yield, which is what it had before.
 */
#define	POLL_TURN_MS	1

long
poll_turn(void)
{
	static mach_port_name_t	nap;
	struct mach_msg_header	buf;

	if (nap == MACH_PORT_NULL) {
		nap = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE);
		if (nap == MACH_PORT_NULL)
			return (syscall0(SYS_YIELD));
	}

	/*
	 * Nobody holds a SEND right on this port, so nothing can ever arrive:
	 * the only way out is the timeout, which is the point.  One millisecond
	 * asked for, one timer tick given -- the kernel's deadlines are checked
	 * from the tick, and that is the finest grain of waiting this machine
	 * has.  Asking for the smallest thing and being handed the resolution
	 * is more honest than naming the resolution here, where it would be a
	 * second copy of a number that lives in the clock.
	 */
	(void)mach_msg_recv_timed(nap, &buf, sizeof(buf), POLL_TURN_MS);
	return (0);
}

long
spawn(const char *name)
{

	return (syscall1(SYS_SPAWN, (long)name));
}

long
spawn_with_port(const char *name, mach_port_name_t source_name)
{

	return (syscall2(SYS_SPAWN_WITH_PORT, (long)name, (long)source_name));
}

int
task_set_exception_port(mach_port_name_t notify_port)
{

	return ((int)syscall1(SYS_TASK_SET_EXC_PORT, (long)notify_port));
}

int
task_set_exception_ports(uint32_t types_mask, mach_port_name_t notify_port)
{

	return ((int)syscall2(SYS_TASK_SET_EXC_PORTS,
	    (long)types_mask, (long)notify_port));
}

int
thread_set_exception_ports(uint32_t types_mask, mach_port_name_t notify_port)
{

	return ((int)syscall2(SYS_THREAD_SET_EXC_PORTS,
	    (long)types_mask, (long)notify_port));
}

long
task_get_port_snapshot(uint64_t task_id,
    struct mach_port_snapshot_entry *out, size_t max_entries)
{

	return (syscall3(SYS_TASK_GET_PORT_SNAPSHOT,
	    (long)task_id, (long)out, (long)max_entries));
}

long
task_get_vm_regions(uint64_t task_id,
    struct mach_vm_region_entry *out, size_t max_entries)
{

	return (syscall3(SYS_TASK_GET_VM_REGIONS,
	    (long)task_id, (long)out, (long)max_entries));
}

int
task_alive(uint64_t task_id)
{

	return ((int)syscall1(SYS_TASK_ALIVE, (long)task_id));
}

int
task_kill(mach_port_name_t target_port)
{

	return ((int)syscall1(SYS_TASK_KILL, (long)target_port));
}

long
spawn_returns_taskport(const char *name, mach_port_name_t *out_taskport)
{

	return (syscall2(SYS_SPAWN_RETURNS_TASKPORT, (long)name,
	    (long)out_taskport));
}

long
spawn_args(const char *name, int argc, char *const argv[],
    mach_port_name_t *out_taskport)
{

	return (syscall4(SYS_SPAWN_ARGS, (long)name, (long)argv,
	    (long)argc, (long)out_taskport));
}

long
cons_feed(const char *buf, unsigned long len)
{

	return (syscall2(SYS_CONS_FEED, (long)buf, (long)len));
}

/* ---- vm ------------------------------------------------------------ */

void *
vm_allocate(size_t bytes, uint32_t prot)
{
	long	rv;

	rv = syscall2(SYS_VM_ALLOCATE, (long)bytes, (long)prot);
	if (rv < 0)
		return (NULL);
	return ((void *)(uintptr_t)rv);
}

int
vm_deallocate(void *va, size_t bytes)
{

	return ((int)syscall2(SYS_VM_DEALLOCATE,
	    (long)(uintptr_t)va, (long)bytes));
}
