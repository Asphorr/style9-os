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
 * We save the SysV AMD64 callee-saved set (rbp, rbx, r12-r15) -- caller-
 * saved registers are clobbered as a side-effect of any C call site, so
 * the caller is already prepared for that.  After the 6 pushes %rsp is
 * stored into *rdi; loading %rsi into %rsp swaps stacks atomically with
 * respect to interrupts (the IRQ handler operates on whichever stack is
 * current, which is now the incoming one).
 *
 * The FPU/SSE register file is saved/restored eagerly around the swap:
 * FXSAVE the outgoing state to (%rdx) on entry, FXRSTOR the incoming state
 * from (%rcx) once on the new stack.  The kernel is built -mno-sse and never
 * touches XMM, so the only state in flight belongs to ring-3 threads, and
 * per-thread save/restore keeps two SSE-using tasks from corrupting each
 * other's registers.  Both areas are 16-byte aligned (FXSAVE/FXRSTOR #GP
 * otherwise) and pre-seeded clean by fpu_clean_state at thread create.
 *
 * For brand-new threads thread_create has pre-baked the stack with
 * 6 zero-quads + a RIP pointing at thread_trampoline, so the first time
 * we ret from here we land in C inside the new thread.
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
