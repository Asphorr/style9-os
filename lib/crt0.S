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
 * Argc / argv is plumbed as zero / NULL for now.  When the program
 * loader grows command-line argument passing, we'll spell out the
 * stack layout it puts together and adjust this stub to pull argv
 * off it the way SysV mandates.
 */

	.section .text._start, "ax", @progbits
	.code64
	.globl	_start
	.type	_start, @function
_start:
	xorq	%rbp, %rbp		/* terminate frame chain        */
	xorq	%rdi, %rdi		/* argc = 0                     */
	xorq	%rsi, %rsi		/* argv = NULL                  */
	andq	$-16, %rsp		/* SysV: 16-byte alignment      */
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
