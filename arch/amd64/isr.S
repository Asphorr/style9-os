/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * IDT stub bodies (x86_64).
 *
 * One stub per architectural vector 0..47.  Each stub normalises the
 * trap frame -- pushing a dummy err code on vectors that the CPU does
 * not push one for -- then falls through to isr_common, which saves
 * the 15 GPRs and calls intr_dispatch in C.
 *
 * isr_table at the bottom is a flat array of stub addresses indexed by
 * vector number; the C side reads it to populate the IDT.
 *
 * Calling convention reminder: SysV AMD64 places arg 0 in %rdi; we
 * leave %rsp pointing at the trapframe and move it into %rdi before
 * calling intr_dispatch.
 */

	.section .text
	.code64

	.macro ISR_NOERR n
	.globl	isr_\n
	.type	isr_\n, @function
isr_\n:
	pushq	$0
	pushq	$\n
	jmp	isr_common
	.size	isr_\n, . - isr_\n
	.endm

	.macro ISR_ERR n
	.globl	isr_\n
	.type	isr_\n, @function
isr_\n:
	pushq	$\n
	jmp	isr_common
	.size	isr_\n, . - isr_\n
	.endm

/* Vectors 0-31: CPU exceptions.  #DF/#TS/#NP/#SS/#GP/#PF/#AC push err. */
ISR_NOERR 0
ISR_NOERR 1
ISR_NOERR 2
ISR_NOERR 3
ISR_NOERR 4
ISR_NOERR 5
ISR_NOERR 6
ISR_NOERR 7
ISR_ERR   8
ISR_NOERR 9
ISR_ERR   10
ISR_ERR   11
ISR_ERR   12
ISR_ERR   13
ISR_ERR   14
ISR_NOERR 15
ISR_NOERR 16
ISR_ERR   17
ISR_NOERR 18
ISR_NOERR 19
ISR_NOERR 20
ISR_NOERR 21
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

/* Vectors 32-47: hardware IRQs after PIC remap.  None push an err. */
ISR_NOERR 32
ISR_NOERR 33
ISR_NOERR 34
ISR_NOERR 35
ISR_NOERR 36
ISR_NOERR 37
ISR_NOERR 38
ISR_NOERR 39
ISR_NOERR 40
ISR_NOERR 41
ISR_NOERR 42
ISR_NOERR 43
ISR_NOERR 44
ISR_NOERR 45
ISR_NOERR 46
ISR_NOERR 47

/*
 * Common dispatcher tail (x86_64).
 *
 * Stack on entry (low -> high):
 *	[rsp+0]	trap vector		(just pushed)
 *	[rsp+8]	err code		(CPU- or stub-pushed)
 *	[rsp+16]saved RIP
 *	[rsp+24]saved CS
 *	[rsp+32]saved RFLAGS
 *	[rsp+40]saved RSP	(CPU pushes always in long mode)
 *	[rsp+48]saved SS
 *
 * We push the 15 GPRs (no pusha in 64-bit) so %rsp ends up pointing at
 * a populated struct trapframe.  The trap frame's low offsets are r15
 * first (lowest address) through rax (just below the dwords pushed
 * earlier).  Calling intr_dispatch with the trapframe in %rdi follows
 * SysV-AMD64 arg passing.
 */
	.globl	isr_common
	.type	isr_common, @function
isr_common:
	pushq	%rax
	pushq	%rcx
	pushq	%rdx
	pushq	%rbx
	pushq	%rbp
	pushq	%rsi
	pushq	%rdi
	pushq	%r8
	pushq	%r9
	pushq	%r10
	pushq	%r11
	pushq	%r12
	pushq	%r13
	pushq	%r14
	pushq	%r15

	cld
	movq	%rsp, %rdi			/* arg = struct trapframe * */
	call	intr_dispatch

	popq	%r15
	popq	%r14
	popq	%r13
	popq	%r12
	popq	%r11
	popq	%r10
	popq	%r9
	popq	%r8
	popq	%rdi
	popq	%rsi
	popq	%rbp
	popq	%rbx
	popq	%rdx
	popq	%rcx
	popq	%rax

	addq	$16, %rsp			/* discard trapno + err     */
	iretq
	.size	isr_common, . - isr_common

/*
 * Vector-number -> stub-address table.  idt_init() reads this when
 * populating the IDT so each gate calls the right normalising stub.
 */
	.section .rodata
	.globl	isr_table
	.type	isr_table, @object
	.align	8
isr_table:
	.quad	isr_0,  isr_1,  isr_2,  isr_3,  isr_4,  isr_5,  isr_6,  isr_7
	.quad	isr_8,  isr_9,  isr_10, isr_11, isr_12, isr_13, isr_14, isr_15
	.quad	isr_16, isr_17, isr_18, isr_19, isr_20, isr_21, isr_22, isr_23
	.quad	isr_24, isr_25, isr_26, isr_27, isr_28, isr_29, isr_30, isr_31
	.quad	isr_32, isr_33, isr_34, isr_35, isr_36, isr_37, isr_38, isr_39
	.quad	isr_40, isr_41, isr_42, isr_43, isr_44, isr_45, isr_46, isr_47
	.size	isr_table, . - isr_table
