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
	default:
		return (SYS_E_NOSYS);
	}
}

long
syscall_dispatch(struct syscall_frame *f)
{
	long	rv;

	/*
	 * Async-kill detection point #1.  If task_request_terminate
	 * fired while this thread was running in ring 3, t_killed is set
	 * by the time we re-enter the kernel here.  Retire before
	 * dispatching: the caller has been marked dead and any reply
	 * we'd produce will never be observed (the user pages are about
	 * to be torn down).  See kern/task.h's t_killed comment for the
	 * full detection-site list.
	 */
	if (current_thread->th_task != kernel_task &&
	    task_kill_pending(current_thread->th_task))
		thread_exit();
	/* NOTREACHED if killed */

	/*
	 * Darwin-personality tasks (a Mach-O that declared PLATFORM_MACOS)
	 * dispatch through the Apple class-encoded path; every native style9
	 * task takes the untouched table below.  See kern/darwin.c.
	 */
	if (current_thread->th_task->t_personality == TASK_PERSONALITY_DARWIN)
		rv = darwin_dispatch(f);
	else
		rv = syscall_dispatch_body(f);

	/*
	 * Detection point #5: syscall-exit kill check.  Catches the
	 * "syscall ITSELF caused the kill" case (e.g. SYS_TASK_KILL on
	 * self, or a future signal-style syscall that posts a kill to
	 * its own task).  The entry check (#1) would catch this on the
	 * NEXT syscall, but a self-kill should retire immediately so
	 * the user never gets a sysretq -- no half-step of returned user
	 * code between issuing the kill and dying.  The IRQ-return
	 * detection point (#4) and the thread_block_release pre-park /
	 * post-wake points (#2, #3) cover the gaps if for some reason
	 * we miss here, but this is the cleanest path.
	 */
	if (current_thread->th_task != kernel_task &&
	    task_kill_pending(current_thread->th_task))
		thread_exit();
	/* NOTREACHED if killed */

	return (rv);
}

/*
 * syscall_console_write: copy `len` bytes from the user buffer into a kernel
 * scratch under an SMAP bracket, then push them to the tty (without holding
 * AC=1 across the tty lock + console output).  Returns bytes written, or
 * SYS_E_FAULT if the buffer escapes the user-VA window; the 4 KiB cap keeps
 * the scratch on the kernel stack.  Backs both SYS_PRINT and the Darwin
 * personality's write(2) (kern/darwin.c).
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

	/*
	 * Copy out of the user buffer into a kernel scratch under SMAP
	 * bracket, then push to tty without holding AC=1 across the
	 * tty's locking + console output path.  4 KiB cap keeps the
	 * scratch on the kernel stack.
	 */
	smap_user_access_begin();
	for (i = 0; i < len; i++)
		scratch[i] = buf[i];
	smap_user_access_end();

	for (i = 0; i < len; i++)
		tty_putc(scratch[i]);

	return ((long)len);
}

static long
sys_print(const char *buf, size_t len)
{

	return (syscall_console_write(buf, len));
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

/*
 * Shared copyin for a NUL-terminated user string.  Walks up to kbuf_size
 * bytes from uptr, range-checking every byte under one SMAP bracket and
 * stopping at the first NUL.  Returns the length (excluding NUL), SYS_E_FAULT
 * on a bad pointer, or SYS_E_INVAL when no NUL appears in kbuf_size bytes.
 * The textbook copyin_str the inline copies in sys_spawn et al. predate; the
 * Darwin dyld backchannel (kern/darwin.c) routes its path argument through it.
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
 * Mach message send/recv.  The user supplies a pointer to a Mach header;
 * we honour the user's msgh_size and read the bytes directly from the
 * caller's address space (its pmap is current for the syscall's lifetime).
 * No kmalloc round-trip on send -- the mach_msg_send path makes its own
 * copy into the queued message.
 *
 * The range-check + SMAP-bracket + mach_msg_* core lives in the non-static
 * syscall_msg_* helpers so the Darwin personality's mach_msg trap
 * (kern/darwin.c) reuses the exact same path; the sys_msg_* wrappers are
 * just the style9 syscall-table entry points.
 */
long
syscall_msg_send(const struct mach_msg_header *umsg)
{
	uint32_t	msgh_size;

	if (!user_range_ok((uint64_t)(uintptr_t)umsg,
	    sizeof(struct mach_msg_header)))
		return (SYS_E_FAULT);

	/*
	 * Read msgh_size under one SMAP bracket and then bound the
	 * whole [umsg, umsg+msgh_size) range against the user-VA
	 * window before mach_msg_send's copyin reads the body.  This
	 * keeps the previous "body fits in user VA" guarantee without
	 * leaving an unbracketed deref on the SMAP-enabled path.
	 */
	smap_user_access_begin();
	msgh_size = umsg->msgh_size;
	smap_user_access_end();

	if (!user_range_ok((uint64_t)(uintptr_t)umsg, msgh_size))
		return (SYS_E_FAULT);

	return ((long)mach_msg_send(current_thread->th_task->t_port_space,
	    umsg));
}

static long
sys_msg_send(const struct mach_msg_header *umsg)
{

	return (syscall_msg_send(umsg));
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

	/*
	 * Bracket the byte-by-byte copy of the user name string.  Walks
	 * up to PROGREG_NAME_MAX bytes, stopping at the first NUL.  The
	 * per-byte user_range_ok already covers the address; the bracket
	 * lets the kernel actually read once CR4.SMAP is enabled.
	 */
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
 * sys_task_alive: 1 if a task with this id is still on the live list,
 * 0 if not.  Cheap polling primitive that lets the userspace shell
 * yield-spin until a spawned child has terminated; a real blocking
 * exit-notify port (Mach death notification) is a phase-3 conversation.
 * No fault paths -- the id is a scalar, no user-VA touched.
 */
static long
sys_task_alive(uint64_t task_id)
{

	return (task_is_alive(task_id) ? 1 : 0);
}

/*
 * sys_vm_allocate: hand back a fresh user-VA range, populated with
 * anonymous (zeroed) pages, mapped read/write (and execute if asked)
 * with U=1.  `size` is rounded up to a multiple of 4 KiB; `prot`
 * carries any of VM_PROT_READ / VM_PROT_WRITE / VM_PROT_EXEC --
 * VM_PROT_USER is set unconditionally since vm_allocate is always
 * user-facing.  Returns the chosen VA on success, negative SYS_E_*
 * on failure.
 *
 * Failure paths unwind: pmm_alloc_page or pmap_enter failures, and a
 * final vm_map_enter failure, roll back every page touched so far so
 * a partial allocation never leaks frames.
 */
static long
sys_vm_allocate(uint64_t size, uint32_t prot)
{
	struct task	*t;
	uint8_t		*kva;
	uint64_t	 pa;
	uint64_t	 v;
	uint64_t	 va;
	size_t		 i;
	size_t		 j;
	size_t		 pages;
	uint32_t	 pmap_flags;

	if (size == 0)
		return (SYS_E_INVAL);

	size = (size + 0xFFFull) & ~0xFFFull;
	if (size == 0)
		return (SYS_E_NOMEM);

	prot      &= (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC);
	pmap_flags = prot | VM_PROT_USER;
	pages      = (size_t)(size >> 12);

	t = current_thread->th_task;

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

	return ((long)va);

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
 * sys_vm_deallocate: release a previously vm_allocate'd range.  v1
 * requires the (va, size) pair to mirror the allocate exactly -- the
 * range must start at a known anonymous vm_map_entry whose extent
 * covers the request.  Partial deallocate and ranges crossing entry
 * boundaries are rejected via vm_map_release returning false.  Returns
 * 0 on success, negative SYS_E_* otherwise.
 */
static long
sys_vm_deallocate(uint64_t va, uint64_t size)
{
	struct task	*t;

	if (size == 0)
		return (SYS_E_INVAL);
	if ((va & 0xFFFull) != 0)
		return (SYS_E_INVAL);

	size = (size + 0xFFFull) & ~0xFFFull;
	if (size == 0)
		return (SYS_E_INVAL);

	t = current_thread->th_task;
	if (!vm_map_release(t->t_map, t->t_pmap, va, size))
		return (SYS_E_INVAL);
	return (0);
}

/*
 * sys_port_mod_refs: drop ONE right kind from a name in the caller's
 * port space.  Used by callers that need to split a name carrying both
 * RECV and SEND (e.g. drop only SEND while keeping RECV to receive
 * back) without tearing the whole slot down.  Returns a MACH_E_* on
 * failure -- not a SYS_E_* -- so the user sees the Mach-layer reason.
 */
static long
sys_port_mod_refs(mach_port_name_t name, uint8_t right)
{

	return ((long)port_mod_refs(current_thread->th_task->t_port_space,
	    name, right));
}

/*
 * sys_port_set_alloc: create a new port set in the caller's space and
 * return its name.  A port set bundles multiple ports' receive queues
 * behind one name so a single mach_msg_recv can serve any member.  The
 * name carries MACH_PORT_RIGHT_PORT_SET; you cannot SEND to it.
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
 * sys_port_set_extract: returns the name of the port set `port_name`
 * belongs to in the calling task's space, or 0 (MACH_PORT_NULL) when
 * the port is not currently a member of any set.  Read-only; never
 * mutates the space.  Failure modes (bad name, no RECV right) collapse
 * to MACH_PORT_NULL -- same encoding as "not in a set" since the
 * caller's expected reaction is identical in both cases.
 */
static long
sys_port_set_extract(mach_port_name_t port_name)
{

	return ((long)port_set_extract(current_thread->th_task->t_port_space,
	    port_name));
}

/*
 * sys_port_request_notification: register a notify port to receive a
 * MACH_NOTIFY_* message when the source port reaches the matching
 * event.  v1 supports MACH_NOTIFY_NO_SENDERS only; the caller must hold
 * RECEIVE on `name` and SEND on `notify_port_name`, both in the
 * current task's port space.  The user-supplied `notify_msgid` is
 * carried back unchanged in the notification's nh_msgid field so a
 * service that watches many ports through one notify port can
 * disambiguate the source.
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
 * sys_spawn_with_port: variant of SYS_SPAWN that also hands the child
 * a SEND right at the well-known MACH_PORT_PARENT slot.
 *
 * The caller passes the program name plus a name in their own
 * port_space carrying SEND right.  This handler:
 *	1. validates + copies in the program name (same as sys_spawn),
 *	2. looks up source_name in the caller's space with SEND right,
 *	3. takes a SEND ref so the port survives the spawn race,
 *	4. hands the ref to progreg_spawn_with_port, which forwards it
 *	   into arch_spawn_user's user_spawn_arg; the launcher transfers
 *	   the ref into the child's port_space via space_install_no_ref
 *	   so the install lands at name == MACH_PORT_PARENT (== 3).
 *
 * Returns the new task's id on success, or a negative SYS_E_* on
 * failure (including MACH_E_RIGHT mapped to SYS_E_INVAL when the
 * caller does not actually hold SEND on source_name).
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
	 * Look up the source name with SEND right.  Lookup does not
	 * take a ref; bump it explicitly so the port survives the
	 * window between this return and the launcher consuming it.
	 */
	src = space_lookup(current_thread->th_task->t_port_space,
	    source_name, MACH_PORT_RIGHT_SEND, &dummy);
	if (src == NULL)
		return (SYS_E_INVAL);
	port_ref(src, MACH_PORT_RIGHT_SEND);

	/*
	 * progreg_spawn_with_port owns the ref from here -- on success
	 * the launcher transfers it into the child's name table, on
	 * any failure path arch_spawn_user port_derefs it.  No further
	 * cleanup needed in this handler.
	 */
	return (progreg_spawn_with_port(kname, src));
}

/*
 * sys_task_set_exc_port: install (or replace) the current task's
 * exception port.  Caller passes a name in their own space carrying
 * SEND right; the kernel takes a SEND ref, swaps into t_exc_port, and
 * releases any previous slot's ref.  v1 returns MACH_MSG_OK or a
 * MACH_E_* without exposing the previous slot's name (matching the
 * port_request_notification convention).
 */
static long
sys_task_set_exc_port(mach_port_name_t notify_port_name)
{

	/*
	 * A v1 back-compat: SYS_TASK_SET_EXC_PORT installs the same
	 * notify port across every type slot, so a caller compiled
	 * before A v2 still routes every fault to its single watcher.
	 * SYS_TASK_SET_EXC_PORTS is the modern form (per-type masks).
	 */
	return (sys_task_set_exc_ports(EXC_MASK_ALL, notify_port_name));
}

/*
 * sys_task_set_exc_ports: install (or clear) the calling task's
 * exception ports for every type named in `types_mask` (one or more
 * of EXC_MASK_*).  `notify_port_name == MACH_PORT_NULL` clears the
 * named slots; otherwise the kernel takes popcount(types_mask) SEND
 * refs on the resolved port and writes it into each named slot,
 * replacing whatever was there (refs released via port_deref).
 *
 * Returns MACH_MSG_OK or a MACH_E_* on bad name / bad mask.
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
	 * Pack types in the low 16 bits, behavior flags in the high
	 * 16: a single syscall covers both selection and policy.
	 * Reject any bits outside EXC_MASK_ALL / EXC_FLAGS_VALID so
	 * forward-incompatible callers fail fast rather than having
	 * their stray bits silently reinterpreted by a future revision.
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
	 * Flags are a task-wide property, applied to whichever slot
	 * eventually fires.  Stash whenever the syscall changes any
	 * slot bookkeeping (including the no-types empty-mask case
	 * with flags-only).  Last writer wins.
	 */
	spin_lock(&t->t_lock);
	t->t_exc_flags = flags;
	spin_unlock(&t->t_lock);
	return ((long)MACH_MSG_OK);
}

/*
 * sys_task_get_port_snapshot: copy one wire-format
 * mach_port_snapshot_entry per populated slot in the named task's
 * port_space into the caller's array.  Drives the userspace `lsmp`
 * tool ("list mach ports"): a debugger-style introspection surface
 * that has no Linux equivalent because Linux has no Mach ports.
 *
 * v1 supports only task_id == 0 (snapshot self).  Cross-task
 * introspection would need either a task_for_pid-style authorization
 * primitive or a per-task ref-bumping lookup; deferred until a real
 * consumer (e.g. an external debugger task) shows up.
 *
 * Returns the number of entries written on success, or a negative
 * SYS_E_*.  `max_entries` is capped at MACH_PORT_SNAPSHOT_MAX so the
 * kernel staging buffer stays on the syscall stack.
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
	 * Copy out without holding any port_space lock: kbuf is a
	 * private stack snapshot, so a faulting user_buf write only
	 * tears down this syscall, never deadlocks ps_lock.
	 */
	smap_user_access_begin();
	for (i = 0; i < n; i++)
		ubuf[i] = kbuf[i];
	smap_user_access_end();
	return ((long)n);
}

/*
 * sys_task_get_vm_regions: copy one wire-format mach_vm_region_entry
 * per live entry in the named task's vm_map into the caller's array.
 * Drives the userspace `vmmap` tool: dumps a process's VM layout the
 * same way Darwin's vmmap(1) does, again a question Linux only
 * answers via /proc parsing.
 *
 * v1 supports only task_id == 0 (self) for the same reason as the
 * port-snapshot syscall; cross-task introspection needs an auth
 * primitive that has not landed yet.
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
 * sys_task_kill: capability-based async terminate.
 *
 * `target_port_name` is a port name in the caller's space.  The kernel
 * resolves it, verifies the SEND right, checks that the port's special
 * tag is PORT_SPECIAL_TASK_SELF (i.e., the named port IS a task's
 * task-self port), reads the target task id stored in p_special_arg
 * (never a raw struct task * -- a task-self port can outlive its task),
 * and fires task_request_terminate against it.
 *
 * Caller can trivially kill itself by passing MACH_PORT_TASK_SELF (==1)
 * -- every task has its own task-self port wired into slot 1 of its
 * space at task_create time.  To kill *another* task, the caller must
 * hold SEND on the target's task-self port; today the only ways to
 * acquire that are (a) being handed it via OOL, (b) parent-inject via
 * SYS_SPAWN_WITH_PORT.  So the v1 attack surface is small and
 * Mach-shaped: you cannot kill what you do not have a port to.
 *
 * Refuses kernel_task explicitly (its task-self port is in
 * kernel_space, which is not directly reachable from ring 3 anyway,
 * but the check is cheap and documents intent).
 *
 * Returns MACH_MSG_OK on success, MACH_E_RIGHT if the SEND lookup
 * fails, MACH_E_INVAL if the port is not a task-self port or names
 * kernel_task.  Async semantics: the kill is queued; if the caller
 * killed itself, the actual retire happens on this syscall's return
 * (the dispatch-entry detection point catches it on the NEXT syscall,
 * or this very return path runs through the IRQ-return check at the
 * next PIT tick).  More commonly the caller is not the target, so the
 * syscall returns normally and the target dies asynchronously.
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
	 * The task-self port stores the target's immutable id, never a
	 * raw struct task * (which would dangle once the task is reaped
	 * while this SEND right keeps the port object alive -- the very
	 * UAF this avoids).  Hand the id to task_request_terminate, which
	 * re-validates it under tasks_lock and silently no-ops if the
	 * task is already gone; a stale taskport name thus kills nothing
	 * instead of dereferencing freed memory.  kernel_task is id 1 and
	 * is refused explicitly.
	 */
	target_id = (uint64_t)(uintptr_t)p->p_special_arg;
	if (target_id == 0 || target_id == kernel_task->t_id)
		return ((long)MACH_E_INVAL);

	task_request_terminate(target_id);

	/*
	 * Caller may have just killed itself.  The detection sites take
	 * care of it on the return path (syscall_dispatch's entry check
	 * fires on the next syscall; thread_block_release's pre-park and
	 * the IRQ-return check cover the in-between cases).  Don't try
	 * to short-circuit here -- returning MACH_MSG_OK gives the user
	 * code its sysret if the kill targeted a different task, and
	 * does no harm if it was a self-kill (the thread retires before
	 * any user-visible side effect).
	 */
	return ((long)MACH_MSG_OK);
}

/*
 * sys_spawn_returns_taskport: spawn variant for callers that intend
 * to manage the child (kill/signal it later).  Atomically returns
 * BOTH the new task's id (positive return value) AND a port name in
 * the caller's space holding SEND on the child's task-self port,
 * written through `out_taskport_name`.
 *
 * The capability handed back is the exact argument SYS_TASK_KILL
 * accepts: parent + future task-manager service use this to terminate
 * a child without needing any other lookup or auth.  The Mach
 * fingerprint is: "if you spawned it, you can kill it."
 *
 * Failures fan out into SYS_E_FAULT (bad pointers), SYS_E_INVAL (no
 * such program), SYS_E_NOSYS (out-of-resources during the new task's
 * setup).  On failure `*out_taskport_name` is untouched.
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
 * sys_spawn_args: the full spawn -- a program name, a command-line
 * argument vector, AND (exactly like SYS_SPAWN_RETURNS_TASKPORT) a SEND
 * right on the new task's task-self port installed in the caller's
 * space.  The argv strings are copied into one kernel-side flattened
 * block: (argc+1) leading char* slots (the last is the NULL
 * terminator) followed by the packed, NUL-separated strings, so each
 * kargv[i] points into the same allocation and a single kfree releases
 * it.  Ownership of that block transfers to progreg_spawn_args, which
 * frees it on failure and hands it to the launcher (which materialises
 * it onto the child's stack, then frees it) on success.
 *
 * argc 0 / uargv NULL degrades to the argument-free returns-taskport
 * spawn.  Bounds: argc <= SPAWN_ARGV_MAX, total string bytes <
 * SPAWN_ARG_BYTES_MAX -- a violation is SYS_E_INVAL.  On any failure
 * *out_taskport_name is untouched.
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
	block = (char *)kmalloc(ptrs_sz + SPAWN_ARG_BYTES_MAX);
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
 * sys_thread_set_exc_ports: per-thread variant of
 * sys_task_set_exc_ports.  Operates on the calling thread (no API
 * for setting another thread's slots today); the kernel takes ref
 * accounting the same way as the task-level path.
 *
 * Returns MACH_MSG_OK or a MACH_E_*.
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
