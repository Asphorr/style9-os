/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * darwinhello -- a freestanding ring-3 program that speaks the Darwin (XNU)
 * syscall ABI, NOT style9.  It links against no libc and no crt0: _start
 * issues genuine Apple class-encoded `syscall`s by hand -- the same bytes a
 * macOS x86-64 binary's libSystem stubs emit.
 *
 * Apple puts a CLASS in the high byte of the number in %rax: class 2 is the
 * BSD/Unix gate (write 0x2000004, getpid 0x2000014, exit 0x2000001), class 1
 * is the Mach trap gate (task_self_trap 0x100001C, mach_reply_port
 * 0x100001A).  Arguments follow the System V / Darwin syscall convention
 * (rdi, rsi, rdx, r10, r8, r9 -- the same registers style9 uses), and a BSD
 * call reports failure by setting the carry flag with a positive errno in
 * %rax.
 *
 * elf2macho's `macos` mode wraps this ELF as a Mach-O carrying an
 * LC_BUILD_VERSION naming PLATFORM_MACOS, so macho_load tags the task
 * TASK_PERSONALITY_DARWIN and the kernel routes these syscalls through
 * darwin_dispatch (kern/darwin.c) -- the S2 path -- instead of the native
 * style9 table.
 *
 * Sequence (each step is visible in the boot transcript, via the program's
 * own write() or the kernel's darwin_dispatch trace):
 *	write(1, banner)                    class-2 round trip, carry clear
 *	getpid()                            class-2 value return (our task id)
 *	task_self_trap(), mach_reply_port() class-1 Mach traps
 *	syscall 0x20000FF (unimplemented)   must come back carry-set (errno)
 *	write(1, ok|bug per carry)          proves the error convention
 *	exit(0)
 */

/* Darwin class-encoded syscall numbers (high byte = class). */
.set	SYS_exit,		0x2000001	/* class 2, BSD nr 1   */
.set	SYS_write,		0x2000004	/* class 2, BSD nr 4   */
.set	SYS_getpid,		0x2000014	/* class 2, BSD nr 20  */
.set	SYS_bad,		0x20000FF	/* class 2, BSD nr 255 (unimplemented) */
.set	MACH_reply_port,	0x100001A	/* class 1, trap 26    */
.set	MACH_task_self,		0x100001C	/* class 1, trap 28    */

.section .text._start
.global	_start

_start:
	/* write(1, banner, banner_len): full class-2 round trip. */
	movl	$SYS_write, %eax
	movl	$1, %edi
	leaq	banner(%rip), %rsi
	movl	$(banner_end - banner), %edx
	syscall

	/* getpid(): class-2 value return; the kernel logs our task id. */
	movl	$SYS_getpid, %eax
	syscall

	/* task_self_trap(): Mach class 1, returns the task-self port name. */
	movl	$MACH_task_self, %eax
	syscall

	/* mach_reply_port(): Mach class 1, allocates a fresh reply port. */
	movl	$MACH_reply_port, %eax
	syscall

	/* Unimplemented BSD call: the kernel must report errno + carry set. */
	movl	$SYS_bad, %eax
	syscall
	jc	1f			/* CF set => Darwin error convention held */

	/* Fell through: carry clear on an unimplemented call is a bug. */
	movl	$SYS_write, %eax
	movl	$1, %edi
	leaq	bugmsg(%rip), %rsi
	movl	$(bugmsg_end - bugmsg), %edx
	syscall
	jmp	2f

1:	/* Expected errno path: announce it. */
	movl	$SYS_write, %eax
	movl	$1, %edi
	leaq	okmsg(%rip), %rsi
	movl	$(okmsg_end - okmsg), %edx
	syscall

2:	/* exit(0): class-2, no return. */
	movl	$SYS_exit, %eax
	xorl	%edi, %edi
	syscall

	/* exit must not return; spin if it somehow does. */
3:	jmp	3b

.section .rodata
banner:
	.ascii	"darwinhello: I think I'm on macOS -- issuing Darwin class-encoded syscalls\n"
banner_end:
okmsg:
	.ascii	"darwinhello: unimplemented call returned errno+carry, as Darwin expects\n"
okmsg_end:
bugmsg:
	.ascii	"darwinhello: BUG -- unimplemented call left carry clear\n"
bugmsg_end:
