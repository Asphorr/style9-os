/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "elf.h"
#include "gdt.h"
#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "sched.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "usermode.h"

extern uint8_t	user_blob_start[];
extern uint8_t	user_blob_end[];

/*
 * Symbols emitted by objcopy when it wraps obj/hello.elf into
 * obj/hello_elf.o (rule in the top-level Makefile).  The wrapper
 * creates `..._start`, `..._end`, and `..._size` from the file path,
 * so the names follow the obj/ prefix.
 */
extern uint8_t	_binary_hello_elf_start[];
extern uint8_t	_binary_hello_elf_end[];

static void	usermode_launcher(void *) __attribute__((noreturn));
static void	usermode_elf_launcher(void *) __attribute__((noreturn));

void
usermode_run_first_blob(void)
{
	struct thread	*th;

	th = thread_create(kernel_task, usermode_launcher, NULL,
	    "user-blob");
	if (th == NULL)
		panic("usermode_run_first_blob: thread_create failed");
	thread_start(th);
}

void
usermode_run_hello_elf(void)
{
	struct thread	*th;

	th = thread_create(kernel_task, usermode_elf_launcher, NULL,
	    "user-elf");
	if (th == NULL)
		panic("usermode_run_hello_elf: thread_create failed");
	thread_start(th);
}

/*
 * Launcher for a ring-3 program shipped as an embedded ELF64.  Differs
 * from usermode_launcher in that elf_load handles segment mapping and
 * picks the entry RIP off e_entry; we only own the stack mapping.
 */
static void
usermode_elf_launcher(void *arg)
{
	uint64_t	*kva;
	uint64_t	 entry;
	uint64_t	 stack_pa;
	uint64_t	 ksp;
	size_t		 i;
	size_t		 image_size;
	int		 rv;

	(void)arg;

	image_size = (size_t)(_binary_hello_elf_end -
	    _binary_hello_elf_start);

	rv = elf_load(_binary_hello_elf_start, image_size, &entry);
	if (rv != ELF_E_OK)
		panic("usermode_elf_launcher: elf_load rv=%d", rv);

	stack_pa = pmm_alloc_page();
	if (stack_pa == PA_INVALID)
		panic("usermode_elf_launcher: stack alloc failed");
	if (!pmap_kenter(USER_STACK_VA, stack_pa,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER))
		panic("usermode_elf_launcher: stack map failed");

	kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
	for (i = 0; i < 512; i++)
		kva[i] = 0;

	ksp = (uint64_t)current_thread->th_kstack_base +
	    current_thread->th_kstack_size;
	tss_set_rsp0(ksp);
	syscall_kernel_rsp = ksp;

	kprintf("usermode: hello.elf entry=0x%llx (image=%zu bytes), "
	    "stack=0x%llx\n",
	    (unsigned long long)entry, image_size,
	    (unsigned long long)USER_STACK_TOP);

	usermode_enter(entry, USER_STACK_TOP);
}

/*
 * Kernel-side trampoline for the very first ring-3 program.  Runs as
 * a normal kernel thread; once the user pages are mapped + the blob
 * copied in, jumps to ring 3 with iretq.
 *
 * After iretq, the thread continues to live (the user code runs on
 * its kernel stack only when a syscall pulls it back via syscall_entry),
 * and on SYS_EXIT the kernel-side sys_exit path calls thread_exit
 * which reaps it.
 */
static void
usermode_launcher(void *arg)
{
	uint64_t	*kva;
	uint64_t	 code_pa;
	uint64_t	 stack_pa;
	size_t		 blob_len;
	size_t		 i;
	uint8_t		*src;
	uint8_t		*dst;

	(void)arg;

	code_pa  = pmm_alloc_page();
	stack_pa = pmm_alloc_page();
	if (code_pa == PA_INVALID || stack_pa == PA_INVALID)
		panic("usermode_launcher: pmm out of pages");

	if (!pmap_kenter(USER_CODE_VA, code_pa,
	    VM_PROT_READ | VM_PROT_EXEC | VM_PROT_USER))
		panic("usermode_launcher: code map failed");
	if (!pmap_kenter(USER_STACK_VA, stack_pa,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER))
		panic("usermode_launcher: stack map failed");

	/*
	 * Copy the blob into the user code page.  We touch it via the
	 * kernel-VA alias of the freshly-allocated frame (pmm_kva_from_pa)
	 * rather than USER_CODE_VA, so we do not depend on the leaf
	 * being writable from kernel side; the leaf itself is mapped
	 * read+execute for ring 3.
	 */
	blob_len = (size_t)(user_blob_end - user_blob_start);
	if (blob_len > 0x1000u)
		panic("usermode_launcher: blob > one page");

	src = user_blob_start;
	dst = (uint8_t *)pmm_kva_from_pa(code_pa);
	for (i = 0; i < blob_len; i++)
		dst[i] = src[i];

	/*
	 * Zero the user stack page so the first %rsp read is clean.
	 * (No callee-saved registers to restore on entry, but a
	 * downstream backtrace would land in garbage otherwise.)
	 */
	kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
	for (i = 0; i < 512; i++)
		kva[i] = 0;

	/*
	 * Park the kernel-stack top for both the IRQ-ring-transition
	 * path (TSS.rsp0) and the syscall fast path
	 * (syscall_kernel_rsp).  Both ultimately need the same value;
	 * keeping the syscall path in its own MSR-adjacent global is
	 * deliberate because syscall does not go through the TSS.
	 */
	{
		uint64_t	ksp;

		ksp = (uint64_t)current_thread->th_kstack_base +
		    current_thread->th_kstack_size;
		tss_set_rsp0(ksp);
		syscall_kernel_rsp = ksp;
	}

	kprintf("usermode: entering ring 3 (rip=0x%llx rsp=0x%llx, "
	    "blob=%zu bytes)\n",
	    (unsigned long long)USER_CODE_VA,
	    (unsigned long long)USER_STACK_TOP,
	    blob_len);

	usermode_enter(USER_CODE_VA, USER_STACK_TOP);
}

/*
 * Synthesise a ring-0 -> ring-3 iretq.  The CPU pops, in order:
 *	%rip, %cs, %rflags, %rsp, %ss
 * and atomically switches DPL.
 *
 *	%cs = 0x28 | 3 = 0x2B		user code64
 *	%ss = 0x20 | 3 = 0x23		user data
 *	%rflags = 0x202			IF=1, MBS=1
 */
__attribute__((noreturn))
void
usermode_enter(uint64_t user_rip, uint64_t user_rsp)
{

	__asm__ __volatile__ (
	    "pushq $0x23		\n"	/* SS               */
	    "pushq %0			\n"	/* RSP              */
	    "pushq $0x202		\n"	/* RFLAGS           */
	    "pushq $0x2B		\n"	/* CS               */
	    "pushq %1			\n"	/* RIP              */
	    "iretq			\n"
	    :
	    : "r"(user_rsp), "r"(user_rip)
	    : "memory");

	__builtin_unreachable();
}
