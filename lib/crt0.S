/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * libstyle9 ring-3 startup glue.
 *
 * The kernel's ELF loader installs PT_LOAD segments, lands the new
 * thread on `e_entry` with an empty stack at USER_STACK_TOP, and
 * iretq's to ring 3.  We are e_entry: zero the integer registers
 * (so a backtrace through main's prologue doesn't dig into garbage),
 * call main, then call exit with the return value.
 *
 * .bss is already zero -- the loader allocates fresh frames and zeros
 * them before mapping in elf.c's load_segment().  No initializer loop
 * needed here.
 *
 * Argc / argv: the kernel launcher (arch/amd64/usermode.c
 * build_user_arg_stack) lays down a SysV-style frame at entry %rsp --
 * argc at 0(%rsp), the argv pointer array (argc entries + a NULL
 * terminator) starting at 8(%rsp).  %rsp is 16-byte aligned on entry.
 * An argument-free spawn still gets a valid (argc=0, argv[0]=NULL)
 * frame, so this path is taken unconditionally.  No envp / auxv yet.
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
	/*
	 * exit is __attribute__((noreturn)) but a defensive ud2 here
	 * guarantees we never fall through into whatever happens to live
	 * past the function in memory (the linker may or may not pad
	 * with int3s depending on flags).
	 */
	ud2
	.size	_start, . - _start
