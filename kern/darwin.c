/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "darwin.h"
#include "kprintf.h"
#include "port.h"
#include "sched.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"

/*
 * Darwin (XNU) syscall personality dispatcher -- S2 of the Mach-O ladder.
 * syscall_dispatch routes a TASK_PERSONALITY_DARWIN task here; we decode the
 * Apple class/number out of %rax and translate each onto the style9 primitive
 * that already implements it.  The interesting part is not the translation
 * but the ABI boundary: how the caller passes arguments (already aligned with
 * style9 -- rdi/rsi/rdx/r10/r8/r9) and how it reads results.
 *
 * Return convention, the crux of S2:
 *	- Unix/BSD (class 2): libSystem reads the carry flag.  CF clear means
 *	  %rax is the result; CF set means %rax is a positive errno.  style9
 *	  has no errno (it speaks MACH_E_* and ELF_E_* codes), so the error path
 *	  maps an internal failure onto a Darwin errno and sets carry.
 *	- Mach (class 1): the trap returns a port name or kern_return_t in %rax
 *	  with no carry convention; we clear carry and return the value.
 *
 * The carry flag lives in the saved user RFLAGS the entry stub sysrets with
 * (syscall_entry.S restores %r11 from sf_user_rflags), so every return funnels
 * through darwin_ok()/darwin_err() to set it deterministically -- a Darwin
 * syscall never inherits stale carry from the thread's last user instruction.
 */

#define	RFLAGS_CF	(1u << 0)

static long	darwin_unix(struct syscall_frame *f, uint32_t nr);
static long	darwin_mach(struct syscall_frame *f, uint32_t trap);
static long	darwin_mach_msg(struct syscall_frame *f);
static long	darwin_mach_msg_err(long rv, bool sending);

/* Success: carry clear, `val` in %rax. */
static long
darwin_ok(struct syscall_frame *f, long val)
{

	f->sf_user_rflags &= ~(uint64_t)RFLAGS_CF;
	return (val);
}

/* Error: carry set, positive `err` (a Darwin errno) in %rax. */
static long
darwin_err(struct syscall_frame *f, int err)
{

	f->sf_user_rflags |= RFLAGS_CF;
	return ((long)err);
}

long
darwin_dispatch(struct syscall_frame *f)
{
	uint32_t	class;
	uint32_t	num;

	class = (uint32_t)((f->sf_nr >> DARWIN_SYSCALL_CLASS_SHIFT) &
	    DARWIN_SYSCALL_CLASS_MASK);
	num = (uint32_t)(f->sf_nr & DARWIN_SYSCALL_NUMBER_MASK);

	switch (class) {
	case DARWIN_SYSCALL_CLASS_UNIX:
		return (darwin_unix(f, num));
	case DARWIN_SYSCALL_CLASS_MACH:
		return (darwin_mach(f, num));
	default:
		kprintf("darwin: unhandled syscall class %u (nr=0x%llx)\n",
		    (unsigned)class, (unsigned long long)f->sf_nr);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/*
 * Class 2: the BSD/Unix call gate.  Arguments are already in sf_arg0..5 in
 * the right order, so each case just hands them to the style9 primitive.
 */
static long
darwin_unix(struct syscall_frame *f, uint32_t nr)
{

	switch (nr) {
	case DARWIN_SYS_write: {
		long	rv;
		int	fd;

		fd = (int)f->sf_arg0;
		if (fd != 1 && fd != 2)		/* stdout/stderr -> console */
			return (darwin_err(f, DARWIN_EBADF));
		rv = syscall_console_write((const char *)f->sf_arg1,
		    (size_t)f->sf_arg2);
		if (rv < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		kprintf("darwin: UNIX write(fd=%d, %ld bytes) -> %ld\n",
		    fd, (long)f->sf_arg2, rv);
		return (darwin_ok(f, rv));
	}
	case DARWIN_SYS_getpid: {
		uint64_t	id;

		id = current_thread->th_task->t_id;
		kprintf("darwin: UNIX getpid() -> %llu\n",
		    (unsigned long long)id);
		return (darwin_ok(f, (long)id));
	}
	case DARWIN_SYS_exit:
		kprintf("darwin: UNIX exit(%d)\n", (int)f->sf_arg0);
		thread_exit();
		/* NOTREACHED */
		return (darwin_ok(f, 0));
	default:
		kprintf("darwin: unimplemented BSD syscall %u\n",
		    (unsigned)nr);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/*
 * Class 1: the Mach trap gate.  These return a value directly in %rax with no
 * carry convention; we still clear carry so a Mach trap never leaves it set
 * from a prior BSD error on the same thread.
 */
static long
darwin_mach(struct syscall_frame *f, uint32_t trap)
{

	switch (trap) {
	case DARWIN_MACH_task_self_trap:
		kprintf("darwin: MACH task_self_trap() -> name=%u\n",
		    (unsigned)MACH_PORT_TASK_SELF);
		return (darwin_ok(f, (long)MACH_PORT_TASK_SELF));
	case DARWIN_MACH_mach_reply_port: {
		mach_port_name_t	n;

		n = port_allocate(current_thread->th_task->t_port_space,
		    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
		kprintf("darwin: MACH mach_reply_port() -> name=%u\n",
		    (unsigned)n);
		return (darwin_ok(f, (long)n));	/* MACH_PORT_NULL on failure */
	}
	case DARWIN_MACH_mach_msg_trap:
		return (darwin_mach_msg(f));
	default:
		/* Port-returning traps signal failure with a null name. */
		kprintf("darwin: unhandled mach trap %u\n", (unsigned)trap);
		return (darwin_ok(f, (long)MACH_PORT_NULL));
	}
}

/*
 * Map a style9 mach_msg result -- MACH_MSG_OK / a positive MACH_E_*, or the
 * negative SYS_E_FAULT a user-range check returns -- onto the Darwin
 * mach_msg_return_t the caller reads.  `sending` selects the SEND_* vs RCV_*
 * code family.
 */
static long
darwin_mach_msg_err(long rv, bool sending)
{

	switch (rv) {
	case MACH_E_NAME:
	case MACH_E_RIGHT:
	case MACH_E_DEAD:
		return (sending ? DARWIN_MACH_SEND_INVALID_DEST :
		    DARWIN_MACH_RCV_INVALID_NAME);
	case MACH_E_TOOSMALL:
		return (DARWIN_MACH_RCV_TOO_LARGE);
	case MACH_E_TIMEOUT:
		return (sending ? DARWIN_MACH_SEND_TIMED_OUT :
		    DARWIN_MACH_RCV_TIMED_OUT);
	default:
		return (sending ? DARWIN_MACH_SEND_INVALID_DATA :
		    DARWIN_MACH_RCV_INVALID_DATA);
	}
}

/*
 * mach_msg_trap (Mach class 1, trap 31): the classic combined send/receive.
 * Darwin's mach_msg(3) packs its arguments into the syscall registers in the
 * usual order; we read msg/option/rcv_size/rcv_name/timeout.  The 7th arg
 * (notify) is unsupported -- a classic mach_msg passes MACH_PORT_NULL there.
 * send_size (arg2) is implicit: the kernel honours msg->msgh_size.  A combined
 * SEND|RCV sends then receives into the same buffer, exactly as mach_msg(3)
 * does; the shared syscall_msg_* helpers do the user-range check + SMAP
 * bracket + drive the kernel's existing message path.  Returns a
 * mach_msg_return_t in %rax with carry clear (Mach convention).
 */
static long
darwin_mach_msg(struct syscall_frame *f)
{
	struct mach_msg_header	*msg;
	uint64_t		 timeout;
	uint32_t		 option;
	uint32_t		 rcv_size;
	mach_port_name_t	 rcv_name;
	long			 rv;

	msg      = (struct mach_msg_header *)f->sf_arg0;
	option   = (uint32_t)f->sf_arg1;
	rcv_size = (uint32_t)f->sf_arg3;
	rcv_name = (mach_port_name_t)f->sf_arg4;
	timeout  = f->sf_arg5;

	if (option & DARWIN_MACH_SEND_MSG) {
		rv = syscall_msg_send(msg);
		if (rv != MACH_MSG_OK) {
			kprintf("darwin: MACH mach_msg send -> rv=%ld\n", rv);
			return (darwin_ok(f, darwin_mach_msg_err(rv, true)));
		}
	}
	if (option & DARWIN_MACH_RCV_MSG) {
		if (option & DARWIN_MACH_RCV_TIMEOUT)
			rv = syscall_msg_recv_timed(rcv_name, msg, rcv_size,
			    timeout);
		else
			rv = syscall_msg_recv(rcv_name, msg, rcv_size);
		if (rv != MACH_MSG_OK) {
			kprintf("darwin: MACH mach_msg recv -> rv=%ld\n", rv);
			return (darwin_ok(f, darwin_mach_msg_err(rv, false)));
		}
	}

	kprintf("darwin: MACH mach_msg(option=0x%x) -> KERN_SUCCESS\n",
	    (unsigned)option);
	return (darwin_ok(f, DARWIN_MACH_MSG_SUCCESS));
}
