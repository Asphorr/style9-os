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
#include "macho.h"
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
static long	darwin_style9(struct syscall_frame *f, uint32_t num);
static long	darwin_s9_map_image(struct syscall_frame *f);
static bool	darwin_streq(const char *a, const char *b);

/*
 * The clean-room libSystem.B.dylib (user/libsystem.c), embedded as a Mach-O
 * blob.  The dyld backchannel maps it by path on demand -- it is the only
 * dependency the S4 programs name.  objcopy derives these symbols from the
 * input file name "libSystem.B.dylib" (every non-alphanumeric byte -> '_').
 * As Tier-1 grows, add a row to darwin_dylibs[] and embed the matching dylib;
 * dyld + this service already resolve an arbitrary canonical path against the
 * table, so no linker rewrite is needed -- only more dylibs and more symbols.
 */
extern uint8_t	_binary_libSystem_B_dylib_start[];
extern uint8_t	_binary_libSystem_B_dylib_end[];

#define	DARWIN_DYLIB_PATH_MAX	256

struct darwin_dylib {
	const char	*dy_path;
	const uint8_t	*dy_start;
	const uint8_t	*dy_end;
};

static const struct darwin_dylib	darwin_dylibs[] = {
	{ "/usr/lib/libSystem.B.dylib",
	    _binary_libSystem_B_dylib_start, _binary_libSystem_B_dylib_end },
};

#define	DARWIN_NDYLIBS	(sizeof(darwin_dylibs) / sizeof(darwin_dylibs[0]))

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
	case DARWIN_SYSCALL_CLASS_STYLE9:
		return (darwin_style9(f, num));
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

/*
 * style9-private call gate (class DARWIN_SYSCALL_CLASS_STYLE9), reached only
 * from our own dyld -- never from a genuine Apple binary.  See darwin.h.
 */
static long
darwin_style9(struct syscall_frame *f, uint32_t num)
{

	switch (num) {
	case DARWIN_S9_dyld_map_image:
		return (darwin_s9_map_image(f));
	default:
		kprintf("darwin: unimplemented style9 call %u\n",
		    (unsigned)num);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/*
 * map_image(const char *path): map the embedded dylib registered under `path`
 * into the calling task at its next dylib base, and return that base in %rax.
 * dyld reads the dependency name out of the main image's LC_LOAD_DYLIB and
 * hands it here; the kernel owns the actual mapping (it holds the blob + the
 * VM machinery), which keeps the user/kernel SMAP boundary clean.  Carry set
 * with 0 in %rax on any failure (unknown path, fault, OOM) so dyld can branch.
 */
static long
darwin_s9_map_image(struct syscall_frame *f)
{
	char		path[DARWIN_DYLIB_PATH_MAX];
	struct task	*t;
	uint64_t	bias;
	uint64_t	span;
	size_t		i;
	long		n;
	int		rv;

	t = current_thread->th_task;

	n = syscall_copyin_str((const char *)f->sf_arg0, path, sizeof(path));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));

	for (i = 0; i < DARWIN_NDYLIBS; i++)
		if (darwin_streq(path, darwin_dylibs[i].dy_path))
			break;
	if (i == DARWIN_NDYLIBS) {
		kprintf("darwin: s9 map_image '%s' -> not registered\n", path);
		return (darwin_err(f, DARWIN_ENOENT));
	}

	if (t->t_darwin_dylib_next == 0)
		t->t_darwin_dylib_next = DARWIN_DYLIB_BASE;
	bias = t->t_darwin_dylib_next;

	rv = macho_map_dylib(t, darwin_dylibs[i].dy_start,
	    (size_t)(darwin_dylibs[i].dy_end - darwin_dylibs[i].dy_start),
	    bias, &span);
	if (rv != MACHO_E_OK) {
		kprintf("darwin: s9 map_image '%s' map rv=%d\n", path, rv);
		return (darwin_err(f, DARWIN_ENOMEM));
	}
	t->t_darwin_dylib_next = bias + span;

	kprintf("darwin: s9 map_image '%s' -> base=0x%llx span=0x%llx\n",
	    path, (unsigned long long)bias, (unsigned long long)span);
	return (darwin_ok(f, (long)bias));
}

/* Tiny NUL-terminated string compare for the dylib registry lookup. */
static bool
darwin_streq(const char *a, const char *b)
{
	size_t	i;

	for (i = 0; ; i++) {
		if (a[i] != b[i])
			return (false);
		if (a[i] == '\0')
			return (true);
	}
}
