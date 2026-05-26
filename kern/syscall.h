/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_SYSCALL_H_
#define	_SYS_SYSCALL_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Kernel syscall ABI.
 *
 * Ring-3 callers issue the `syscall` instruction with the number in
 * %rax and up to six arguments in %rdi, %rsi, %rdx, %r10, %r8, %r9.
 * The return value comes back in %rax; negative values are kernel
 * error codes (see SYS_E_*).  The entry stub in
 * arch/amd64/syscall_entry.S marshals everything into a struct
 * syscall_frame and calls syscall_dispatch().
 *
 * The numbering is deliberately sparse so each subsystem can grow its
 * own block without renumbering existing ones.
 */

#define	SYS_PRINT		0	/* (const char *buf, size_t len) -> bytes  */
#define	SYS_EXIT		1	/* (int code) -> NORETURN                  */
#define	SYS_YIELD		2	/* ()         -> 0                         */
#define	SYS_PORT_ALLOC		3	/* (uint8_t right_mask)         -> name    */
#define	SYS_PORT_DEALLOC	4	/* (mach_port_name_t name)      -> 0/err   */
#define	SYS_MSG_SEND		5	/* (struct mach_msg_header *)   -> 0/err   */
#define	SYS_MSG_RECV		6	/* (name, buf, buf_size)        -> 0/err   */
#define	SYS_MSG_RECV_TIMED	7	/* (name, buf, buf_size, ms)    -> 0/err   */
#define	SYS_MSG_RPC		8	/* (req, replybuf, repsize, ms) -> 0/err   */

#define	SYS_E_NOSYS	(-1)
#define	SYS_E_FAULT	(-2)
#define	SYS_E_INVAL	(-3)

/*
 * On-stack frame the entry asm hands to syscall_dispatch.  Field order
 * matches the push sequence in syscall_entry.S; do not reorder
 * without updating the asm.
 */
struct syscall_frame {
	uint64_t	sf_arg0;
	uint64_t	sf_arg1;
	uint64_t	sf_arg2;
	uint64_t	sf_arg3;
	uint64_t	sf_arg4;
	uint64_t	sf_arg5;
	uint64_t	sf_nr;
	uint64_t	sf_user_rflags;
	uint64_t	sf_user_rip;
	uint64_t	sf_user_rsp;
};

/* Install MSRs (EFER.SCE, STAR, LSTAR, FMASK).  Call once per CPU. */
void	syscall_init(void);

/* C dispatcher invoked from syscall_entry. */
long	syscall_dispatch(struct syscall_frame *);

/*
 * Per-thread bookkeeping the scheduler keeps in sync with the entry
 * stub: each time we are about to run a ring-3 thread, sched stores
 * that thread's kernel-stack top into `syscall_kernel_rsp` so the
 * stub knows where to switch on the next syscall.  Also stamped into
 * the TSS (for IRQ/exception ring-transition).
 */
extern uint64_t	syscall_kernel_rsp;

#endif /* !_SYS_SYSCALL_H_ */
