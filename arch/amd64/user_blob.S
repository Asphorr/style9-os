/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Inline ring-3 program, kept in .rodata; usermode_launcher copies
 * user_blob_start .. user_blob_end into a user page and iretq's to it.
 * Only usermode_run_first_blob starts that, and nothing calls it now.
 * Position-independent, so the page's VA does not matter.
 *
 *	syscall(SYS_PRINT, "hello ring 3\n", 13)
 *	syscall(SYS_EXIT, 0)
 */

.section .rodata
.align	16

.global	user_blob_start
.global	user_blob_end

user_blob_start:
	leaq	msg(%rip), %rdi
	movq	$13, %rsi
	/* SYS_PRINT */
	movq	$0, %rax
	syscall

	/* SYS_EXIT, code 0 */
	movq	$1, %rax
	movq	$0, %rdi
	syscall

	/* SYS_EXIT must not return; if it does, spin. */
1:	jmp	1b

msg:
	.ascii	"hello ring 3\n"

user_blob_end:
