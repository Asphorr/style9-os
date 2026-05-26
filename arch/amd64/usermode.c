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
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "sched.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "usermode.h"
#include "vm.h"

extern uint8_t	user_blob_start[];
extern uint8_t	user_blob_end[];

/*
 * Boot-time blob path: the very first ring-3 program (a hand-written
 * scrap of asm in arch/amd64/user_blob.S) for early SYS_PRINT/EXIT
 * smoke testing.  Kept around because it is still a viable "no libc"
 * minimal ring-3 entry point.
 */
static void	usermode_launcher(void *) __attribute__((noreturn));

/*
 * Carries a spawn intent across the thread boundary.  Allocated by
 * arch_spawn_user, freed by the launcher just before it iretq's so
 * leaks don't accumulate if the program loops forever in ring 3.
 */
struct user_spawn_arg {
	const char	*sa_name;
	const uint8_t	*sa_image;
	size_t		 sa_image_size;
};

static void	usermode_elf_launcher(void *) __attribute__((noreturn));

void
usermode_run_first_blob(void)
{
	struct task	*ut;
	struct thread	*th;

	ut = task_create("user-blob");
	if (ut == NULL)
		panic("usermode_run_first_blob: task_create failed");
	th = thread_create(ut, usermode_launcher, NULL, "user-blob");
	if (th == NULL)
		panic("usermode_run_first_blob: thread_create failed");
	thread_start(th);
}

/*
 * Generic user-program spawn entry.  Called from progreg_spawn (which
 * looked up the embedded ELF blob in the program registry).  Builds a
 * fresh task, allocates a small descriptor on the kernel heap to hand
 * the image pointer + name across to the launcher thread, then
 * thread_starts it.  Returns the new task's id, or a negative
 * SYS_E_* on allocation failure.
 *
 * The descriptor is heap-allocated rather than stack so it survives
 * after arch_spawn_user returns; the launcher takes ownership and
 * kfrees it during setup.
 */
long
arch_spawn_user(const char *name, const uint8_t *image, size_t image_size)
{
	struct user_spawn_arg	*sa;
	struct task		*ut;
	struct thread		*th;

	if (name == NULL || image == NULL || image_size == 0)
		return (SYS_E_INVAL);

	sa = (struct user_spawn_arg *)kmalloc(sizeof(*sa));
	if (sa == NULL)
		return (SYS_E_NOSYS);	/* OOM; closest in our small set */
	sa->sa_name       = name;
	sa->sa_image      = image;
	sa->sa_image_size = image_size;

	ut = task_create(name);
	if (ut == NULL) {
		kfree(sa);
		return (SYS_E_NOSYS);
	}

	th = thread_create(ut, usermode_elf_launcher, sa, "user-elf");
	if (th == NULL) {
		task_deref(ut);
		kfree(sa);
		return (SYS_E_NOSYS);
	}
	thread_start(th);

	/*
	 * Drop the creator's ref now that the task is anchored by its
	 * thread.  task_create returns t_refs=1; thread_create's
	 * task_attach_thread bumps it to 2.  Without this release, the
	 * final task_detach_thread on the exiting user thread would only
	 * take refs 2 -> 1, never to 0, and the dead task would linger in
	 * task_list forever -- SYS_TASK_ALIVE would keep reporting it
	 * alive and the userspace shell's wait_child yield-spin would
	 * never terminate.
	 *
	 * Safe to drop here because the thread is already enqueued: the
	 * scheduler holds a stable view of the task via th_task whether
	 * or not the thread has run yet, and task_deref only frees the
	 * task when t_refs hits zero AND t_nthreads == 0 (KASSERT'd).
	 */
	task_deref(ut);
	return ((long)ut->t_id);
}

/*
 * Launcher for a ring-3 program shipped as an embedded ELF64.  Differs
 * from usermode_launcher in that elf_load handles segment mapping and
 * picks the entry RIP off e_entry; we only own the stack mapping.
 *
 * Runs as a kernel thread attached to the freshly-created user task, so
 * by the time we get here the scheduler has already loaded our task's
 * CR3 -- elf_load's pmap_enter calls land in (and TLB-flush) the live
 * page table.
 */
static void
usermode_elf_launcher(void *arg)
{
	struct user_spawn_arg	*sa;
	struct task		*ut;
	uint64_t		*kva;
	uint64_t		 entry;
	uint64_t		 stack_pa;
	uint64_t		 ksp;
	size_t			 i;
	int			 rv;

	sa = (struct user_spawn_arg *)arg;
	ut = current_thread->th_task;

	rv = elf_load(ut, sa->sa_image, sa->sa_image_size, &entry);
	if (rv != ELF_E_OK)
		panic("usermode_elf_launcher: elf_load %s rv=%d",
		    sa->sa_name, rv);

	stack_pa = pmm_alloc_page();
	if (stack_pa == PA_INVALID)
		panic("usermode_elf_launcher: stack alloc failed");
	if (!pmap_enter(ut->t_pmap, USER_STACK_VA, stack_pa,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER))
		panic("usermode_elf_launcher: stack map failed");
	if (!vm_map_enter(ut->t_map, USER_STACK_VA, 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER, VME_F_ANON))
		panic("usermode_elf_launcher: vm_map_enter stack");

	kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
	for (i = 0; i < 512; i++)
		kva[i] = 0;

	ksp = (uint64_t)current_thread->th_kstack_base +
	    current_thread->th_kstack_size;
	tss_set_rsp0(ksp);
	syscall_kernel_rsp = ksp;

	kprintf("usermode: spawn '%s' entry=0x%llx (image=%zu bytes), "
	    "stack=0x%llx\n",
	    sa->sa_name, (unsigned long long)entry, sa->sa_image_size,
	    (unsigned long long)USER_STACK_TOP);

	kfree(sa);
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
	struct task	*ut;
	uint64_t	*kva;
	uint64_t	 code_pa;
	uint64_t	 stack_pa;
	size_t		 blob_len;
	size_t		 i;
	uint8_t		*src;
	uint8_t		*dst;

	(void)arg;

	ut = current_thread->th_task;

	code_pa  = pmm_alloc_page();
	stack_pa = pmm_alloc_page();
	if (code_pa == PA_INVALID || stack_pa == PA_INVALID)
		panic("usermode_launcher: pmm out of pages");

	if (!pmap_enter(ut->t_pmap, USER_CODE_VA, code_pa,
	    VM_PROT_READ | VM_PROT_EXEC | VM_PROT_USER))
		panic("usermode_launcher: code map failed");
	if (!pmap_enter(ut->t_pmap, USER_STACK_VA, stack_pa,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER))
		panic("usermode_launcher: stack map failed");

	if (!vm_map_enter(ut->t_map, USER_CODE_VA, 0x1000,
	    VM_PROT_READ | VM_PROT_EXEC | VM_PROT_USER, VME_F_ANON))
		panic("usermode_launcher: vm_map_enter code");
	if (!vm_map_enter(ut->t_map, USER_STACK_VA, 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER, VME_F_ANON))
		panic("usermode_launcher: vm_map_enter stack");

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
	    "blob=%zu bytes, task=%s)\n",
	    (unsigned long long)USER_CODE_VA,
	    (unsigned long long)USER_STACK_TOP,
	    blob_len, ut->t_name);

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
