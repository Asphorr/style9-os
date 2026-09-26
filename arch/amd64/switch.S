/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * thread_switch_asm(uint64_t *old_rsp_save, uint64_t new_rsp,
 *     void *old_fpu, void *new_fpu)
 *
 *	rdi: address of the outgoing thread's th_rsp_save field
 *	rsi: incoming thread's saved rsp value (loaded into %rsp directly)
 *	rdx: outgoing thread's 16-byte-aligned FXSAVE area (th_fpu)
 *	rcx: incoming thread's 16-byte-aligned FXSAVE area (th_fpu)
 *
 * Saves RFLAGS and the SysV callee-saved set (rbp, rbx, r12-r15); the
 * caller has already given up the caller-saved ones.  After the 7 pushes
 * %rsp is stored into *rdi, and the single load of %rsi swaps stacks
 * atomically with respect to interrupts.
 *
 * The FPU/SSE file is switched eagerly: FXSAVE to (%rdx) on the way out,
 * FXRSTOR from (%rcx) on the new stack.  The kernel is -mno-sse, so the
 * state is always a ring-3 thread's.  Both areas must be 16-byte aligned
 * (FXSAVE/FXRSTOR #GP otherwise); fpu_clean_state seeds them at create.
 *
 * A new thread's stack is pre-baked by thread_create: RFLAGS 0x202, six
 * zero quads, and a return RIP of thread_trampoline.
 */

	.section .text
	.code64
	.globl	thread_switch_asm
	.type	thread_switch_asm, @function
thread_switch_asm:
	fxsave	(%rdx)
	pushfq
	pushq	%rbp
	pushq	%rbx
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15
	movq	%rsp, (%rdi)
	movq	%rsi, %rsp
	fxrstor	(%rcx)
	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%rbx
	popq	%rbp
	popfq
	ret
	.size	thread_switch_asm, . - thread_switch_asm
