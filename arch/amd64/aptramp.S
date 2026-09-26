/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "mp.h"

/*
 * An application processor's path from reset to C.
 *
 * Real mode (where it wakes), 32-bit protected mode with paging off (where
 * CR4.PAE, CR3 and EFER.LME are set up), then long mode.  Each transition
 * ends in a far jump, the only way to reload CS and so change how the next
 * instruction is decoded.
 *
 * Everything up to long mode is this blob, copied to AP_TRAMP_PA before it
 * runs.  It is assembled for that address (TRAMP()), so the GDT base and
 * far-jump targets are link-time constants and nothing is patched.  Long
 * mode is entered at a kernel symbol, ap_start64, reachable through the
 * kernel's identity map: the copied part ends as soon as there are page
 * tables.
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
	 * Data segments with base zero, so every address in the blob is the
	 * physical address it was assembled with; CS is whatever the STARTUP
	 * IPI made it and is not relied on.
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
	 * ljmpl $AP_SEL_CODE32, $TRAMP(ap_tramp_pm32), by hand: in 16-bit
	 * code the assembler would encode a 16-bit offset.  0x66 makes EA
	 * take ptr16:32.
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
	 * PAE, then CR3, then EFER.LME, all before paging.  NXE too: the
	 * kernel's page tables set bit 63, which is a reserved-bit fault
	 * while EFER.NXE is clear -- on the first instruction after paging,
	 * with no IDT to take it.
	 */
	movl	$0x20, %eax			/* CR4.PAE               */
	movl	%eax, %cr4

	movl	AP_PARAM_PA + AP_P_CR3, %eax
	movl	%eax, %cr3

	movl	$0xC0000080, %ecx		/* MSR_EFER              */
	rdmsr
	orl	$0x900, %eax			/* LME (8) | NXE (11)    */
	wrmsr

	/*
	 * And turn the caches on: CR0.CD and CR0.NW are set after INIT, and
	 * an AP that kept them would run correctly but uncached (emulators
	 * ignore the bits, so only real hardware shows it).  The boot CPU's
	 * were cleared by the firmware.
	 */
	movl	%cr0, %eax
	andl	$0x9FFFFFFF, %eax		/* CD, NW: caches on     */
	orl	$0x80000000, %eax		/* PG: long mode is live */
	movl	%eax, %cr0

	ljmp	$AP_SEL_CODE64, $ap_start64

	/*
	 * The trampoline's GDT: null, code64, data, code32.  code64 and data
	 * match the kernel's (see mp.h), so gdt_init_cpu changes nothing
	 * under CS = 0x08.
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
 * Long mode, out of the blob: ordinary kernel text, reached by absolute
 * address through the identity map the CR3 above installed.  The parameter
 * block is read here, since a 64-bit stack pointer does not survive the
 * mode switches in a 32-bit register.
 *
 * %fs and %gs are left alone: the per-CPU block lives in the GS base, which
 * ap_entry installs first thing, and nothing may reload %gs after that.
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
	 * ap_entry does not return; if it did, stop here rather than fall
	 * into whatever follows in .text.
	 */
1:
	cli
	hlt
	jmp	1b

	.section .note.GNU-stack, "", @progbits
