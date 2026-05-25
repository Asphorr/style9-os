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
 * Single-CPU model: we stash the user %rsp in a global scratch slot
 * and load the kernel stack from `syscall_kernel_rsp`, which the
 * scheduler installs as part of every user-thread dispatch.  Once
 * SMP arrives this moves to per-CPU via GS-base, but the structure
 * of the entry stays the same.
 *
 * Argument convention follows real Mach / Linux:
 *	%rax	syscall number
 *	%rdi	arg0   %rsi  arg1   %rdx  arg2
 *	%r10	arg3 (NOT %rcx -- SYSCALL clobbered it)
 *	%r8	arg4   %r9   arg5
 * Return value goes back in %rax.
 */

.section .text

.global	syscall_entry
.type	syscall_entry, @function

.global	syscall_kernel_rsp
.global	syscall_user_rsp_scratch

syscall_entry:
	movq	%rsp, syscall_user_rsp_scratch(%rip)
	movq	syscall_kernel_rsp(%rip), %rsp

	/*
	 * Build a struct syscall_frame on the kernel stack, in field
	 * order.  Matches the layout in kern/syscall.h exactly so the
	 * C dispatcher can use ->arg0 / ->nr / ->user_rip naming.
	 */
	pushq	syscall_user_rsp_scratch(%rip)	/* +72 user_rsp     */
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
	 * Restore the bits SYSRETQ needs: %rcx <- user RIP,
	 * %r11 <- user RFLAGS, %rsp <- user RSP.  Use %rdx as a
	 * scratch for the RSP transfer; %rdx isn't preserved across
	 * a syscall (caller-save).
	 */
	movq	56(%rsp), %r11
	movq	64(%rsp), %rcx
	movq	72(%rsp), %rdx

	addq	$80, %rsp			/* tear down frame */

	movq	%rdx, %rsp			/* back to user stack */

	sysretq

.size	syscall_entry, .-syscall_entry

.section .data
.align	8

syscall_kernel_rsp:
	.quad	0

syscall_user_rsp_scratch:
	.quad	0
