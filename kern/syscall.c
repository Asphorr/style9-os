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
#include "kmem.h"
#include "kprintf.h"
#include "msr.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "port_internal.h"
#include "progreg.h"
#include "sched.h"
#include "smap.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "tty.h"
#include "vm.h"

#define	RFLAGS_IF	(1u << 9)
#define	RFLAGS_DF	(1u << 10)

extern void	syscall_entry(void);

static long	sys_print(const char *buf, size_t len);
static long	sys_exit(int code) __attribute__((noreturn));
static long	sys_yield(void);
static long	sys_port_alloc(uint8_t rights);
static long	sys_port_dealloc(mach_port_name_t name);
static long	sys_msg_send(const struct mach_msg_header *umsg);
static long	sys_msg_send_timed(const struct mach_msg_header *umsg,
		    uint64_t timeout_ms);
static long	sys_msg_recv(mach_port_name_t name,
		    struct mach_msg_header *ubuf, size_t ubuf_size);
static long	sys_msg_recv_timed(mach_port_name_t name,
		    struct mach_msg_header *ubuf, size_t ubuf_size,
		    uint64_t timeout_ms);
static long	sys_msg_rpc(struct mach_msg_header *ureq,
		    struct mach_msg_header *ureply, size_t ureply_size,
		    uint64_t timeout_ms);
static long	sys_spawn(const char *uname);
static long	sys_task_alive(uint64_t task_id);
static long	sys_vm_allocate(uint64_t size, uint32_t prot);
static long	sys_vm_deallocate(uint64_t va, uint64_t size);
static long	sys_port_mod_refs(mach_port_name_t name, uint8_t right);
static long	sys_port_set_alloc(void);
static long	sys_port_set_insert(mach_port_name_t set_name,
		    mach_port_name_t port_name);
static long	sys_port_set_remove(mach_port_name_t set_name,
		    mach_port_name_t port_name);
static long	sys_port_request_notification(mach_port_name_t name,
		    uint32_t notify_type,
		    mach_port_name_t notify_port_name,
		    uint32_t notify_msgid);
static long	sys_spawn_with_port(const char *uname,
		    mach_port_name_t source_name);
static long	sys_task_set_exc_port(mach_port_name_t notify_port_name);
static long	sys_port_set_extract(mach_port_name_t port_name);
static long	sys_task_set_exc_ports(uint32_t types_mask,
		    mach_port_name_t notify_port_name);
static long	sys_thread_set_exc_ports(uint32_t types_mask,
		    mach_port_name_t notify_port_name);
static long	sys_task_get_port_snapshot(uint64_t task_id,
		    struct mach_port_snapshot_entry *ubuf,
		    size_t max_entries);
static long	sys_task_get_vm_regions(uint64_t task_id,
		    struct mach_vm_region_entry *ubuf,
		    size_t max_entries);
static long	sys_task_kill(mach_port_name_t target_port_name);
static long	sys_spawn_returns_taskport(const char *uname,
		    mach_port_name_t *out_taskport_name);
static long	sys_spawn_args(const char *uname, char *const *uargv,
		    uint64_t argc, mach_port_name_t *out_taskport_name);
static long	sys_cons_feed(const char *ubuf, size_t len);

static bool	user_range_ok(uint64_t addr, size_t len);

/*
 * Enable SYSCALL/SYSRET and wire it to our entry stub.
 *
 * STAR encodes:
 *	[47:32]	kernel selector base -- CPU loads CS=that+0, SS=+8 on
 *		SYSCALL.  We set 0x08 so CS=0x08, SS=0x10.
 *	[63:48]	user selector base -- CPU loads CS=that+16, SS=+8 on
 *		SYSRETQ (64-bit).  We set 0x18 so CS=0x28|3, SS=0x20|3.
 *
 * LSTAR is the 64-bit entry RIP.  FMASK clears IF and DF on entry.
 * All four registers are per-CPU; see syscall.h.
 */
void
syscall_init_cpu(void)
{
	uint64_t	star;

	wrmsr(MSR_EFER, rdmsr(MSR_EFER) | EFER_SCE);

	star  = ((uint64_t)0x08u) << 32;
	star |= ((uint64_t)0x18u) << 48;
	wrmsr(MSR_STAR, star);

	wrmsr(MSR_LSTAR, (uint64_t)(uintptr_t)&syscall_entry);
	wrmsr(MSR_FMASK, RFLAGS_IF | RFLAGS_DF);
}

void
syscall_init(void)
{

	syscall_init_cpu();

	kprintf("syscall: enabled, entry=%p\n",
	    (void *)(uintptr_t)&syscall_entry);
}

static long
syscall_dispatch_body(struct syscall_frame *f)
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
	case SYS_MSG_SEND_TIMED:
		return (sys_msg_send_timed(
		    (const struct mach_msg_header *)f->sf_arg0,
		    (uint64_t)f->sf_arg1));
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
	case SYS_TASK_ALIVE:
		return (sys_task_alive((uint64_t)f->sf_arg0));
	case SYS_VM_ALLOCATE:
		return (sys_vm_allocate((uint64_t)f->sf_arg0,
		    (uint32_t)f->sf_arg1));
	case SYS_VM_DEALLOCATE:
		return (sys_vm_deallocate((uint64_t)f->sf_arg0,
		    (uint64_t)f->sf_arg1));
	case SYS_PORT_MOD_REFS:
		return (sys_port_mod_refs((mach_port_name_t)f->sf_arg0,
		    (uint8_t)f->sf_arg1));
	case SYS_PORT_SET_ALLOC:
		return (sys_port_set_alloc());
	case SYS_PORT_SET_INSERT:
		return (sys_port_set_insert((mach_port_name_t)f->sf_arg0,
		    (mach_port_name_t)f->sf_arg1));
	case SYS_PORT_SET_REMOVE:
		return (sys_port_set_remove((mach_port_name_t)f->sf_arg0,
		    (mach_port_name_t)f->sf_arg1));
	case SYS_PORT_SET_EXTRACT:
		return (sys_port_set_extract((mach_port_name_t)f->sf_arg0));
	case SYS_TASK_SET_EXC_PORTS:
		return (sys_task_set_exc_ports(
		    (uint32_t)f->sf_arg0,
		    (mach_port_name_t)f->sf_arg1));
	case SYS_THREAD_SET_EXC_PORTS:
		return (sys_thread_set_exc_ports(
		    (uint32_t)f->sf_arg0,
		    (mach_port_name_t)f->sf_arg1));
	case SYS_PORT_REQUEST_NOTIFICATION:
		return (sys_port_request_notification(
		    (mach_port_name_t)f->sf_arg0,
		    (uint32_t)f->sf_arg1,
		    (mach_port_name_t)f->sf_arg2,
		    (uint32_t)f->sf_arg3));
	case SYS_SPAWN_WITH_PORT:
		return (sys_spawn_with_port(
		    (const char *)f->sf_arg0,
		    (mach_port_name_t)f->sf_arg1));
	case SYS_TASK_SET_EXC_PORT:
		return (sys_task_set_exc_port(
		    (mach_port_name_t)f->sf_arg0));
	case SYS_TASK_GET_PORT_SNAPSHOT:
		return (sys_task_get_port_snapshot(
		    (uint64_t)f->sf_arg0,
		    (struct mach_port_snapshot_entry *)f->sf_arg1,
		    (size_t)f->sf_arg2));
	case SYS_TASK_GET_VM_REGIONS:
		return (sys_task_get_vm_regions(
		    (uint64_t)f->sf_arg0,
		    (struct mach_vm_region_entry *)f->sf_arg1,
		    (size_t)f->sf_arg2));
	case SYS_TASK_KILL:
		return (sys_task_kill((mach_port_name_t)f->sf_arg0));
	case SYS_SPAWN_RETURNS_TASKPORT:
		return (sys_spawn_returns_taskport(
		    (const char *)f->sf_arg0,
		    (mach_port_name_t *)f->sf_arg1));
	case SYS_SPAWN_ARGS:
		return (sys_spawn_args(
		    (const char *)f->sf_arg0,
		    (char *const *)f->sf_arg1,
		    (uint64_t)f->sf_arg2,
		    (mach_port_name_t *)f->sf_arg3));
	case SYS_CONS_FEED:
		return (sys_cons_feed(
		    (const char *)f->sf_arg0,
		    (size_t)f->sf_arg1));
	default:
		return (SYS_E_NOSYS);
	}
}

long
syscall_dispatch(struct syscall_frame *f)
{
	long	rv;

	sched_cpu_to_sys();

	/*
	 * Async-kill detection point #1: a kill requested while this thread
	 * ran in ring 3.  Retire before dispatching; no reply would ever be
	 * seen.  kern/task.h's t_killed comment lists every detection point.
	 */
	if (current_thread->th_task != kernel_task &&
	    task_kill_pending(current_thread->th_task))
		thread_exit();
	/* NOTREACHED if killed */

	/* Darwin-personality tasks take the Apple path (kern/darwin.c). */
	if (current_thread->th_task->t_personality == TASK_PERSONALITY_DARWIN)
		rv = darwin_dispatch(f);
	else
		rv = syscall_dispatch_body(f);

	/*
	 * Every mutex taken in a syscall is released by here.  This bounds
	 * the kill deferral too: a killed holder may run on only because it
	 * reaches this boundary holding nothing (the mutex clause in
	 * kern/sched.c's kill checks).
	 */
	KASSERT(current_thread->th_mutex_depth == 0,
	    "syscall returning to ring 3 with a mutex still held");

	/*
	 * Detection point #5: a kill caused by this syscall itself (e.g.
	 * SYS_TASK_KILL on self).  Retire now rather than sysretq and run
	 * user code until the next check.
	 */
	if (current_thread->th_task != kernel_task &&
	    task_kill_pending(current_thread->th_task))
		thread_exit();
	/* NOTREACHED if killed */

	/*
	 * Deliver any signal pending on a Darwin task (SIGPIPE from this very
	 * write, say): default-terminate retires the thread here, a caught
	 * signal is delivered on-stack by reshaping `f'.  Native tasks have
	 * no Darwin signal state.
	 */
	if (current_thread->th_task != kernel_task &&
	    current_thread->th_task->t_personality == TASK_PERSONALITY_DARWIN)
		darwin_signal_deliver_syscall(f, rv);

	sched_cpu_to_user();
	return (rv);
}

/*
 * syscall_console_write: copy up to 4 KiB (a kernel-stack scratch) from the
 * user buffer under an SMAP bracket, then push it to the tty without AC=1
 * held across the tty lock.  Returns bytes written or SYS_E_FAULT.  Backs
 * SYS_PRINT and the Darwin console write(2).
 */
long
syscall_console_write(const char *buf, size_t len)
{
	char	scratch[4096];
	size_t	i;

	if (len > sizeof(scratch))
		len = sizeof(scratch);
	if (len == 0)
		return (0);
	if (!user_range_ok((uint64_t)(uintptr_t)buf, len))
		return (SYS_E_FAULT);

	smap_user_access_begin();
	for (i = 0; i < len; i++)
		scratch[i] = buf[i];
	smap_user_access_end();

	/* One batch, so the cursor moves once, not across every column. */
	tty_batch_begin();
	for (i = 0; i < len; i++)
		tty_putc(scratch[i]);
	tty_batch_end();

	return ((long)len);
}

static long
sys_print(const char *buf, size_t len)
{

	return (syscall_console_write(buf, len));
}

/*
 * sys_cons_feed: copy up to 256 bytes from the user buffer and load them as
 * the Darwin console's script (darwin_cons_feed), which a Darwin shell then
 * reads through its real read(2) path; the boot demo uses it.  Returns
 * bytes fed or SYS_E_FAULT.
 */
static long
sys_cons_feed(const char *ubuf, size_t len)
{
	char	scratch[256];
	size_t	i;

	if (len > sizeof(scratch))
		len = sizeof(scratch);
	if (len == 0)
		return (0);
	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, len))
		return (SYS_E_FAULT);

	smap_user_access_begin();
	for (i = 0; i < len; i++)
		scratch[i] = ubuf[i];
	smap_user_access_end();

	darwin_cons_feed(scratch, len);
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

	/*
	 * 1 if another thread got the CPU, 0 if none was waiting.  A poll
	 * loop counted in yields needs the difference once the awaited thread
	 * can run on another CPU (thread_yield; poll_turn in libstyle9).
	 */
	return (thread_yield() ? 1 : 0);
}

/*
 * User pointer range check: ring-3 mappings live in [0x40000000,
 * 0x80000000), so anything outside, in particular kernel VA below the
 * 1 GiB identity map, is refused.  Derefs are also bracketed with
 * smap_user_access_begin/end, so with SMAP on a missed check faults
 * rather than leaks.
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

/*
 * Copy in a NUL-terminated user string: up to kbuf_size bytes, each
 * range-checked, under one SMAP bracket.  Returns the length (excluding
 * the NUL), SYS_E_FAULT on a bad pointer, or SYS_E_INVAL when no NUL
 * appears in kbuf_size bytes.  The inline copies in sys_spawn et al.
 * predate it.
 */
long
syscall_copyin_str(const char *uptr, char *kbuf, size_t kbuf_size)
{
	uintptr_t	uaddr;
	size_t		i;

	if (uptr == NULL || kbuf == NULL || kbuf_size == 0)
		return (SYS_E_FAULT);

	uaddr = (uintptr_t)uptr;
	if (!user_range_ok((uint64_t)uaddr, 1))
		return (SYS_E_FAULT);

	smap_user_access_begin();
	for (i = 0; i < kbuf_size; i++) {
		if (!user_range_ok((uint64_t)(uaddr + i), 1)) {
			smap_user_access_end();
			return (SYS_E_FAULT);
		}
		kbuf[i] = uptr[i];
		if (uptr[i] == '\0') {
			smap_user_access_end();
			return ((long)i);
		}
	}
	smap_user_access_end();
	return (SYS_E_INVAL);	/* no NUL within kbuf_size */
}

/*
 * Copy `n` bytes from kernel `kbuf` to user `uptr`: one range check of the
 * whole span, one SMAP bracket.  Returns 0, or SYS_E_FAULT for a NULL,
 * wrapping or out-of-window destination.
 */
long
syscall_copyout(void *uptr, const void *kbuf, size_t n)
{
	const uint8_t	*src;
	uint8_t		*dst;
	uintptr_t	 uaddr;
	size_t		 i;

	if (n == 0)
		return (0);
	if (uptr == NULL)
		return (SYS_E_FAULT);
	uaddr = (uintptr_t)uptr;
	if (uaddr + n < uaddr)			/* length wrap */
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)uaddr, n))
		return (SYS_E_FAULT);

	src = (const uint8_t *)kbuf;
	dst = (uint8_t *)uptr;
	smap_user_access_begin();
	for (i = 0; i < n; i++)
		dst[i] = src[i];
	smap_user_access_end();
	return (0);
}

/*
 * The mirror of syscall_copyout: `n` bytes from user `uptr` into kernel
 * `kbuf`.  Returns 0 or SYS_E_FAULT.
 */
long
syscall_copyin(void *kbuf, const void *uptr, size_t n)
{
	const uint8_t	*src;
	uint8_t		*dst;
	uintptr_t	 uaddr;
	size_t		 i;

	if (n == 0)
		return (0);
	if (uptr == NULL)
		return (SYS_E_FAULT);
	uaddr = (uintptr_t)uptr;
	if (uaddr + n < uaddr)			/* length wrap */
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)uaddr, n))
		return (SYS_E_FAULT);

	src = (const uint8_t *)uptr;
	dst = (uint8_t *)kbuf;
	smap_user_access_begin();
	for (i = 0; i < n; i++)
		dst[i] = src[i];
	smap_user_access_end();
	return (0);
}

/*
 * Copy a NULL-terminated user vector (execve's char *argv[]) into one
 * kernel-owned flat block laid out like sys_spawn_args': argc+1 char *
 * slots (the last NULL) pointing into the packed strings that follow.  At
 * most `max_ptrs' pointers and `max_bytes' string bytes (SYS_E_INVAL
 * beyond).  On success returns 0 with the kmalloc'd block in *blockp (the
 * caller kfrees it) and the count in *argcp; a NULL uargv is argc 0 with
 * no block.  SYS_E_FAULT or SYS_E_NOMEM otherwise.
 */
long
syscall_copyin_vec(char *const *uargv, char ***blockp, int *argcp,
    size_t max_ptrs, size_t max_bytes)
{
	char		**kargv;
	char		 *block;
	char		 *strs;
	const char	 *uarg;
	size_t		  ptrs_sz;
	size_t		  used;
	size_t		  i;
	size_t		  k;
	size_t		  n;

	*blockp = NULL;
	*argcp  = 0;
	if (uargv == NULL)
		return (0);
	if (!user_range_ok((uint64_t)(uintptr_t)uargv, sizeof(char *)))
		return (SYS_E_FAULT);

	/* First pass: count entries up to the NULL terminator. */
	n = 0;
	for (;;) {
		if (n > max_ptrs)
			return (SYS_E_INVAL);
		if (!user_range_ok((uint64_t)(uintptr_t)(uargv + n),
		    sizeof(char *)))
			return (SYS_E_FAULT);
		smap_user_access_begin();
		uarg = uargv[n];
		smap_user_access_end();
		if (uarg == NULL)
			break;
		n++;
	}
	if (n == 0)
		return (0);

	ptrs_sz = (n + 1) * sizeof(char *);
	block = kmalloc(ptrs_sz + max_bytes);
	if (block == NULL)
		return (SYS_E_NOMEM);
	kargv = (char **)block;
	strs  = block + ptrs_sz;
	used  = 0;

	for (i = 0; i < n; i++) {
		smap_user_access_begin();
		uarg = uargv[i];
		smap_user_access_end();
		if (uarg == NULL ||
		    !user_range_ok((uint64_t)(uintptr_t)uarg, 1)) {
			kfree(block);
			return (SYS_E_FAULT);
		}
		kargv[i] = strs + used;
		k = 0;
		smap_user_access_begin();
		for (;;) {
			if (used >= max_bytes) {
				smap_user_access_end();
				kfree(block);
				return (SYS_E_INVAL);
			}
			if (!user_range_ok((uint64_t)(uintptr_t)(uarg + k),
			    1)) {
				smap_user_access_end();
				kfree(block);
				return (SYS_E_FAULT);
			}
			strs[used] = uarg[k];
			used++;
			if (uarg[k] == '\0')
				break;
			k++;
		}
		smap_user_access_end();
	}
	kargv[n] = NULL;

	*blockp = kargv;
	*argcp  = (int)n;
	return (0);
}

/* The argv shape: the spawn caps, which the launcher's frame is sized for. */
long
syscall_copyin_argv(char *const *uargv, char ***blockp, int *argcp)
{

	return (syscall_copyin_vec(uargv, blockp, argcp, SPAWN_ARGV_MAX,
	    SPAWN_ARG_BYTES_MAX));
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
 * Mach message send/recv.  The user passes a Mach header pointer; its
 * msgh_size is honoured and the body read straight from the caller's
 * address space (its pmap is current throughout), mach_msg_send making
 * the queued copy.  The core is in the syscall_msg_* helpers, shared with
 * the Darwin mach_msg trap; sys_msg_* are the table entry points.
 */
long
syscall_msg_send(const struct mach_msg_header *umsg)
{

	return (syscall_msg_send_timed(umsg, MACH_TIMEOUT_FOREVER));
}

long
syscall_msg_send_timed(const struct mach_msg_header *umsg,
    uint64_t timeout_ms)
{
	uint32_t	msgh_size;

	if (!user_range_ok((uint64_t)(uintptr_t)umsg,
	    sizeof(struct mach_msg_header)))
		return (SYS_E_FAULT);

	/*
	 * Read msgh_size under an SMAP bracket, then bound the whole
	 * [umsg, umsg + msgh_size) before mach_msg_send reads the body.
	 */
	smap_user_access_begin();
	msgh_size = umsg->msgh_size;
	smap_user_access_end();

	if (!user_range_ok((uint64_t)(uintptr_t)umsg, msgh_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_send_timed(
	    current_thread->th_task->t_port_space, umsg, timeout_ms));
}

static long
sys_msg_send(const struct mach_msg_header *umsg)
{

	return (syscall_msg_send(umsg));
}

static long
sys_msg_send_timed(const struct mach_msg_header *umsg, uint64_t timeout_ms)
{

	return (syscall_msg_send_timed(umsg, timeout_ms));
}

long
syscall_msg_recv(mach_port_name_t name, struct mach_msg_header *ubuf,
    size_t ubuf_size)
{

	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, ubuf_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_recv_block(
	    current_thread->th_task->t_port_space,
	    name, ubuf, ubuf_size));
}

static long
sys_msg_recv(mach_port_name_t name, struct mach_msg_header *ubuf,
    size_t ubuf_size)
{

	return (syscall_msg_recv(name, ubuf, ubuf_size));
}

long
syscall_msg_recv_timed(mach_port_name_t name, struct mach_msg_header *ubuf,
    size_t ubuf_size, uint64_t timeout_ms)
{

	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, ubuf_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_recv_timed(
	    current_thread->th_task->t_port_space,
	    name, ubuf, ubuf_size, timeout_ms));
}

static long
sys_msg_recv_timed(mach_port_name_t name, struct mach_msg_header *ubuf,
    size_t ubuf_size, uint64_t timeout_ms)
{

	return (syscall_msg_recv_timed(name, ubuf, ubuf_size, timeout_ms));
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
	uint32_t	ureq_size;

	if (!user_range_ok((uint64_t)(uintptr_t)ureq,
	    sizeof(struct mach_msg_header)))
		return (SYS_E_FAULT);

	smap_user_access_begin();
	ureq_size = ureq->msgh_size;
	smap_user_access_end();

	if (!user_range_ok((uint64_t)(uintptr_t)ureq, ureq_size))
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)(uintptr_t)ureply, ureply_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_rpc(current_thread->th_task->t_port_space, ureq,
	    ureply, ureply_size, timeout_ms));
}

/*
 * sys_spawn: launch the named program in a fresh task.  Copies the name
 * (at most PROGREG_NAME_MAX bytes, each range-checked) and hands it to
 * progreg_spawn.  Returns the new task id or a negative SYS_E_*.  The
 * inline copy is what syscall_copyin_str now does.
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

	/* The bracket lets the kernel read the user bytes with CR4.SMAP on. */
	smap_user_access_begin();
	for (i = 0; i < PROGREG_NAME_MAX; i++) {
		if (!user_range_ok((uint64_t)(uaddr + (long)i), 1)) {
			smap_user_access_end();
			return (SYS_E_FAULT);
		}
		kname[i] = uname[i];
		if (uname[i] == '\0')
			break;
	}
	smap_user_access_end();

	if (i == PROGREG_NAME_MAX)
		return (SYS_E_INVAL);

	return (progreg_spawn(kname));
}

/*
 * sys_task_alive: 1 if a task with this id is still on the live list, 0
 * if not; a polling primitive for the native shell.  Touches no user
 * memory.
 */
static long
sys_task_alive(uint64_t task_id)
{

	return (task_is_alive(task_id) ? 1 : 0);
}

/*
 * syscall_vm_allocate: the core of SYS_VM_ALLOCATE, also driven by the
 * task-self port's TASK_OP_VM_ALLOCATE (kern/task.c).  Allocates an
 * anonymous, zeroed, page-rounded range in `t` with the requested
 * VM_PROT_* plus VM_PROT_USER, and writes its VA to *va_out.  Returns 0 or
 * a negative SYS_E_*.
 *
 * `t` need not be the current task: pages are zeroed through the direct
 * map and pmap_enter targets t->t_pmap.  Any failure rolls back every page
 * mapped so far, so no frame leaks.
 */
long
syscall_vm_allocate(struct task *t, uint64_t size, uint32_t prot,
    uint64_t *va_out)
{
	uint8_t		*kva;
	uint64_t	 pa;
	uint64_t	 v;
	uint64_t	 va;
	size_t		 i;
	size_t		 j;
	size_t		 pages;
	uint32_t	 pmap_flags;

	if (t == NULL || va_out == NULL)
		return (SYS_E_INVAL);
	if (size == 0)
		return (SYS_E_INVAL);

	size = (size + 0xFFFull) & ~0xFFFull;
	if (size == 0)
		return (SYS_E_NOMEM);

	prot      &= (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC);
	pmap_flags = prot | VM_PROT_USER;
	pages      = (size_t)(size >> 12);

	if (!vm_map_find_space(t->t_map, size, &va))
		return (SYS_E_NOMEM);

	for (i = 0; i < pages; i++) {
		v  = va + (uint64_t)i * 0x1000ull;
		pa = pmm_alloc_page();
		if (pa == PA_INVALID)
			goto unwind;
		kva = (uint8_t *)pmm_kva_from_pa(pa);
		for (j = 0; j < 0x1000u; j++)
			kva[j] = 0;
		if (!pmap_enter(t->t_pmap, v, pa, pmap_flags)) {
			pmm_free_page(pa);
			goto unwind;
		}
	}

	if (!vm_map_enter(t->t_map, va, size,
	    (uint8_t)pmap_flags, VME_F_ANON))
		goto unwind;

	*va_out = va;
	return (0);

unwind:
	for (j = 0; j < i; j++) {
		v  = va + (uint64_t)j * 0x1000ull;
		pa = pmap_extract(t->t_pmap, v);
		(void)pmap_remove(t->t_pmap, v);
		if (pa != PA_INVALID)
			pmm_free_page(pa);
	}
	return (SYS_E_NOMEM);
}

/*
 * sys_vm_allocate: syscall_vm_allocate on the calling task.  Returns the
 * VA or a negative SYS_E_*.
 */
static long
sys_vm_allocate(uint64_t size, uint32_t prot)
{
	uint64_t	va;
	long		rv;

	va = 0;
	rv = syscall_vm_allocate(current_thread->th_task, size, prot, &va);
	if (rv != 0)
		return (rv);
	return ((long)va);
}

/*
 * syscall_vm_deallocate: the core of SYS_VM_DEALLOCATE.  Releases a range
 * of `t`'s map; it need not match an allocation, as vm_map_release cuts
 * entries at the edges.  A range with a hole, or covering borrowed
 * memory, is refused.  Returns 0 or a negative SYS_E_*.
 */
long
syscall_vm_deallocate(struct task *t, uint64_t va, uint64_t size)
{

	if (t == NULL)
		return (SYS_E_INVAL);
	if (size == 0)
		return (SYS_E_INVAL);
	if ((va & 0xFFFull) != 0)
		return (SYS_E_INVAL);

	size = (size + 0xFFFull) & ~0xFFFull;
	if (size == 0)
		return (SYS_E_INVAL);

	if (!vm_map_release(t->t_map, t->t_pmap, va, size))
		return (SYS_E_INVAL);
	return (0);
}

/* sys_vm_deallocate: syscall_vm_deallocate on the calling task. */
static long
sys_vm_deallocate(uint64_t va, uint64_t size)
{

	return (syscall_vm_deallocate(current_thread->th_task, va, size));
}

/*
 * sys_port_mod_refs: drop one right kind from a name in the caller's
 * space, e.g. SEND while keeping RECV, without tearing the slot down.
 * Fails with a MACH_E_*, not a SYS_E_*.
 */
static long
sys_port_mod_refs(mach_port_name_t name, uint8_t right)
{

	return ((long)port_mod_refs(current_thread->th_task->t_port_space,
	    name, right));
}

/*
 * sys_port_set_alloc: create a port set in the caller's space and return
 * its name (MACH_PORT_RIGHT_PORT_SET; not a SEND target).  One
 * mach_msg_recv on it serves any member.
 */
static long
sys_port_set_alloc(void)
{
	mach_port_name_t	n;

	n = port_set_allocate(current_thread->th_task->t_port_space);
	if (n == MACH_PORT_NULL)
		return (SYS_E_NOMEM);
	return ((long)n);
}

static long
sys_port_set_insert(mach_port_name_t set_name, mach_port_name_t port_name)
{

	return ((long)port_set_insert(current_thread->th_task->t_port_space,
	    set_name, port_name));
}

static long
sys_port_set_remove(mach_port_name_t set_name, mach_port_name_t port_name)
{

	return ((long)port_set_remove(current_thread->th_task->t_port_space,
	    set_name, port_name));
}

/*
 * sys_port_set_extract: the name of the port set `port_name` belongs to
 * in the caller's space, or MACH_PORT_NULL if none.  Read-only.  A bad
 * name or missing RECV right also answers MACH_PORT_NULL; the caller
 * reacts the same way.
 */
static long
sys_port_set_extract(mach_port_name_t port_name)
{

	return ((long)port_set_extract(current_thread->th_task->t_port_space,
	    port_name));
}

/*
 * sys_port_request_notification: register a notify port for an event on
 * `name`: MACH_NOTIFY_NO_SENDERS (caller holds RECEIVE on `name`) or
 * MACH_NOTIFY_DEAD_NAME (caller holds SEND).  The caller needs SEND on
 * `notify_port_name`.  `notify_msgid` comes back in the notification's
 * nh_msgid, so one notify port can watch many sources.
 */
static long
sys_port_request_notification(mach_port_name_t name, uint32_t notify_type,
    mach_port_name_t notify_port_name, uint32_t notify_msgid)
{
	mach_port_name_t	prev;

	return ((long)port_request_notification(
	    current_thread->th_task->t_port_space,
	    name, notify_type, notify_port_name, notify_msgid, &prev));
}

/*
 * sys_spawn_with_port: SYS_SPAWN that also gives the child a SEND right,
 * from the caller's `source_name', at MACH_PORT_PARENT (3).  The name is
 * copied in as in sys_spawn; a SEND ref taken here keeps the port alive
 * through the spawn and is moved into the child by the launcher
 * (space_install_no_ref).  Returns the new task id, or a negative
 * SYS_E_* (SYS_E_INVAL if the caller holds no SEND on source_name).
 */
static long
sys_spawn_with_port(const char *uname, mach_port_name_t source_name)
{
	struct port	*src;
	char		 kname[PROGREG_NAME_MAX];
	size_t		 i;
	long		 uaddr;
	uint8_t		 dummy;

	if (uname == NULL)
		return (SYS_E_FAULT);

	uaddr = (long)(uintptr_t)uname;
	if (!user_range_ok((uint64_t)uaddr, 1))
		return (SYS_E_FAULT);

	smap_user_access_begin();
	for (i = 0; i < PROGREG_NAME_MAX; i++) {
		if (!user_range_ok((uint64_t)(uaddr + (long)i), 1)) {
			smap_user_access_end();
			return (SYS_E_FAULT);
		}
		kname[i] = uname[i];
		if (uname[i] == '\0')
			break;
	}
	smap_user_access_end();

	if (i == PROGREG_NAME_MAX)
		return (SYS_E_INVAL);

	/*
	 * Lookup takes no ref; take one so the port survives until the
	 * launcher consumes it.
	 */
	src = space_lookup(current_thread->th_task->t_port_space,
	    source_name, MACH_PORT_RIGHT_SEND, &dummy);
	if (src == NULL)
		return (SYS_E_INVAL);
	port_ref(src, MACH_PORT_RIGHT_SEND);

	/*
	 * The ref is consumed: moved into the child on success, dropped on
	 * any failure.
	 */
	return (progreg_spawn_with_port(kname, src));
}

/*
 * sys_task_set_exc_port: the older single-port form of
 * SYS_TASK_SET_EXC_PORTS: install `notify_port_name' (a SEND right in the
 * caller's space) in every exception type slot, so all faults go to one
 * watcher.  Returns MACH_MSG_OK or a MACH_E_*; the previous port's name
 * is not returned.
 */
static long
sys_task_set_exc_port(mach_port_name_t notify_port_name)
{

	return (sys_task_set_exc_ports(EXC_MASK_ALL, notify_port_name));
}

/*
 * sys_task_set_exc_ports: set or clear the calling task's exception ports
 * for every EXC_MASK_* type in the low 16 bits of `arg`; the high 16 are
 * behaviour flags (EXC_FLAGS_VALID).  MACH_PORT_NULL clears the slots;
 * otherwise each named slot takes a SEND ref on the port, releasing what
 * it held.  Returns MACH_MSG_OK or a MACH_E_* on a bad name or mask.
 */
static long
sys_task_set_exc_ports(uint32_t arg, mach_port_name_t notify_port_name)
{
	struct task	*t;
	struct port	*new_port;
	uint32_t	 types_mask;
	uint32_t	 flags;
	uint8_t		 dummy;
	int		 rv;

	t = current_thread->th_task;

	/*
	 * Unknown flag bits are refused now rather than silently given a
	 * meaning by a later revision.
	 */
	types_mask = arg & EXC_MASK_ALL;
	flags      = arg & ~EXC_MASK_ALL;
	if ((flags & ~EXC_FLAGS_VALID) != 0)
		return ((long)MACH_E_INVAL);

	new_port = NULL;
	if (notify_port_name != MACH_PORT_NULL) {
		new_port = space_lookup(t->t_port_space, notify_port_name,
		    MACH_PORT_RIGHT_SEND, &dummy);
		if (new_port == NULL)
			return ((long)MACH_E_RIGHT);
	}

	rv = task_set_exception_ports(t, types_mask, new_port);
	if (rv != MACH_MSG_OK)
		return ((long)rv);

	/*
	 * Flags are task-wide, applied to whichever slot fires; stored on
	 * every successful call, even a flags-only one.  Last writer wins.
	 */
	spin_lock(&t->t_lock);
	t->t_exc_flags = flags;
	spin_unlock(&t->t_lock);
	return ((long)MACH_MSG_OK);
}

/*
 * sys_task_get_port_snapshot: copy one mach_port_snapshot_entry per
 * populated slot of the task's port space into the caller's array, for
 * the `lsmp' tool.  Only task_id 0 (self) is accepted: another task would
 * need an authorization primitive (task_for_pid-style).  `max_entries` is
 * capped at MACH_PORT_SNAPSHOT_MAX so the staging buffer fits on the
 * stack.  Returns the number written or a negative SYS_E_*.
 */
static long
sys_task_get_port_snapshot(uint64_t task_id,
    struct mach_port_snapshot_entry *ubuf, size_t max_entries)
{
	struct mach_port_snapshot_entry	 kbuf[MACH_PORT_SNAPSHOT_MAX];
	struct task			*t;
	size_t				 bytes;
	size_t				 i;
	size_t				 n;

	if (max_entries == 0)
		return (0);
	if (max_entries > MACH_PORT_SNAPSHOT_MAX)
		max_entries = MACH_PORT_SNAPSHOT_MAX;

	bytes = max_entries * sizeof(*ubuf);
	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, bytes))
		return (SYS_E_FAULT);

	if (task_id != 0)
		return (SYS_E_INVAL);
	t = current_thread->th_task;

	n = port_space_snapshot(t->t_port_space, kbuf, max_entries);

	/*
	 * Copied out with no port_space lock held, so a faulting user write
	 * cannot deadlock ps_lock.
	 */
	smap_user_access_begin();
	for (i = 0; i < n; i++)
		ubuf[i] = kbuf[i];
	smap_user_access_end();
	return ((long)n);
}

/*
 * sys_task_get_vm_regions: copy one mach_vm_region_entry per vm_map entry
 * into the caller's array, for the `vmmap' tool (after Darwin's
 * vmmap(1)).  Self only (task_id 0), as for the port snapshot.
 */
static long
sys_task_get_vm_regions(uint64_t task_id,
    struct mach_vm_region_entry *ubuf, size_t max_entries)
{
	struct mach_vm_region_entry	 kbuf[MACH_VM_REGION_MAX];
	struct task			*t;
	size_t				 bytes;
	size_t				 i;
	size_t				 n;

	if (max_entries == 0)
		return (0);
	if (max_entries > MACH_VM_REGION_MAX)
		max_entries = MACH_VM_REGION_MAX;

	bytes = max_entries * sizeof(*ubuf);
	if (!user_range_ok((uint64_t)(uintptr_t)ubuf, bytes))
		return (SYS_E_FAULT);

	if (task_id != 0)
		return (SYS_E_INVAL);
	t = current_thread->th_task;

	n = vm_map_snapshot(t->t_map, kbuf, max_entries);

	smap_user_access_begin();
	for (i = 0; i < n; i++)
		ubuf[i] = kbuf[i];
	smap_user_access_end();
	return ((long)n);
}

/*
 * sys_task_kill: capability-based asynchronous terminate.  Resolves
 * `target_port_name` with SEND in the caller's space, requires it to be a
 * task-self port (PORT_SPECIAL_TASK_SELF), and requests termination of
 * the task id it stores.  You cannot kill what you hold no port to: your
 * own (MACH_PORT_TASK_SELF, 1), or one handed over in a message, by
 * SYS_SPAWN_WITH_PORT or SYS_SPAWN_RETURNS_TASKPORT.  kernel_task is
 * refused, though ring 3 cannot reach its port anyway.
 *
 * Returns MACH_MSG_OK, MACH_E_RIGHT if the SEND lookup fails, or
 * MACH_E_INVAL for a non-task port or kernel_task.  The kill is queued; a
 * self-kill retires at this syscall's exit (detection point #5), another
 * task dies asynchronously.
 */
static long
sys_task_kill(mach_port_name_t target_port_name)
{
	struct port	*p;
	uint64_t	 target_id;
	uint8_t		 dummy;

	p = space_lookup(current_thread->th_task->t_port_space,
	    target_port_name, MACH_PORT_RIGHT_SEND, &dummy);
	if (p == NULL)
		return ((long)MACH_E_RIGHT);

	if (p->p_special != PORT_SPECIAL_TASK_SELF)
		return ((long)MACH_E_INVAL);

	/*
	 * The port stores the task's id, not a struct task *, which would
	 * dangle once the task is reaped while a SEND right keeps the port
	 * alive.  task_request_terminate re-validates the id under
	 * tasks_lock and does nothing if the task is gone.
	 */
	target_id = (uint64_t)(uintptr_t)p->p_special_arg;
	if (target_id == 0 || target_id == kernel_task->t_id)
		return ((long)MACH_E_INVAL);

	task_request_terminate(target_id);

	/*
	 * A self-kill is not short-circuited here: syscall_dispatch's exit
	 * check retires the thread before it returns to ring 3.
	 */
	return ((long)MACH_MSG_OK);
}

/*
 * sys_spawn_returns_taskport: spawn for a caller that will manage the
 * child.  Returns the task id and writes to `out_taskport_name` a name in
 * the caller's space with SEND on the child's task-self port -- exactly
 * what SYS_TASK_KILL takes: if you spawned it, you can kill it.
 * SYS_E_FAULT for bad pointers, SYS_E_INVAL for an unknown program, or
 * the launcher's SYS_E_* on setup failure; `*out_taskport_name` is then
 * untouched.
 */
static long
sys_spawn_returns_taskport(const char *uname,
    mach_port_name_t *out_taskport_name)
{
	char			 kname[PROGREG_NAME_MAX];
	mach_port_name_t	 kname_out;
	size_t			 i;
	long			 uaddr;
	long			 rv;

	if (uname == NULL || out_taskport_name == NULL)
		return (SYS_E_FAULT);

	uaddr = (long)(uintptr_t)uname;
	if (!user_range_ok((uint64_t)uaddr, 1))
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)(uintptr_t)out_taskport_name,
	    sizeof(mach_port_name_t)))
		return (SYS_E_FAULT);

	smap_user_access_begin();
	for (i = 0; i < PROGREG_NAME_MAX; i++) {
		if (!user_range_ok((uint64_t)(uaddr + (long)i), 1)) {
			smap_user_access_end();
			return (SYS_E_FAULT);
		}
		kname[i] = uname[i];
		if (uname[i] == '\0')
			break;
	}
	smap_user_access_end();

	if (i == PROGREG_NAME_MAX)
		return (SYS_E_INVAL);

	kname_out = MACH_PORT_NULL;
	rv = progreg_spawn_returning_taskport(kname,
	    current_thread->th_task->t_port_space, &kname_out);
	if (rv < 0)
		return (rv);

	smap_user_access_begin();
	*out_taskport_name = kname_out;
	smap_user_access_end();

	return (rv);
}

/*
 * sys_spawn_args: the full spawn -- a program name, an argument vector,
 * and the taskport of SYS_SPAWN_RETURNS_TASKPORT.  argv is copied into one
 * flat block ((argc+1) char * slots, the last NULL, then the packed
 * strings), freed by a single kfree, which progreg_spawn_args consumes.
 * argc 0 or a NULL uargv spawns with no arguments.  argc beyond
 * SPAWN_ARGV_MAX or more than SPAWN_ARG_BYTES_MAX string bytes is
 * SYS_E_INVAL.  On failure *out_taskport_name is untouched.
 */
static long
sys_spawn_args(const char *uname, char *const *uargv, uint64_t argc,
    mach_port_name_t *out_taskport_name)
{
	char			 kname[PROGREG_NAME_MAX];
	char			*block;
	char		       **kargv;
	char			*strs;
	const char		*uarg;
	mach_port_name_t	 kname_out;
	size_t			 ptrs_sz;
	size_t			 used;
	size_t			 i;
	size_t			 k;
	long			 uaddr;
	long			 rv;

	if (uname == NULL || out_taskport_name == NULL)
		return (SYS_E_FAULT);
	if (argc > SPAWN_ARGV_MAX)
		return (SYS_E_INVAL);

	uaddr = (long)(uintptr_t)uname;
	if (!user_range_ok((uint64_t)uaddr, 1))
		return (SYS_E_FAULT);
	if (!user_range_ok((uint64_t)(uintptr_t)out_taskport_name,
	    sizeof(mach_port_name_t)))
		return (SYS_E_FAULT);

	/* Copy the program name (bounded, NUL-terminated). */
	smap_user_access_begin();
	for (i = 0; i < PROGREG_NAME_MAX; i++) {
		if (!user_range_ok((uint64_t)(uaddr + (long)i), 1)) {
			smap_user_access_end();
			return (SYS_E_FAULT);
		}
		kname[i] = uname[i];
		if (uname[i] == '\0')
			break;
	}
	smap_user_access_end();
	if (i == PROGREG_NAME_MAX)
		return (SYS_E_INVAL);

	/* No argument vector: identical to sys_spawn_returns_taskport. */
	if (argc == 0 || uargv == NULL) {
		kname_out = MACH_PORT_NULL;
		rv = progreg_spawn_args(kname, 0, NULL,
		    current_thread->th_task->t_port_space, &kname_out);
		if (rv < 0)
			return (rv);
		smap_user_access_begin();
		*out_taskport_name = kname_out;
		smap_user_access_end();
		return (rv);
	}

	if (!user_range_ok((uint64_t)(uintptr_t)uargv, argc * sizeof(char *)))
		return (SYS_E_FAULT);

	ptrs_sz = (size_t)(argc + 1) * sizeof(char *);
	block = kmalloc(ptrs_sz + SPAWN_ARG_BYTES_MAX);
	if (block == NULL)
		return (SYS_E_NOMEM);
	kargv = (char **)block;
	strs  = block + ptrs_sz;
	used  = 0;

	for (i = 0; i < argc; i++) {
		/* Pull the i'th user pointer out of the argv array. */
		smap_user_access_begin();
		uarg = uargv[i];
		smap_user_access_end();

		if (uarg == NULL ||
		    !user_range_ok((uint64_t)(uintptr_t)uarg, 1)) {
			kfree(block);
			return (SYS_E_FAULT);
		}

		kargv[i] = strs + used;

		/* Copy this argument (NUL included) into the packed region. */
		k = 0;
		smap_user_access_begin();
		for (;;) {
			if (used >= SPAWN_ARG_BYTES_MAX) {
				smap_user_access_end();
				kfree(block);
				return (SYS_E_INVAL);
			}
			if (!user_range_ok((uint64_t)(uintptr_t)(uarg + k), 1)) {
				smap_user_access_end();
				kfree(block);
				return (SYS_E_FAULT);
			}
			strs[used] = uarg[k];
			used++;
			if (uarg[k] == '\0')
				break;
			k++;
		}
		smap_user_access_end();
	}
	kargv[argc] = NULL;

	/* Ownership of `block` passes to progreg_spawn_args from here. */
	kname_out = MACH_PORT_NULL;
	rv = progreg_spawn_args(kname, (int)argc, kargv,
	    current_thread->th_task->t_port_space, &kname_out);
	if (rv < 0)
		return (rv);

	smap_user_access_begin();
	*out_taskport_name = kname_out;
	smap_user_access_end();
	return (rv);
}

/*
 * sys_thread_set_exc_ports: sys_task_set_exc_ports for the calling thread
 * (there is no way to name another), with the same ref accounting and no
 * flag bits.  Returns MACH_MSG_OK or a MACH_E_*.
 */
static long
sys_thread_set_exc_ports(uint32_t types_mask, mach_port_name_t notify_port_name)
{
	struct task	*t;
	struct port	*new_port;
	uint8_t		 dummy;

	t = current_thread->th_task;

	if ((types_mask & ~EXC_MASK_ALL) != 0)
		return ((long)MACH_E_INVAL);

	new_port = NULL;
	if (notify_port_name != MACH_PORT_NULL) {
		new_port = space_lookup(t->t_port_space, notify_port_name,
		    MACH_PORT_RIGHT_SEND, &dummy);
		if (new_port == NULL)
			return ((long)MACH_E_RIGHT);
	}

	return ((long)thread_set_exception_ports(current_thread, types_mask,
	    new_port));
}
