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
 * No register is free on entry, so the kernel stack is found through the
 * GS base: %gs:CPU_KERNEL_RSP is this CPU's kernel stack top, re-armed by
 * the scheduler on every switch, and %gs:CPU_USER_RSP is a per-CPU slot
 * for the ring-3 %rsp while it is replaced.
 *
 * Argument convention follows Linux:
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

	/* Build a struct syscall_frame (kern/syscall.h), in field order. */
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
	 * As on Linux, the kernel preserves everything but %rax (return),
	 * %rcx (RIP) and %r11 (RFLAGS), so the argument registers go back
	 * as the user passed them.
	 *
	 * The user %rsp is taken from the frame, not the per-CPU slot:
	 * another thread's syscall may have reused the slot meanwhile.  It
	 * is parked in the slot again because every free register is about
	 * to be restored.
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
