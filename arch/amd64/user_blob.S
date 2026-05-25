/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Inline ring-3 program embedded as raw bytes in the kernel image.
 * Linked into .rodata; the launcher copies user_blob_start ..
 * user_blob_end into a user-mapped page and jumps to it with iretq.
 *
 * The code is position-independent (RIP-relative leaq) so it works
 * regardless of which VA the launcher chooses for the user page.
 *
 * What it does:
 *	syscall(SYS_PRINT, "hello ring 3\n", 13)
 *	syscall(SYS_EXIT, 0)
 *	(loop forever if SYS_EXIT mysteriously returned)
 */

.section .rodata
.align	16

.global	user_blob_start
.global	user_blob_end

user_blob_start:
	/* leaq msg(%rip), %rdi */
	leaq	msg(%rip), %rdi
	/* movq $13, %rsi */
	movq	$13, %rsi
	/* movq $0, %rax  -- SYS_PRINT */
	movq	$0, %rax
	syscall

	/* movq $1, %rax  -- SYS_EXIT */
	movq	$1, %rax
	/* movq $0, %rdi  -- code = 0 */
	movq	$0, %rdi
	syscall

	/* SYS_EXIT must not return; if it does, halt the user-mode CPU. */
1:	jmp	1b

msg:
	.ascii	"hello ring 3\n"

user_blob_end:
