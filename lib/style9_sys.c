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
