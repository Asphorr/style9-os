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

#include "port.h"	/* mach_port_name_t, struct mach_msg_header */

/*
 * Kernel syscall ABI.
 *
 * Ring-3 callers issue `syscall` with the number in %rax and up to six
 * arguments in %rdi, %rsi, %rdx, %r10, %r8, %r9.  The result comes back
 * in %rax; negative values are SYS_E_* codes (some calls return MACH_E_*
 * instead, as noted).  arch/amd64/syscall_entry.S builds a struct
 * syscall_frame and calls syscall_dispatch().  Darwin-personality tasks
 * use Apple's numbering instead (darwin.h).
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
#define	SYS_SPAWN		9	/* (const char *name)           -> task_id */
#define	SYS_TASK_ALIVE		10	/* (uint64_t task_id)           -> 0/1     */
#define	SYS_VM_ALLOCATE		11	/* (size_t bytes, uint32_t prot) -> VA    */
#define	SYS_VM_DEALLOCATE	12	/* (uint64_t va, size_t bytes)  -> 0/err  */
#define	SYS_PORT_MOD_REFS	13	/* (name, right)                -> 0/err  */
#define	SYS_PORT_SET_ALLOC	14	/* ()                           -> name   */
#define	SYS_PORT_SET_INSERT	15	/* (set_name, port_name)        -> 0/err  */
#define	SYS_PORT_SET_REMOVE	16	/* (set_name, port_name)        -> 0/err  */
#define	SYS_PORT_REQUEST_NOTIFICATION 17 /* (name, type, notify, msgid) -> 0/err */
#define	SYS_SPAWN_WITH_PORT	18	/* (const char *name, mach_port_name_t) -> task_id */
#define	SYS_TASK_SET_EXC_PORT	19	/* (mach_port_name_t notify) -> 0/err */
#define	SYS_PORT_SET_EXTRACT	20	/* (port_name) -> set_name or 0       */
#define	SYS_TASK_SET_EXC_PORTS	21	/* (mask, notify) -> 0/err            */
#define	SYS_THREAD_SET_EXC_PORTS 22	/* (mask, notify) -> 0/err            */
#define	SYS_TASK_GET_PORT_SNAPSHOT 23	/* (task_id, buf, max_entries) -> count */
#define	SYS_TASK_GET_VM_REGIONS	24	/* (task_id, buf, max_entries) -> count   */
#define	SYS_TASK_KILL		25	/* (mach_port_name_t task_port) -> 0/err  */
#define	SYS_SPAWN_RETURNS_TASKPORT 26	/* (const char *name, mach_port_name_t *out) -> task_id */
#define	SYS_SPAWN_ARGS		27	/* (name, char *const argv[], argc, mach_port_name_t *out) -> task_id */
#define	SYS_CONS_FEED		28	/* (const char *buf, size_t len) -> bytes fed */
#define	SYS_MSG_SEND_TIMED	29	/* (struct mach_msg_header *, ms) -> 0/err */

#define	SYS_E_NOSYS	(-1)
#define	SYS_E_FAULT	(-2)
#define	SYS_E_INVAL	(-3)
#define	SYS_E_NOMEM	(-4)

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

/* Boot-time setup: syscall_init_cpu on the boot CPU, plus a banner. */
void	syscall_init(void);

/*
 * Install EFER.SCE, STAR, LSTAR and FMASK on the calling CPU.  All four are
 * per-CPU, so every CPU must run this; one that did not takes #UD on its
 * first SYSCALL.
 */
void	syscall_init_cpu(void);

/* C dispatcher invoked from syscall_entry. */
long	syscall_dispatch(struct syscall_frame *);

/*
 * Console write: copy up to 4 KiB from user `buf` to the tty.  Returns
 * bytes written, or SYS_E_FAULT when `buf` is outside the user-VA window.
 * Backs SYS_PRINT and the Darwin console write(2).
 */
long	syscall_console_write(const char *buf, size_t len);

/*
 * User-memory copies, each range-checked against the user-VA window and
 * done under an SMAP bracket.
 *
 * syscall_copyin_str: a NUL-terminated string into `kbuf` (capacity
 * `kbuf_size`).  Returns its length without the NUL, SYS_E_FAULT if the
 * pointer leaves the window, or SYS_E_INVAL if no NUL fits.
 */
long	syscall_copyin_str(const char *uptr, char *kbuf, size_t kbuf_size);

/*
 * syscall_copyout / syscall_copyin: `n` bytes kernel to user / user to
 * kernel.  Return 0, or SYS_E_FAULT if the user span leaves the window or
 * wraps.
 */
long	syscall_copyout(void *uptr, const void *kbuf, size_t n);
long	syscall_copyin(void *kbuf, const void *uptr, size_t n);

/*
 * syscall_copyin_argv: a NULL-terminated user argv (execve shape) into one
 * kernel-owned flat block (argc+1 char * slots, then the packed strings;
 * the sys_spawn_args layout), within SPAWN_ARGV_MAX / SPAWN_ARG_BYTES_MAX.
 * On success *blockp owns the block (kfree it) and *argcp is the count; a
 * NULL uargv is argc 0 with no block.  Returns 0 or a negative SYS_E_*
 * (SYS_E_INVAL past a cap).
 */
long	syscall_copyin_argv(char *const *uargv, char ***blockp, int *argcp);

/*
 * The same with explicit caps, e.g. SPAWN_ENV_MAX / SPAWN_ENV_BYTES_MAX
 * for an execve's environment.
 */
long	syscall_copyin_vec(char *const *uargv, char ***blockp, int *argcp,
	    size_t max_ptrs, size_t max_bytes);

/*
 * Mach message send/recv core: range check, then the matching mach_msg_*
 * call.  Back SYS_MSG_SEND[_TIMED] / SYS_MSG_RECV[_TIMED] and the Darwin
 * mach_msg trap.  Return MACH_MSG_OK (0), a positive MACH_E_*, or
 * SYS_E_FAULT for a bad user pointer.
 */
long	syscall_msg_send(const struct mach_msg_header *umsg);
long	syscall_msg_send_timed(const struct mach_msg_header *umsg,
	    uint64_t timeout_ms);
long	syscall_msg_recv(mach_port_name_t name, struct mach_msg_header *ubuf,
	    size_t ubuf_size);
long	syscall_msg_recv_timed(mach_port_name_t name,
	    struct mach_msg_header *ubuf, size_t ubuf_size, uint64_t timeout_ms);

/*
 * VM allocate/deallocate core of SYS_VM_ALLOCATE / SYS_VM_DEALLOCATE,
 * shared with the task-self port's TASK_OP_VM_* (kern/task.c).  `t` may
 * be any task.  syscall_vm_allocate writes the VA of a zeroed range to
 * *va_out.  Both return 0 or a negative SYS_E_*.
 */
long	syscall_vm_allocate(struct task *t, uint64_t size, uint32_t prot,
	    uint64_t *va_out);
long	syscall_vm_deallocate(struct task *t, uint64_t va, uint64_t size);

/*
 * The entry stub's kernel stack is per-CPU: on every switch the scheduler
 * stores the incoming thread's kstack top with cpu_set_kernel_rsp
 * (machine/cpu.h), found at %gs:CPU_KERNEL_RSP, and in the CPU's TSS for
 * interrupts from ring 3.
 */

#endif /* !_SYS_SYSCALL_H_ */
