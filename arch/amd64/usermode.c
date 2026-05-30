/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "elf.h"
#include "gdt.h"
#include "kmem.h"
#include "kprintf.h"
#include "macho.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "port_internal.h"
#include "progreg.h"
#include "sched.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "usermode.h"
#include "vm.h"

extern uint8_t	user_blob_start[];
extern uint8_t	user_blob_end[];

/*
 * The clean-room dynamic linker (user/dyld.c), embedded as a Mach-O blob.
 * The launcher maps it alongside any image that carries an LC_LOAD_DYLINKER
 * and enters through it -- see usermode_elf_launcher.  Not a progreg program:
 * dyld is the linker, never spawned by name.
 */
extern uint8_t	_binary_dyld_macho_start[];
extern uint8_t	_binary_dyld_macho_end[];

/*
 * Boot-time blob path: the very first ring-3 program (a hand-written
 * scrap of asm in arch/amd64/user_blob.S) for early SYS_PRINT/EXIT
 * smoke testing.  Kept around because it is still a viable "no libc"
 * minimal ring-3 entry point.
 */
static void	usermode_launcher(void *) __attribute__((noreturn));

/*
 * Carries a spawn intent across the thread boundary.  Allocated by
 * arch_spawn_user, freed by the launcher just before it iretq's so
 * leaks don't accumulate if the program loops forever in ring 3.
 *
 * sa_inject_port is optional: when non-NULL the launcher installs a
 * SEND right on it into the child's port_space at MACH_PORT_PARENT.
 * Caller has already taken one SEND ref on the port; the install
 * transfers that ref into the child's name table (space_install_no_ref).
 *
 * sa_argv is the optional command line: a kernel-owned flattened block
 * (the leading sa_argc char* slots point into the trailing packed
 * strings) or NULL.  The launcher copies the strings onto the child's
 * stack and kfrees the block; arch_spawn_user kfrees it on any failure
 * path before the launcher ever runs.
 */
struct user_spawn_arg {
	const char	*sa_name;
	const uint8_t	*sa_image;
	size_t		 sa_image_size;
	struct port	*sa_inject_port;
	char	       **sa_argv;
	int		 sa_argc;
};

static void	usermode_elf_launcher(void *) __attribute__((noreturn));

void
usermode_run_first_blob(void)
{
	struct task	*ut;
	struct thread	*th;

	ut = task_create("user-blob");
	if (ut == NULL)
		panic("usermode_run_first_blob: task_create failed");
	th = thread_create(ut, usermode_launcher, NULL, "user-blob");
	if (th == NULL)
		panic("usermode_run_first_blob: thread_create failed");
	thread_start(th);
}

/*
 * Generic user-program spawn entry.  Called from progreg_spawn (which
 * looked up the embedded ELF blob in the program registry).  Builds a
 * fresh task, allocates a small descriptor on the kernel heap to hand
 * the image pointer + name across to the launcher thread, then
 * thread_starts it.  Returns the new task's id, or a negative
 * SYS_E_* on allocation failure.
 *
 * The descriptor is heap-allocated rather than stack so it survives
 * after arch_spawn_user returns; the launcher takes ownership and
 * kfrees it during setup.
 */
long
arch_spawn_user(const char *name, const uint8_t *image, size_t image_size,
    struct port *inject_port, struct port_space *caller_space,
    mach_port_name_t *out_taskport_name, int argc, char **argv)
{
	struct user_spawn_arg	*sa;
	struct task		*ut;
	struct thread		*th;
	mach_port_name_t	 taskport_name;
	int			 rv;

	if (name == NULL || image == NULL || image_size == 0) {
		if (inject_port != NULL)
			port_deref(inject_port, MACH_PORT_RIGHT_SEND);
		if (argv != NULL)
			kfree(argv);
		return (SYS_E_INVAL);
	}

	sa = (struct user_spawn_arg *)kmalloc(sizeof(*sa));
	if (sa == NULL) {
		if (inject_port != NULL)
			port_deref(inject_port, MACH_PORT_RIGHT_SEND);
		if (argv != NULL)
			kfree(argv);
		return (SYS_E_NOSYS);	/* OOM; closest in our small set */
	}
	sa->sa_name        = name;
	sa->sa_image       = image;
	sa->sa_image_size  = image_size;
	sa->sa_inject_port = inject_port;
	sa->sa_argc        = argv != NULL ? argc : 0;
	sa->sa_argv        = argv;

	ut = task_create(name);
	if (ut == NULL) {
		if (inject_port != NULL)
			port_deref(inject_port, MACH_PORT_RIGHT_SEND);
		if (argv != NULL)
			kfree(argv);
		kfree(sa);
		return (SYS_E_NOSYS);
	}

	/*
	 * Optional caller-space taskport install.  When `caller_space`
	 * is non-NULL, the caller wants a SEND right on the new task's
	 * task-self port installed in their space + the resulting name
	 * written back.  Done BEFORE thread_create so a failure path
	 * cleanly task_derefs the not-yet-running task -- t_refs is
	 * still 1 (no thread attached), so deref to 0 frees it via
	 * task__chain_remove.
	 *
	 * Powers SYS_SPAWN_RETURNS_TASKPORT: the shell + future
	 * task-manager-style services use this to acquire the
	 * capability needed by SYS_TASK_KILL on the child.
	 */
	if (caller_space != NULL && out_taskport_name != NULL) {
		rv = space_install(caller_space, ut->t_self_port,
		    MACH_PORT_RIGHT_SEND, &taskport_name);
		if (rv != MACH_MSG_OK) {
			if (inject_port != NULL)
				port_deref(inject_port, MACH_PORT_RIGHT_SEND);
			if (argv != NULL)
				kfree(argv);
			task_deref(ut);
			kfree(sa);
			return (SYS_E_NOSYS);
		}
		*out_taskport_name = taskport_name;
	}

	th = thread_create(ut, usermode_elf_launcher, sa, "user-elf");
	if (th == NULL) {
		if (inject_port != NULL)
			port_deref(inject_port, MACH_PORT_RIGHT_SEND);
		if (argv != NULL)
			kfree(argv);
		if (caller_space != NULL && out_taskport_name != NULL) {
			/*
			 * Roll back the taskport install on thread-create
			 * failure: drop the SEND ref we installed in the
			 * caller's space.  Otherwise the caller would
			 * receive a name pointing at a port whose task is
			 * about to be freed -- a UAF window the moment they
			 * try to use it.
			 */
			(void)space_drop_one_right(caller_space,
			    *out_taskport_name, MACH_PORT_RIGHT_SEND);
			*out_taskport_name = MACH_PORT_NULL;
		}
		task_deref(ut);
		kfree(sa);
		return (SYS_E_NOSYS);
	}
	thread_start(th);

	/*
	 * Drop the creator's ref now that the task is anchored by its
	 * thread.  task_create returns t_refs=1; thread_create's
	 * task_attach_thread bumps it to 2.  Without this release, the
	 * final task_detach_thread on the exiting user thread would only
	 * take refs 2 -> 1, never to 0, and the dead task would linger in
	 * task_list forever -- SYS_TASK_ALIVE would keep reporting it
	 * alive and the userspace shell's wait_child yield-spin would
	 * never terminate.
	 *
	 * Safe to drop here because the thread is already enqueued: the
	 * scheduler holds a stable view of the task via th_task whether
	 * or not the thread has run yet, and task_deref only frees the
	 * task when t_refs hits zero AND t_nthreads == 0 (KASSERT'd).
	 */
	task_deref(ut);
	return ((long)ut->t_id);
}

static size_t
spawn_strlen(const char *s)
{
	size_t	n;

	n = 0;
	while (s[n] != '\0')
		n++;
	return (n);
}

/*
 * Materialise the SysV-style initial stack for a freshly loaded ring-3
 * program in its (already mapped + zeroed) stack page, writing through
 * the kernel alias `kva_base` of the page's USER_STACK_VA base.  The
 * layout, low address to high, starting at the returned %rsp:
 *
 *	[ argc           ]   <- returned user %rsp (16-byte aligned)
 *	[ argv[0]        ]      user VA of the first string
 *	[ ...            ]
 *	[ argv[argc-1]   ]
 *	[ NULL           ]      argv terminator
 *	[ (alignment gap)]
 *	[ packed strings ]   <- top of the page
 *
 * crt0.S reads argc at %rsp and argv at %rsp+8.  argc==0 still lays
 * down a valid (argc=0, argv[0]=NULL) frame, so every program -- with
 * arguments or not -- enters through the one path.  The progreg.h caps
 * (SPAWN_ARGV_MAX / SPAWN_ARG_BYTES_MAX) guarantee the whole block fits
 * the page with room to spare; the KASSERT documents that invariant
 * rather than handling an overflow the syscall layer already excluded.
 */
static uint64_t
build_user_arg_stack(uint64_t kva_base, int argc, char *const *argv)
{
	uint64_t	argv_uva[SPAWN_ARGV_MAX];
	uint64_t	sp;
	uint64_t	rsp;
	uint64_t	slot;
	uint8_t		*dst;
	const char	*s;
	size_t		len;
	size_t		j;
	int		i;

	if (argc < 0)
		argc = 0;
	if (argc > SPAWN_ARGV_MAX)
		argc = SPAWN_ARGV_MAX;

	/* Pack the argument strings downward from the top of the page. */
	sp = USER_STACK_TOP;
	for (i = 0; i < argc; i++) {
		s = argv[i];
		len = spawn_strlen(s) + 1;	/* include the NUL */
		sp -= len;
		dst = (uint8_t *)(uintptr_t)(kva_base + (sp - USER_STACK_VA));
		for (j = 0; j < len; j++)
			dst[j] = (uint8_t)s[j];
		argv_uva[i] = sp;
	}

	/*
	 * Reserve the argc slot + (argc+1) pointer slots below the strings,
	 * then align the argc slot down to 16 bytes (the SysV entry
	 * invariant).  Aligning down only widens the unused gap up to the
	 * strings, never overruns them.
	 */
	rsp = sp - (uint64_t)(8 * (argc + 2));
	rsp &= ~(uint64_t)15;

	KASSERT(rsp >= USER_STACK_VA,
	    "build_user_arg_stack: arg block underflows the user stack page");

	*(uint64_t *)(uintptr_t)(kva_base + (rsp - USER_STACK_VA)) =
	    (uint64_t)argc;
	for (i = 0; i < argc; i++) {
		slot = rsp + 8 + (uint64_t)(8 * i);
		*(uint64_t *)(uintptr_t)(kva_base + (slot - USER_STACK_VA)) =
		    argv_uva[i];
	}
	slot = rsp + 8 + (uint64_t)(8 * argc);
	*(uint64_t *)(uintptr_t)(kva_base + (slot - USER_STACK_VA)) = 0;

	return (rsp);
}

/*
 * Materialise the dyld handoff frame for a dynamically-linked Darwin image.
 * Mirrors the dyld4 _dyld_start contract: %rsp points at the main image's
 * mach_header, immediately followed by the SysV argument vector, then empty
 * envp[] and apple[] terminators:
 *
 *	[ main mach_header ]	<- returned user %rsp (16-byte aligned)
 *	[ argc ]
 *	[ argv[0] ... ]
 *	[ NULL ]		argv terminator
 *	[ NULL ]		envp (empty)
 *	[ NULL ]		apple (empty)
 *	[ (alignment gap) ]
 *	[ packed strings ]	<- top of the page
 *
 * Our dyld reads main_mh at [rsp], walks its chained fixups, and jumps to the
 * main LC_MAIN entry.  The string packing is identical to build_user_arg_stack;
 * only the leading mach_header word and the trailing envp/apple terminators
 * differ.
 */
static uint64_t
build_dyld_arg_stack(uint64_t kva_base, uint64_t main_mh, uint64_t stack_top,
    int argc, char *const *argv)
{
	uint64_t	argv_uva[SPAWN_ARGV_MAX];
	uint64_t	rsp;
	uint64_t	slot;
	uint64_t	sp;
	uint64_t	top_base;
	uint8_t		*dst;
	const char	*s;
	size_t		j;
	size_t		len;
	int		i;
	int		nwords;

	if (argc < 0)
		argc = 0;
	if (argc > SPAWN_ARGV_MAX)
		argc = SPAWN_ARGV_MAX;

	/*
	 * `kva_base` aliases the TOP page of the stack -- [top_base, stack_top)
	 * -- so every store is addressed relative to top_base.  The whole
	 * handoff frame (strings + pointer block) lives in that one page; the
	 * SPAWN_* caps keep it well under 4 KiB, which the KASSERT documents.
	 */
	top_base = stack_top - 0x1000;

	/* Pack the argument strings downward from the top of the stack. */
	sp = stack_top;
	for (i = 0; i < argc; i++) {
		s = argv[i];
		len = spawn_strlen(s) + 1;	/* include the NUL */
		sp -= len;
		dst = (uint8_t *)(uintptr_t)(kva_base + (sp - top_base));
		for (j = 0; j < len; j++)
			dst[j] = (uint8_t)s[j];
		argv_uva[i] = sp;
	}

	/*
	 * Reserve the handoff block below the strings:
	 *	mach_header + argc + argv[argc] + argv NULL + envp NULL + apple NULL
	 * = argc + 5 quadwords.  Align the base (where %rsp lands) down to 16.
	 */
	nwords = argc + 5;
	rsp = sp - (uint64_t)(8 * nwords);
	rsp &= ~(uint64_t)15;

	KASSERT(rsp >= top_base,
	    "build_dyld_arg_stack: handoff block underflows the stack top page");

	slot = rsp;
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) = main_mh;
	slot += 8;
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) =
	    (uint64_t)argc;
	slot += 8;
	for (i = 0; i < argc; i++) {
		*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) =
		    argv_uva[i];
		slot += 8;
	}
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) = 0; /* argv  */
	slot += 8;
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) = 0; /* envp  */
	slot += 8;
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) = 0; /* apple */

	return (rsp);
}

/*
 * Launcher for a ring-3 program shipped as an embedded image.  Differs
 * from usermode_launcher in that the format loader handles segment
 * mapping and picks the entry RIP off the image header; we only own the
 * stack mapping.  The image is format-sniffed on its first four bytes:
 * an ELF magic routes to elf_load, a Mach-O (thin MH_MAGIC_64 or a
 * fat/universal archive) routes to macho_load.  Both loaders share the
 * (task, image, size, &entry) contract, so everything downstream of the
 * sniff -- stack frame, port injection, ring-3 transition -- is identical
 * regardless of container format.
 *
 * Runs as a kernel thread attached to the freshly-created user task, so
 * by the time we get here the scheduler has already loaded our task's
 * CR3 -- the loader's pmap_enter calls land in (and TLB-flush) the live
 * page table.
 */
static void
usermode_elf_launcher(void *arg)
{
	struct user_spawn_arg	*sa;
	struct task		*ut;
	uint64_t		*kva;
	uint64_t		 entry;
	uint64_t		 ksp;
	uint64_t		 main_base;
	uint64_t		 stack_pa;
	uint64_t		 stack_top;
	uint64_t		 stack_va;
	uint64_t		 top_pa;
	uint64_t		 user_rsp;
	size_t			 i;
	size_t			 npages;
	size_t			 p;
	uint32_t		 magic;
	int			 rv;
	bool			 needs_dyld;

	sa = (struct user_spawn_arg *)arg;
	ut = current_thread->th_task;

	needs_dyld = false;
	main_base  = 0;

	magic = sa->sa_image_size >= sizeof(uint32_t) ?
	    *(const uint32_t *)sa->sa_image : 0;
	if (magic == MACHO_MAGIC_64 || magic == MACHO_FAT_MAGIC ||
	    magic == MACHO_FAT_CIGAM) {
		struct macho_load_result	mres;

		rv = macho_load(ut, sa->sa_image, sa->sa_image_size, &mres);
		if (rv != MACHO_E_OK)
			panic("usermode_elf_launcher: macho_load %s rv=%d",
			    sa->sa_name, rv);
		entry      = mres.entry;
		needs_dyld = mres.needs_dyld;
		main_base  = mres.image_base;
	} else {
		rv = elf_load(ut, sa->sa_image, sa->sa_image_size, &entry);
		if (rv != ELF_E_OK)
			panic("usermode_elf_launcher: elf_load %s rv=%d",
			    sa->sa_name, rv);
	}

	/*
	 * Map the initial user stack.  A dynamically-linked Darwin image gets a
	 * multi-page stack (DARWIN_STACK_PAGES) growing down from
	 * DARWIN_STACK_TOP -- real Apple binaries build large and
	 * variable-length frames (probed by ____chkstk_darwin) that a single
	 * page would overflow.  Every other image keeps the historical single
	 * page at USER_STACK_VA.  Pages are allocated one at a time (the pmm is
	 * page-granular and they need not be contiguous); the handoff frame is
	 * built entirely within the TOP page, whose kernel alias `kva` the frame
	 * builders write through.
	 */
	if (needs_dyld) {
		stack_top = DARWIN_STACK_TOP;
		npages    = DARWIN_STACK_PAGES;
	} else {
		stack_top = USER_STACK_TOP;
		npages    = 1;
	}
	stack_va = stack_top - (uint64_t)npages * 0x1000;

	top_pa = PA_INVALID;
	for (p = 0; p < npages; p++) {
		uint64_t	page_va;

		page_va  = stack_top - (uint64_t)(p + 1) * 0x1000;
		stack_pa = pmm_alloc_page();
		if (stack_pa == PA_INVALID)
			panic("usermode_elf_launcher: stack alloc failed");
		if (!pmap_enter(ut->t_pmap, page_va, stack_pa,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER))
			panic("usermode_elf_launcher: stack map failed");
		kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
		for (i = 0; i < 512; i++)
			kva[i] = 0;
		if (p == 0)
			top_pa = stack_pa;	/* TOP page -- carries the frame */
	}
	if (!vm_map_enter(ut->t_map, stack_va, (uint64_t)npages * 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER, VME_F_ANON))
		panic("usermode_elf_launcher: vm_map_enter stack");

	kva = (uint64_t *)pmm_kva_from_pa(top_pa);

	/*
	 * Resolve the entry RIP and the initial stack.  A dynamically-linked
	 * Darwin image (LC_LOAD_DYLINKER present) is not entered directly: the
	 * kernel maps our clean-room dyld alongside it and enters dyld with a
	 * dyld4-shaped handoff stack carrying the main image's mach_header.
	 * dyld binds the image, then jumps to its LC_MAIN entry.  Every other
	 * image -- a style9 ELF, or a non-dynamic Mach-O -- takes the ordinary
	 * SysV argc/argv frame and is entered at its own RIP.
	 */
	if (needs_dyld) {
		struct macho_load_result	dres;
		const uint8_t			*dyld_img;
		size_t				 dyld_sz;

		dyld_img = _binary_dyld_macho_start;
		dyld_sz  = (size_t)(_binary_dyld_macho_end -
		    _binary_dyld_macho_start);
		rv = macho_load(ut, dyld_img, dyld_sz, &dres);
		if (rv != MACHO_E_OK)
			panic("usermode_elf_launcher: dyld load failed rv=%d", rv);
		entry    = dres.entry;
		user_rsp = build_dyld_arg_stack((uint64_t)kva, main_base,
		    stack_top, sa->sa_argc, sa->sa_argv);
	} else {
		user_rsp = build_user_arg_stack((uint64_t)kva, sa->sa_argc,
		    sa->sa_argv);
	}

	ksp = (uint64_t)current_thread->th_kstack_base +
	    current_thread->th_kstack_size;
	tss_set_rsp0(ksp);
	syscall_kernel_rsp = ksp;

	/*
	 * Inject the parent's port into the child's port_space at the
	 * well-known MACH_PORT_PARENT slot before transitioning to
	 * ring 3.  Slots 1 (task_self) and 2 (bootstrap) were filled by
	 * task_create, so the next-free slot is by construction
	 * MACH_PORT_PARENT == 3; space_install_no_ref consumes the SEND
	 * ref the parent already held, so no extra port_ref here.
	 */
	if (sa->sa_inject_port != NULL) {
		mach_port_name_t	pname;
		int			pr;

		pr = space_install_no_ref(ut->t_port_space,
		    sa->sa_inject_port, MACH_PORT_RIGHT_SEND, &pname);
		if (pr != MACH_MSG_OK)
			panic("usermode_elf_launcher: inject install rv=%d", pr);
		if (pname != MACH_PORT_PARENT)
			panic("usermode_elf_launcher: inject name %u, "
			    "expected %u (parent port_space pre-populated?)",
			    (unsigned)pname, (unsigned)MACH_PORT_PARENT);
	}

	kprintf("usermode: spawn '%s' entry=0x%llx (image=%zu bytes), "
	    "rsp=0x%llx argc=%d%s%s\n",
	    sa->sa_name, (unsigned long long)entry, sa->sa_image_size,
	    (unsigned long long)user_rsp, sa->sa_argc,
	    sa->sa_inject_port != NULL ? ", parent port injected" : "",
	    needs_dyld ? ", via dyld" : "");

	if (sa->sa_argv != NULL)
		kfree(sa->sa_argv);
	kfree(sa);
	usermode_enter(entry, user_rsp);
}

/*
 * Kernel-side trampoline for the very first ring-3 program.  Runs as
 * a normal kernel thread; once the user pages are mapped + the blob
 * copied in, jumps to ring 3 with iretq.
 *
 * After iretq, the thread continues to live (the user code runs on
 * its kernel stack only when a syscall pulls it back via syscall_entry),
 * and on SYS_EXIT the kernel-side sys_exit path calls thread_exit
 * which reaps it.
 */
static void
usermode_launcher(void *arg)
{
	struct task	*ut;
	uint64_t	*kva;
	uint64_t	 code_pa;
	uint64_t	 stack_pa;
	size_t		 blob_len;
	size_t		 i;
	uint8_t		*src;
	uint8_t		*dst;

	(void)arg;

	ut = current_thread->th_task;

	code_pa  = pmm_alloc_page();
	stack_pa = pmm_alloc_page();
	if (code_pa == PA_INVALID || stack_pa == PA_INVALID)
		panic("usermode_launcher: pmm out of pages");

	if (!pmap_enter(ut->t_pmap, USER_CODE_VA, code_pa,
	    VM_PROT_READ | VM_PROT_EXEC | VM_PROT_USER))
		panic("usermode_launcher: code map failed");
	if (!pmap_enter(ut->t_pmap, USER_STACK_VA, stack_pa,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER))
		panic("usermode_launcher: stack map failed");

	if (!vm_map_enter(ut->t_map, USER_CODE_VA, 0x1000,
	    VM_PROT_READ | VM_PROT_EXEC | VM_PROT_USER, VME_F_ANON))
		panic("usermode_launcher: vm_map_enter code");
	if (!vm_map_enter(ut->t_map, USER_STACK_VA, 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER, VME_F_ANON))
		panic("usermode_launcher: vm_map_enter stack");

	/*
	 * Copy the blob into the user code page.  We touch it via the
	 * kernel-VA alias of the freshly-allocated frame (pmm_kva_from_pa)
	 * rather than USER_CODE_VA, so we do not depend on the leaf
	 * being writable from kernel side; the leaf itself is mapped
	 * read+execute for ring 3.
	 */
	blob_len = (size_t)(user_blob_end - user_blob_start);
	if (blob_len > 0x1000u)
		panic("usermode_launcher: blob > one page");

	src = user_blob_start;
	dst = (uint8_t *)pmm_kva_from_pa(code_pa);
	for (i = 0; i < blob_len; i++)
		dst[i] = src[i];

	/*
	 * Zero the user stack page so the first %rsp read is clean.
	 * (No callee-saved registers to restore on entry, but a
	 * downstream backtrace would land in garbage otherwise.)
	 */
	kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
	for (i = 0; i < 512; i++)
		kva[i] = 0;

	/*
	 * Park the kernel-stack top for both the IRQ-ring-transition
	 * path (TSS.rsp0) and the syscall fast path
	 * (syscall_kernel_rsp).  Both ultimately need the same value;
	 * keeping the syscall path in its own MSR-adjacent global is
	 * deliberate because syscall does not go through the TSS.
	 */
	{
		uint64_t	ksp;

		ksp = (uint64_t)current_thread->th_kstack_base +
		    current_thread->th_kstack_size;
		tss_set_rsp0(ksp);
		syscall_kernel_rsp = ksp;
	}

	kprintf("usermode: entering ring 3 (rip=0x%llx rsp=0x%llx, "
	    "blob=%zu bytes, task=%s)\n",
	    (unsigned long long)USER_CODE_VA,
	    (unsigned long long)USER_STACK_TOP,
	    blob_len, ut->t_name);

	usermode_enter(USER_CODE_VA, USER_STACK_TOP);
}

/*
 * Synthesise a ring-0 -> ring-3 iretq.  The CPU pops, in order:
 *	%rip, %cs, %rflags, %rsp, %ss
 * and atomically switches DPL.
 *
 *	%cs = 0x28 | 3 = 0x2B		user code64
 *	%ss = 0x20 | 3 = 0x23		user data
 *	%rflags = 0x202			IF=1, MBS=1
 */
__attribute__((noreturn))
void
usermode_enter(uint64_t user_rip, uint64_t user_rsp)
{

	__asm__ __volatile__ (
	    "pushq $0x23		\n"	/* SS               */
	    "pushq %0			\n"	/* RSP              */
	    "pushq $0x202		\n"	/* RFLAGS           */
	    "pushq $0x2B		\n"	/* CS               */
	    "pushq %1			\n"	/* RIP              */
	    "iretq			\n"
	    :
	    : "r"(user_rsp), "r"(user_rip)
	    : "memory");

	__builtin_unreachable();
}
