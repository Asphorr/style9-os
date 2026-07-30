/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "mp.h"

/*
 * The road an application processor walks from reset to C.
 *
 * Three addressing modes, in this order, because the architecture allows no
 * shortcut: real mode (where it wakes up), 32-bit protected mode with paging
 * off (the only place CR4.PAE, CR3 and EFER.LME may be set up), then long
 * mode.  Each transition ends in a FAR JUMP, which is the only instruction
 * that reloads CS and therefore the only way to change what mode the next
 * instruction is decoded in.
 *
 * Everything up to long mode lives in this blob, which is copied to
 * AP_TRAMP_PA before any of it runs.  Nothing in it is patched: the blob is
 * assembled knowing where it will be, so the GDT's base and the far-jump
 * target are ordinary link-time constants.  The alternative -- assembling at
 * zero and fixing up addresses afterwards -- is the same code plus a table of
 * offsets that must agree with it, and the failure mode of a disagreement is
 * a processor jumping into the middle of nowhere with no way to say so.
 *
 * Long mode is reached at a KERNEL symbol, not one in the blob, because by
 * then paging is on and the kernel's identity map makes its own addresses
 * mean what they say.  So the copied part ends the moment there is a page
 * table, and the rest is ordinary kernel text that a debugger can find.
 */

#define	TRAMP(x)	(AP_TRAMP_PA + ((x) - ap_tramp_start))

	.section .rodata.aptramp, "a", @progbits
	.balign	16
	.globl	ap_tramp_start
	.globl	ap_tramp_end

	.code16
ap_tramp_start:
	cli
	cld

	/*
	 * Flat 16-bit segments with base zero, so that every address in this
	 * blob is the physical address it was assembled with -- CS is whatever
	 * the startup message made it, and relying on it would mean two
	 * different ways of naming the same byte.
	 */
	xorw	%ax, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	lgdtl	TRAMP(ap_tramp_gdt_desc)

	movl	%cr0, %eax
	orl	$0x1, %eax		/* PE: protected mode, paging off */
	movl	%eax, %cr0

	/*
	 * ljmpl $AP_SEL_CODE32, $TRAMP(ap_tramp_pm32), spelled by hand: in
	 * 16-bit code the assembler would encode a 16-bit offset, and this one
	 * needs 32.  0x66 is the operand-size override that makes EA take
	 * ptr16:32.
	 */
	.byte	0x66, 0xEA
	.long	TRAMP(ap_tramp_pm32)
	.word	AP_SEL_CODE32

	.code32
	.balign	4
ap_tramp_pm32:
	movw	$AP_SEL_DATA, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	/*
	 * The exact order the architecture requires: PAE before a CR3 that
	 * means anything under it, CR3 before paging, LME before paging.  And
	 * NXE with it -- not optional here, because this kernel's page tables
	 * DO set the no-execute bit, and bit 63 of a PTE is a reserved-bit
	 * violation rather than a permission while EFER.NXE is clear.  An AP
	 * that skipped it would take a page fault on its first instruction
	 * after paging, with no IDT to take it on.
	 */
	movl	$0x20, %eax			/* CR4.PAE               */
	movl	%eax, %cr4

	movl	AP_PARAM_PA + AP_P_CR3, %eax
	movl	%eax, %cr3

	movl	$0xC0000080, %ecx		/* MSR_EFER              */
	rdmsr
	orl	$0x900, %eax			/* LME (8) | NXE (11)    */
	wrmsr

	movl	%cr0, %eax
	orl	$0x80000000, %eax		/* PG: long mode is live */
	movl	%eax, %cr0

	ljmp	$AP_SEL_CODE64, $ap_start64

	/*
	 * The trampoline's own GDT: null, code64, data, code32.  The first two
	 * match the kernel's table on purpose (see mp.h), so that reaching C
	 * with CS = 0x08 is already correct and gdt_init_cpu changes nothing
	 * about the segment this processor is executing in.
	 */
	.balign	8
ap_tramp_gdt:
	.quad	0x0000000000000000		/* null                  */
	.quad	0x00AF9A000000FFFF		/* 0x08 code64, DPL0     */
	.quad	0x00CF92000000FFFF		/* 0x10 data,   DPL0     */
	.quad	0x00CF9A000000FFFF		/* 0x18 code32, DPL0     */
ap_tramp_gdt_end:

ap_tramp_gdt_desc:
	.word	ap_tramp_gdt_end - ap_tramp_gdt - 1
	.long	TRAMP(ap_tramp_gdt)

ap_tramp_end:

/*
 * Long mode, and out of the blob: this is ordinary kernel text, reached by
 * absolute address because the identity map is what the CR3 above installed.
 *
 * The parameter block is read here rather than carried in registers across the
 * mode switches, because a 64-bit stack pointer does not fit in a 32-bit
 * register and splitting it would be two more things to get right.
 *
 * %fs and %gs are deliberately NOT touched.  Loading a segment register zeroes
 * its base, the per-CPU block is reached through the GS base, and the first
 * thing ap_entry does is install it -- so the one instruction that could undo
 * that is the one not written here.
 */
	.text
	.code64
	.globl	ap_start64
ap_start64:
	movw	$AP_SEL_DATA, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %ss

	movq	$AP_PARAM_PA, %rax
	movq	AP_P_RSP(%rax), %rsp
	movq	AP_P_CPU(%rax), %rdi

	xorq	%rbp, %rbp
	movabsq	$ap_entry, %rax
	callq	*%rax

	/*
	 * ap_entry does not return.  If it ever does, this processor stops
	 * here rather than falling into whatever follows in .text -- a CPU
	 * executing the next function by accident is the worst possible way
	 * to find out.
	 */
1:
	cli
	hlt
	jmp	1b

	.section .note.GNU-stack, "", @progbits
