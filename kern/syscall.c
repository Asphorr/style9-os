/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "port.h"
#include "progreg.h"
#include "sched.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "tty.h"

#define	MSR_EFER	0xC0000080u
#define	MSR_STAR	0xC0000081u
#define	MSR_LSTAR	0xC0000082u
#define	MSR_FMASK	0xC0000084u

#define	EFER_SCE	(1u << 0)

#define	RFLAGS_IF	(1u << 9)
#define	RFLAGS_DF	(1u << 10)

extern void	syscall_entry(void);

static long	sys_print(const char *buf, size_t len);
static long	sys_exit(int code) __attribute__((noreturn));
static long	sys_yield(void);
static long	sys_port_alloc(uint8_t rights);
static long	sys_port_dealloc(mach_port_name_t name);
static long	sys_msg_send(const struct mach_msg_header *umsg);
static long	sys_msg_recv(mach_port_name_t name,
		    struct mach_msg_header *ubuf, size_t ubuf_size);
static long	sys_msg_recv_timed(mach_port_name_t name,
		    struct mach_msg_header *ubuf, size_t ubuf_size,
		    uint64_t timeout_ms);
static long	sys_msg_rpc(struct mach_msg_header *ureq,
		    struct mach_msg_header *ureply, size_t ureply_size,
		    uint64_t timeout_ms);
static long	sys_spawn(const char *uname);

static bool	user_range_ok(uint64_t addr, size_t len);

static inline uint64_t
rdmsr(uint32_t msr)
{
	uint32_t	lo;
	uint32_t	hi;

	__asm__ __volatile__("rdmsr"
	    : "=a"(lo), "=d"(hi)
	    : "c"(msr));
	return (((uint64_t)hi << 32) | lo);
}

static inline void
wrmsr(uint32_t msr, uint64_t val)
{
	uint32_t	lo;
	uint32_t	hi;

	lo = (uint32_t)(val & 0xFFFFFFFFu);
	hi = (uint32_t)(val >> 32);
	__asm__ __volatile__("wrmsr"
	    :
	    : "c"(msr), "a"(lo), "d"(hi));
}

/*
 * Enable SYSCALL/SYSRET and wire it to our entry stub.
 *
 * STAR encodes:
 *	[47:32]	kernel selector base -- CPU loads CS=that+0, SS=+8 on
 *		SYSCALL.  We set 0x08 so CS=0x08, SS=0x10.
 *	[63:48]	user selector base -- CPU loads CS=that+16, SS=+8 on
 *		SYSRETQ (64-bit).  We set 0x18 so CS=0x28|3, SS=0x20|3.
 *
 * LSTAR is the 64-bit entry RIP.  FMASK clears IF + DF on entry so
 * the kernel side starts with interrupts disabled and a known
 * direction.
 */
void
syscall_init(void)
{
	uint64_t	star;

	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

	star  = ((uint64_t)0x08u) << 32;
	star |= ((uint64_t)0x18u) << 48;
	wrmsr(MSR_STAR, star);

	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);
	wrmsr(MSR_FMASK, RFLAGS_IF | RFLAGS_DF);

	kprintf("syscall: enabled, entry=%p\n",
	    (void *)(uintptr_t)&syscall_entry);
}

long
syscall_dispatch(struct syscall_frame *f)
{

	switch (f->sf_nr) {
	case SYS_PRINT:
		return (sys_print((const char *)f->sf_arg0,
		    (size_t)f->sf_arg1));
	case SYS_EXIT:
		sys_exit((int)f->sf_arg0);
		/* NOTREACHED */
	case SYS_YIELD:
		return (sys_yield());
	case SYS_PORT_ALLOC:
		return (sys_port_alloc((uint8_t)f->sf_arg0));
	case SYS_PORT_DEALLOC:
		return (sys_port_dealloc((mach_port_name_t)f->sf_arg0));
	case SYS_MSG_SEND:
		return (sys_msg_send(
		    (const struct mach_msg_header *)f->sf_arg0));
	case SYS_MSG_RECV:
		return (sys_msg_recv((mach_port_name_t)f->sf_arg0,
		    (struct mach_msg_header *)f->sf_arg1,
		    (size_t)f->sf_arg2));
	case SYS_MSG_RECV_TIMED:
		return (sys_msg_recv_timed((mach_port_name_t)f->sf_arg0,
		    (struct mach_msg_header *)f->sf_arg1,
		    (size_t)f->sf_arg2,
		    (uint64_t)f->sf_arg3));
	case SYS_MSG_RPC:
		return (sys_msg_rpc((struct mach_msg_header *)f->sf_arg0,
		    (struct mach_msg_header *)f->sf_arg1,
		    (size_t)f->sf_arg2,
		    (uint64_t)f->sf_arg3));
	case SYS_SPAWN:
		return (sys_spawn((const char *)f->sf_arg0));
	default:
		return (SYS_E_NOSYS);
	}
}

/*
 * sys_print: write `len` bytes from the user buffer to the kernel
 * console.  No copy-in yet -- the kernel reads the user VA directly
 * (shared PML4, US=1 on the leaf).  When SMAP lands the touch will
 * be bracketed by stac/clac.
 */
static long
sys_print(const char *buf, size_t len)
{
	size_t	i;

	if (len > 4096u)
		len = 4096u;

	for (i = 0; i < len; i++)
		tty_putc(buf[i]);

	return ((long)len);
}

static long
sys_exit(int code)
{

	kprintf("[user thread exited, code=%d]\n", code);
	thread_exit();
	/* NOTREACHED */
}

static long
sys_yield(void)
{

	thread_yield();
	return (0);
}

/*
 * User pointer range check.
 *
 * Every task gets its own pmap; ring-3 leaves live at [0x40000000,
 * 0x80000000) by convention.  This check rejects pointers outside that
 * window (in particular anything aiming back into kernel-VA below the
 * 1 GiB identity map).  When SMAP comes online we'll also bracket the
 * deref with stac/clac so a missed range check fails closed rather
 * than just being a policy violation.
 */
#define	USER_VA_LO	0x40000000ULL
#define	USER_VA_HI	0x80000000ULL

static bool
user_range_ok(uint64_t addr, size_t len)
{

	if (len == 0)
		return (true);
	if (addr < USER_VA_LO || addr >= USER_VA_HI)
		return (false);
	if (addr + len < addr)
		return (false);
	if (addr + len > USER_VA_HI)
		return (false);
	return (true);
}

static long
sys_port_alloc(uint8_t rights)
{
	mach_port_name_t	n;

	n = port_allocate(current_thread->th_task->t_port_space, rights);
	if (n == MACH_PORT_NULL)
		return (SYS_E_INVAL);
	return ((long)n);
}

static long
sys_port_dealloc(mach_port_name_t name)
{

	return ((long)port_deallocate(current_thread->th_task->t_port_space,
	    name));
}

/*
 * sys_msg_send / sys_msg_recv.
 *
 * The user supplies a pointer to a Mach header.  We honour the user's
 * msgh_size (header + body bytes) and read the bytes directly from the
 * caller's address space: each ring-3 task runs under its own pmap, so
 * `umsg` is a valid VA only while this thread is current (which it is
 * for the lifetime of the syscall).  No kmalloc round-trip yet -- the
 * mach_msg_send path makes its own copy into the queued message.
 */
static long
sys_msg_send(const struct mach_msg_header *umsg)
{

	if (!user_range_ok((uint64_t)(uintptr_t)umsg,
	    sizeof(struct mach_msg_header)))
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)(uintptr_t)umsg, umsg->msgh_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_send(current_thread->th_task->t_port_space,
	    umsg));
}

static long
sys_msg_recv(mach_port_name_t name, struct mach_msg_header *ubuf,
    size_t ubuf_size)
{

	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, ubuf_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_recv_block(
	    current_thread->th_task->t_port_space,
	    name, ubuf, ubuf_size));
}

static long
sys_msg_recv_timed(mach_port_name_t name, struct mach_msg_header *ubuf,
    size_t ubuf_size, uint64_t timeout_ms)
{

	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, ubuf_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_recv_timed(
	    current_thread->th_task->t_port_space,
	    name, ubuf, ubuf_size, timeout_ms));
}

/*
 * SYS_MSG_RPC.  The user supplies a fully-populated request header and
 * a reply buffer; the kernel allocates the reply port internally,
 * splices it into req->msgh_local, sends, waits with timeout, and
 * tears the reply port down.  msgh_local in the user's request is
 * overwritten in place -- this is the standard Mach RPC convention.
 */
static long
sys_msg_rpc(struct mach_msg_header *ureq, struct mach_msg_header *ureply,
    size_t ureply_size, uint64_t timeout_ms)
{

	if (!user_range_ok((uint64_t)(uintptr_t)ureq,
	    sizeof(struct mach_msg_header)))
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)(uintptr_t)ureq, ureq->msgh_size))
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)(uintptr_t)ureply, ureply_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_rpc(current_thread->th_task->t_port_space, ureq,
	    ureply, ureply_size, timeout_ms));
}

/*
 * sys_spawn: launch the named program in a fresh task.  Copies the
 * caller's name string into a small kernel-side buffer (bounded by
 * PROGREG_NAME_MAX), validates each byte lies inside the user-VA
 * window the kernel can dereference, then hands the lookup to
 * progreg_spawn.  Returns the new task's id on success or a negative
 * SYS_E_* code.
 *
 * No copy-in via copyin() yet -- we still rely on per-task PML4 being
 * loaded for the calling thread, then read the byte through its U=1
 * leaves directly.  Once SMAP/copyin land this becomes the textbook
 * copyin_str.
 */
static long
sys_spawn(const char *uname)
{
	char	kname[PROGREG_NAME_MAX];
	size_t	i;
	long	uaddr;

	if (uname == NULL)
		return (SYS_E_FAULT);

	uaddr = (long)(uintptr_t)uname;
	if (!user_range_ok((uint64_t)uaddr, 1))
		return (SYS_E_FAULT);

	for (i = 0; i < PROGREG_NAME_MAX; i++) {
		if (!user_range_ok((uint64_t)(uaddr + (long)i), 1))
			return (SYS_E_FAULT);
		kname[i] = uname[i];
		if (uname[i] == '\0')
			break;
	}
	if (i == PROGREG_NAME_MAX)
		return (SYS_E_INVAL);

	return (progreg_spawn(kname));
}
