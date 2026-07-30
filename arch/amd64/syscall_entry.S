/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Entry stub for the SYSCALL instruction.  On entry from ring 3 the
 * CPU has:
 *	- saved user RIP into %rcx
 *	- saved user RFLAGS into %r11
 *	- loaded %cs = STAR[47:32] (0x08), %ss = +8 (0x10)
 *	- loaded %rip = LSTAR (this address)
 *	- cleared bits in %rflags per FMASK (IF + DF, so we run with
 *	  interrupts disabled)
 *	- NOT switched the stack -- %rsp still points at user space
 *
 * There is no register to spare on entry -- %rsp still points into ring 3
 * and every GPR holds either an argument or something the ABI promises to
 * give back -- so the kernel stack has to come out of memory, and the
 * address of that memory has to come from somewhere that needs no register
 * to find.  That is what the GS base is for: %gs:CPU_KERNEL_RSP is this
 * CPU's kernel stack top, installed by the scheduler on every dispatch into
 * a user thread, and %gs:CPU_USER_RSP is this CPU's one-slot parking space
 * for the ring-3 %rsp we are about to overwrite.
 *
 * BOTH USED TO BE GLOBALS, which was correct for exactly as long as there
 * was one CPU: two CPUs entering a syscall at once would each stash their
 * user stack in the same word, and the second one home would return the
 * first one's ring-3 %rsp to both.  The structure of the entry is unchanged
 * -- only where the two words live.
 *
 * Argument convention follows real Mach / Linux:
 *	%rax	syscall number
 *	%rdi	arg0   %rsi  arg1   %rdx  arg2
 *	%r10	arg3 (NOT %rcx -- SYSCALL clobbered it)
 *	%r8	arg4   %r9   arg5
 * Return value goes back in %rax.
 */

#include "cpu.h"

.section .text

.global	syscall_entry
.type	syscall_entry, @function

syscall_entry:
	movq	%rsp, %gs:CPU_USER_RSP
	movq	%gs:CPU_KERNEL_RSP, %rsp

	/*
	 * Build a struct syscall_frame on the kernel stack, in field
	 * order.  Matches the layout in kern/syscall.h exactly so the
	 * C dispatcher can use ->arg0 / ->nr / ->user_rip naming.
	 */
	pushq	%gs:CPU_USER_RSP		/* +72 user_rsp     */
	pushq	%rcx				/* +64 user_rip     */
	pushq	%r11				/* +56 user_rflags  */
	pushq	%rax				/* +48 nr           */
	pushq	%r9				/* +40 arg5         */
	pushq	%r8				/* +32 arg4         */
	pushq	%r10				/* +24 arg3         */
	pushq	%rdx				/* +16 arg2         */
	pushq	%rsi				/*  +8 arg1         */
	pushq	%rdi				/*  +0 arg0         */

	movq	%rsp, %rdi			/* &frame */
	call	syscall_dispatch
	/* %rax holds the syscall's return value */

	/*
	 * Restore caller-save state.  The Linux x86_64 SYSCALL ABI
	 * (which our libc-style user wrappers expect) says the
	 * kernel preserves everything except %rax (return), %rcx
	 * (saved RIP), and %r11 (saved RFLAGS).  GCC emits code for
	 * the user side assuming exactly that, so we mirror it
	 * here: arg registers come back as the user saw them on the
	 * way in.
	 *
	 * Order: stash user RSP in the per-CPU slot first (we cannot
	 * park it in a register, since all the ones we could use are
	 * either return-related or about to be restored).
	 */
	movq	72(%rsp), %rdx
	movq	%rdx, %gs:CPU_USER_RSP

	movq	56(%rsp), %r11			/* user RFLAGS */
	movq	64(%rsp), %rcx			/* user RIP    */

	movq	 0(%rsp), %rdi
	movq	 8(%rsp), %rsi
	movq	16(%rsp), %rdx
	movq	24(%rsp), %r10
	movq	32(%rsp), %r8
	movq	40(%rsp), %r9

	addq	$80, %rsp			/* tear down frame */

	movq	%gs:CPU_USER_RSP, %rsp

	sysretq

.size	syscall_entry, .-syscall_entry
