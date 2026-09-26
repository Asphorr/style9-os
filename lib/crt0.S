/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * libstyle9 ring-3 startup glue.
 *
 * The kernel's ELF loader maps the PT_LOAD segments and iretq's to
 * e_entry, which is _start: clear %rbp to end the frame chain, call
 * main, then exit with its return value.  .bss needs no loop; the
 * loader (vm_map_image) zero-fills it.
 *
 * Entry %rsp is 16-byte aligned and points at a SysV-style frame laid
 * down by build_user_arg_stack (arch/amd64/usermode.c): argc at
 * 0(%rsp), then argc argv pointers and a NULL from 8(%rsp).  A spawn
 * with no arguments still gets argc = 0, argv[0] = NULL.  No envp or
 * auxv yet.
 */

	.section .text._start, "ax", @progbits
	.code64
	.globl	_start
	.type	_start, @function
_start:
	xorq	%rbp, %rbp		/* terminate frame chain        */
	movq	0(%rsp), %rdi		/* argc                         */
	leaq	8(%rsp), %rsi		/* argv (&argv[0])              */
	andq	$-16, %rsp		/* SysV: keep 16-byte alignment */
	call	main
	movl	%eax, %edi		/* main return -> exit code     */
	call	exit
	/* exit is noreturn; trap rather than run into what follows. */
	ud2
	.size	_start, . - _start
