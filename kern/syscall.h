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

/* Install MSRs (EFER.SCE, STAR, LSTAR, FMASK).  Call once per CPU. */
void	syscall_init(void);

/* C dispatcher invoked from syscall_entry. */
long	syscall_dispatch(struct syscall_frame *);

/*
 * Shared console write: copy `len` bytes from user buffer `buf` to the tty
 * under an SMAP bracket.  Returns bytes written, or SYS_E_FAULT when `buf`
 * is outside the user-VA window.  Backs SYS_PRINT and the Darwin
 * personality's write(2) (kern/darwin.c).
 */
long	syscall_console_write(const char *buf, size_t len);

/*
 * Copy a NUL-terminated string from user pointer `uptr` into `kbuf` (capacity
 * `kbuf_size`) under the same user-VA range check + SMAP bracket as the rest of
 * the syscall surface.  Returns the string length excluding the NUL on success,
 * SYS_E_FAULT if the pointer leaves the user window, or SYS_E_INVAL if no NUL
 * appears within `kbuf_size`.  Shared by the native spawn path and the Darwin
 * personality's dyld backchannel (kern/darwin.c).
 */
long	syscall_copyin_str(const char *uptr, char *kbuf, size_t kbuf_size);

/*
 * Copy `n` bytes from kernel `kbuf` to user `uptr`, range-checking the whole
 * destination span under one SMAP bracket.  Returns 0 on success or
 * SYS_E_FAULT if the buffer leaves the user window (or its length wraps).
 * Backs the Darwin personality's read(2), which delivers file bytes
 * (kern/darwin.c).
 */
long	syscall_copyout(void *uptr, const void *kbuf, size_t n);

/*
 * Copy `n` bytes from user `uptr` into kernel `kbuf` -- the mirror of
 * syscall_copyout, same range-check + SMAP discipline.  Returns 0 or
 * SYS_E_FAULT.  Backs the Darwin pipe write path (kern/darwin.c).
 */
long	syscall_copyin(void *kbuf, const void *uptr, size_t n);

/*
 * Copy a NUL-terminated user argv (execve shape) into one kernel-owned
 * flat block (argc+1 leading char * slots, packed strings trailing --
 * the sys_spawn_args layout).  *blockp owns the block on success (kfree
 * it), *argcp the count; NULL uargv is argc 0 with no block.  Returns 0
 * or a negative SYS_E_* on fault / cap overflow.  Backs the Darwin
 * execve(2) path (kern/darwin.c).
 */
long	syscall_copyin_argv(char *const *uargv, char ***blockp, int *argcp);

/*
 * Mach message send/recv core: user-range-check + SMAP bracket + the
 * matching mach_msg_* call.  Back SYS_MSG_SEND / SYS_MSG_RECV[_TIMED] and
 * the Darwin personality's mach_msg trap (kern/darwin.c).  Return MACH_MSG_OK
 * (0) / a positive MACH_E_*, or SYS_E_FAULT for a bad user pointer.
 */
long	syscall_msg_send(const struct mach_msg_header *umsg);
long	syscall_msg_recv(mach_port_name_t name, struct mach_msg_header *ubuf,
	    size_t ubuf_size);
long	syscall_msg_recv_timed(mach_port_name_t name,
	    struct mach_msg_header *ubuf, size_t ubuf_size, uint64_t timeout_ms);

/*
 * VM allocate/deallocate core, factored from SYS_VM_ALLOCATE /
 * SYS_VM_DEALLOCATE so the task-self port's TASK_OP_VM_* dispatch
 * (kern/task.c) reuses the exact find-space -> pmap_enter -> vm_map_enter
 * path (and its rollback) against an explicit target task.  `t` may be any
 * task, not just the caller.  syscall_vm_allocate writes the chosen VA
 * through *va_out; both return 0 on success or a negative SYS_E_*.
 */
long	syscall_vm_allocate(struct task *t, uint64_t size, uint32_t prot,
	    uint64_t *va_out);
long	syscall_vm_deallocate(struct task *t, uint64_t va, uint64_t size);

/*
 * The stack the entry stub switches to is per-CPU state, not a global:
 * each time a ring-3 thread is about to run, the scheduler stores that
 * thread's kernel-stack top with cpu_set_kernel_rsp (machine/cpu.h) so the
 * stub finds it at %gs:CPU_KERNEL_RSP on the next syscall.  Also stamped
 * into that CPU's TSS, for the IRQ/exception ring transition.
 */

#endif /* !_SYS_SYSCALL_H_ */
