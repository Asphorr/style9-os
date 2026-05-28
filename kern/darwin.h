/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DARWIN_H_
#define	_SYS_DARWIN_H_

/*
 * Darwin (XNU) syscall personality -- the second rung of the Mach-O
 * compatibility ladder (S2).  S1 taught the kernel to map the Mach-O
 * container (kern/macho.c); this teaches it to answer the syscalls a
 * genuine Apple-ABI binary issues.  A task is opted in when its image
 * declared PLATFORM_MACOS (see macho.h / TASK_PERSONALITY_DARWIN); for
 * those tasks syscall_dispatch calls darwin_dispatch instead of the native
 * style9 table, leaving the native path untouched.
 *
 * Apple encodes a CLASS in the high byte of the syscall number in %rax and
 * the call number in the low 24 bits.  The argument registers
 * (rdi, rsi, rdx, r10, r8, r9) are exactly the order the style9 entry stub
 * already marshals into struct syscall_frame, so no register shuffling is
 * needed -- only the number decode and the return convention differ.
 *
 * We honour a deliberately small subset, enough to prove the personality
 * end to end and translate each call onto the style9 primitive that already
 * implements it.  The mach_msg trap (S3) routes onto the kernel's existing
 * message path; MIG stub generation and a real libSystem stay userspace/S4.
 */

#define	DARWIN_SYSCALL_CLASS_SHIFT	24
#define	DARWIN_SYSCALL_CLASS_MASK	0xFFu
#define	DARWIN_SYSCALL_NUMBER_MASK	0x00FFFFFFu

/* Syscall classes (xnu osfmk/mach/i386/syscall_sw.h). */
#define	DARWIN_SYSCALL_CLASS_NONE	0
#define	DARWIN_SYSCALL_CLASS_MACH	1	/* Mach trap gate          */
#define	DARWIN_SYSCALL_CLASS_UNIX	2	/* BSD/Unix call gate      */
#define	DARWIN_SYSCALL_CLASS_MDEP	3	/* machine-dependent       */
#define	DARWIN_SYSCALL_CLASS_DIAG	4	/* diagnostics             */
#define	DARWIN_SYSCALL_CLASS_IPC	5	/* IPC                     */

/*
 * BSD (class 2) call numbers we translate -- Darwin's numbering from
 * bsd/kern/syscalls.master, NOT Linux's.  A class-2 call returns its result
 * in %rax with the carry flag clear, or a positive errno in %rax with carry
 * set; see darwin.c.
 */
#define	DARWIN_SYS_exit		1
#define	DARWIN_SYS_read		3
#define	DARWIN_SYS_write	4
#define	DARWIN_SYS_getpid	20

/*
 * Mach traps (class 1) we answer -- positive indices into xnu's
 * mach_trap_table.  These return a port name or a kern_return_t directly in
 * %rax with no carry convention.
 */
#define	DARWIN_MACH_mach_reply_port	26
#define	DARWIN_MACH_thread_self_trap	27
#define	DARWIN_MACH_task_self_trap	28
#define	DARWIN_MACH_mach_msg_trap	31	/* classic combined mach_msg() */

/*
 * BSD errno values (Darwin <sys/errno.h>) for the failures we can produce.
 * style9 has no errno of its own -- the kernel speaks MACH_E_* / ELF_E_* --
 * so darwin.c maps its internal failures onto these on the carry-set path.
 */
#define	DARWIN_EPERM	1
#define	DARWIN_EBADF	9
#define	DARWIN_ENOMEM	12
#define	DARWIN_EFAULT	14
#define	DARWIN_EINVAL	22
#define	DARWIN_ENOSYS	78

/*
 * mach_msg option flags + the mach_msg_return_t values darwin_dispatch
 * produces (Darwin <mach/message.h>).  mach_msg_trap returns one of these in
 * %rax with NO carry convention -- it is a Mach trap (class 1), so even a
 * receive timeout comes back as a code in %rax with carry clear.
 */
#define	DARWIN_MACH_SEND_MSG		0x00000001u
#define	DARWIN_MACH_RCV_MSG		0x00000002u
#define	DARWIN_MACH_SEND_TIMEOUT	0x00000010u
#define	DARWIN_MACH_RCV_TIMEOUT		0x00000100u

#define	DARWIN_MACH_MSG_SUCCESS		0x00000000
#define	DARWIN_MACH_SEND_INVALID_DATA	0x10000002
#define	DARWIN_MACH_SEND_INVALID_DEST	0x10000003
#define	DARWIN_MACH_SEND_TIMED_OUT	0x10000004
#define	DARWIN_MACH_RCV_INVALID_NAME	0x10004002
#define	DARWIN_MACH_RCV_TIMED_OUT	0x10004003
#define	DARWIN_MACH_RCV_TOO_LARGE	0x10004004
#define	DARWIN_MACH_RCV_INVALID_DATA	0x10004005

struct syscall_frame;

/*
 * Dispatch one syscall issued by a TASK_PERSONALITY_DARWIN task.  Decodes
 * the class/number out of f->sf_nr, translates onto a style9 primitive, and
 * sets the carry flag in f->sf_user_rflags per the class's return
 * convention.  Returns the value to land in the caller's %rax.
 */
long	darwin_dispatch(struct syscall_frame *f);

#endif /* !_SYS_DARWIN_H_ */
