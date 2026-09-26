/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "darwin.h"
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
 * usermode_setup_image maps it beside any image that carries an
 * LC_LOAD_DYLINKER and enters through it.  Not a progreg program: dyld is
 * never spawned by name.
 */
extern uint8_t	_binary_dyld_macho_start[];
extern uint8_t	_binary_dyld_macho_end[];

/*
 * The first ring-3 program: the asm in arch/amd64/user_blob.S, which makes
 * SYS_PRINT and SYS_EXIT.  Nothing calls usermode_run_first_blob now; it is
 * kept as a minimal no-libc ring-3 entry.
 */
static void	usermode_launcher(void *) __attribute__((noreturn));

/*
 * A spawn request carried to the launcher thread.  Allocated by
 * arch_spawn_user, freed by the launcher before its iretq, so a program
 * that never exits does not leak it.
 *
 * sa_inject_port, if non-NULL, carries one SEND ref the caller took; the
 * launcher moves it into the child's space at MACH_PORT_PARENT
 * (space_install_no_ref).
 *
 * sa_argv, if non-NULL, is a kernel-owned flattened block (sa_argc char *
 * slots pointing into the packed strings that follow).  The launcher copies
 * it onto the child's stack and kfrees it; on failure arch_spawn_user does.
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
 * Spawn a user program, for the progreg_spawn* family (which found the
 * embedded image in the program registry).  Builds a task and starts a
 * launcher thread with a heap-allocated user_spawn_arg, which outlives
 * this call and is freed by the launcher.  Returns the new task's id or a
 * negative SYS_E_*.
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

	sa = kmalloc(sizeof(*sa));
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
	 * Optional: a SEND right on the new task's task-self port in the
	 * caller's space, name written back (SYS_SPAWN_RETURNS_TASKPORT --
	 * the right SYS_TASK_KILL needs).  Before thread_create, so a
	 * failure can still task_deref the thread-less task (t_refs 1) to
	 * zero and free it.
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
			 * Roll back the taskport install, or the caller
			 * keeps a name for the port of a task about to be
			 * freed.
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
	 * Drop the creator's ref; the thread anchors the task now.
	 * task_create returns t_refs 1 and task_attach_thread makes it 2,
	 * so without this the exiting thread's task_detach_thread would
	 * stop at 1 and the dead task would stay in task_list, alive to
	 * SYS_TASK_ALIVE, for ever.  Safe once the thread is enqueued:
	 * task_deref frees only at t_refs 0 with t_nthreads 0 (KASSERT'd).
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
 * crt0.S reads argc at %rsp and argv at %rsp+8.  argc == 0 still lays
 * down a valid frame, so every program enters the same way.  The progreg.h
 * caps (SPAWN_ARGV_MAX / SPAWN_ARG_BYTES_MAX) guarantee the block fits the
 * page; the KASSERT states that invariant rather than handling overflow.
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
 * Materialise the dyld handoff frame for a dynamically-linked Darwin image,
 * per dyld4's _dyld_start contract: the main image's mach_header at %rsp,
 * then the SysV argument vector, envp[] and an empty apple[]:
 *
 *	[ main mach_header ]	<- returned user %rsp (16-byte aligned)
 *	[ argc ]
 *	[ argv[0] ... ]
 *	[ NULL ]		argv terminator
 *	[ envp[0] ... ]
 *	[ NULL ]		envp terminator
 *	[ NULL ]		apple (empty)
 *	[ (alignment gap) ]
 *	[ envp strings ]
 *	[ argv strings ]	<- stack_top
 *
 * Our dyld reads main_mh at [rsp], applies its chained fixups and jumps to
 * the LC_MAIN entry.  Returns 0 if the frame does not fit the top page.
 */
static uint64_t
build_dyld_arg_stack(uint64_t kva_base, uint64_t main_mh, uint64_t stack_top,
    int argc, char *const *argv, int envc, char *const *envp)
{
	uint64_t	argv_uva[SPAWN_ARGV_MAX];
	uint64_t	envp_uva[SPAWN_ENV_MAX];
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
	if (envc < 0 || envp == NULL)
		envc = 0;
	if (envc > SPAWN_ENV_MAX)
		envc = SPAWN_ENV_MAX;

	/*
	 * `kva_base' aliases the top page of the stack, [top_base, stack_top),
	 * so every store is relative to top_base.  The whole frame, strings
	 * and pointers, lives in that page.
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
	 * Environment strings go below argv's, in the same page.  The caller
	 * chooses their size, so the fit is checked rather than asserted: a
	 * frame that does not fit is a refused execve, not a panic.
	 */
	for (i = 0; i < envc; i++) {
		s = envp[i];
		len = spawn_strlen(s) + 1;
		if (sp - top_base < len)
			return (0);
		sp -= len;
		dst = (uint8_t *)(uintptr_t)(kva_base + (sp - top_base));
		for (j = 0; j < len; j++)
			dst[j] = (uint8_t)s[j];
		envp_uva[i] = sp;
	}

	/*
	 * Reserve the handoff block below the strings:
	 *	mach_header + argc + argv[argc] + argv NULL
	 *	+ envp[envc] + envp NULL + apple NULL
	 * = argc + envc + 5 quadwords.  Align the base (where %rsp lands)
	 * down to 16.
	 */
	nwords = argc + envc + 5;
	if (sp - top_base < (uint64_t)(8 * nwords) + 16)
		return (0);
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
	for (i = 0; i < envc; i++) {
		*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) =
		    envp_uva[i];
		slot += 8;
	}
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) = 0; /* envp  */
	slot += 8;
	*(uint64_t *)(uintptr_t)(kva_base + (slot - top_base)) = 0; /* apple */

	return (rsp);
}

/*
 * usermode_setup_image: build a user image in `ut', for both the spawn
 * launcher and Darwin execve(2).  The first four bytes pick the loader
 * (thin or fat Mach-O -> macho_load, anything else -> elf_load); then the
 * initial stack is mapped, the entry frame built, and the ring-3 rip/rsp
 * returned through the out parameters.
 *
 * A dynamically-linked Darwin image (LC_LOAD_DYLINKER) is entered through
 * our dyld, mapped beside it, with a dyld4-shaped handoff frame
 * (build_dyld_arg_stack) on a stack of DARWIN_STACK_PAGES below
 * DARWIN_STACK_TOP -- real Apple binaries overflow one page.  Any other
 * image gets one page at USER_STACK_VA and the SysV argc/argv frame.
 *
 * Returns 0 or a negative SYS_E_*.  On failure the user address space may
 * be partly populated; the caller deals with that (the spawn launcher
 * panics, execve exits the task).
 */
static long
usermode_setup_image(struct task *ut, const uint8_t *image,
    size_t image_size, const char *name, int argc, char *const *argv,
    int envc, char *const *envp, uint64_t *rip_out, uint64_t *rsp_out)
{
	uint64_t	*kva;
	uint64_t	 entry;
	uint64_t	 main_base;
	uint64_t	 stack_pa;
	uint64_t	 stack_top;
	uint64_t	 stack_va;
	uint64_t	 top_pa;
	uint64_t	 user_rsp;
	size_t		 i;
	size_t		 npages;
	size_t		 p;
	uint32_t	 magic;
	int		 rv;
	bool		 needs_dyld;

	needs_dyld = false;
	main_base  = 0;
	entry      = 0;

	magic = image_size >= sizeof(uint32_t) ?
	    *(const uint32_t *)image : 0;
	if (magic == MACHO_MAGIC_64 || magic == MACHO_FAT_MAGIC ||
	    magic == MACHO_FAT_CIGAM) {
		struct macho_load_result	mres;

		rv = macho_load(ut, image, image_size, &mres);
		if (rv != MACHO_E_OK) {
			kprintf("usermode: macho_load %s rv=%d\n", name, rv);
			return (SYS_E_INVAL);
		}
		entry      = mres.entry;
		needs_dyld = mres.needs_dyld;
		main_base  = mres.image_base;
	} else {
		rv = elf_load(ut, image, image_size, &entry);
		if (rv != ELF_E_OK) {
			kprintf("usermode: elf_load %s rv=%d\n", name, rv);
			return (SYS_E_INVAL);
		}
	}

	/*
	 * Map the initial stack a page at a time; the pages need not be
	 * contiguous.  The frame lives in the top page, written through its
	 * kernel alias `kva'.
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
			return (SYS_E_NOMEM);
		if (!pmap_enter(ut->t_pmap, page_va, stack_pa,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER)) {
			pmm_free_page(stack_pa);
			return (SYS_E_NOMEM);
		}
		kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
		for (i = 0; i < 512; i++)
			kva[i] = 0;
		if (p == 0)
			top_pa = stack_pa;	/* top page, holds the frame */
	}
	if (!vm_map_enter(ut->t_map, stack_va, (uint64_t)npages * 0x1000,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER, VME_F_ANON))
		return (SYS_E_NOMEM);

	kva = (uint64_t *)pmm_kva_from_pa(top_pa);

	if (needs_dyld) {
		struct macho_load_result	dres;
		const uint8_t			*dyld_img;
		size_t				 dyld_sz;

		dyld_img = _binary_dyld_macho_start;
		dyld_sz  = (size_t)(_binary_dyld_macho_end -
		    _binary_dyld_macho_start);
		rv = macho_load(ut, dyld_img, dyld_sz, &dres);
		if (rv != MACHO_E_OK) {
			kprintf("usermode: dyld load failed rv=%d\n", rv);
			return (SYS_E_INVAL);
		}
		entry    = dres.entry;
		user_rsp = build_dyld_arg_stack((uint64_t)kva, main_base,
		    stack_top, argc, argv, envc, envp);
		if (user_rsp == 0) {
			kprintf("usermode: %s: argv + envp do not fit the "
			    "handoff page\n", name);
			return (SYS_E_INVAL);
		}
	} else {
		user_rsp = build_user_arg_stack((uint64_t)kva, argc, argv);
	}

	*rip_out = entry;
	*rsp_out = user_rsp;
	return (0);
}

/*
 * Launcher thread for a spawned program.  usermode_setup_image loads the
 * image and builds the stack; this registers the kstack, injects the
 * parent port and drops to ring 3.  It runs in the new task, so the
 * scheduler has already loaded the task's CR3 and the loader's pmap_enter
 * calls hit the live tables.
 */
static void
usermode_elf_launcher(void *arg)
{
	struct user_spawn_arg	*sa;
	struct task		*ut;
	uint64_t		 entry;
	uint64_t		 ksp;
	uint64_t		 user_rsp;
	long			 rv;

	sa = (struct user_spawn_arg *)arg;
	ut = current_thread->th_task;

	rv = usermode_setup_image(ut, sa->sa_image, sa->sa_image_size,
	    sa->sa_name, sa->sa_argc, sa->sa_argv, 0, NULL, &entry,
	    &user_rsp);
	if (rv < 0)
		panic("usermode_elf_launcher: setup_image %s rv=%ld",
		    sa->sa_name, rv);

	ksp = (uint64_t)current_thread->th_kstack_base +
	    current_thread->th_kstack_size;
	tss_set_rsp0(ksp);
	cpu_set_kernel_rsp(ksp);

	/*
	 * task_create filled names 1 (task_self) and 2 (bootstrap), so the
	 * next free one is MACH_PORT_PARENT (3).  space_install_no_ref
	 * consumes the SEND ref the parent already took.
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
	    "rsp=0x%llx argc=%d%s\n",
	    sa->sa_name, (unsigned long long)entry, sa->sa_image_size,
	    (unsigned long long)user_rsp, sa->sa_argc,
	    sa->sa_inject_port != NULL ? ", parent port injected" : "");

	if (sa->sa_argv != NULL)
		kfree(sa->sa_argv);
	kfree(sa);
	usermode_enter(entry, user_rsp);
}

/*
 * Launcher thread for the user_blob program: map a code and a stack page,
 * copy the blob in and iretq to it.  The thread lives on as the program's;
 * SYS_EXIT ends it through thread_exit.
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
	 * Copy through the frame's kernel alias, not USER_CODE_VA, whose
	 * leaf is read+execute only.
	 */
	blob_len = (size_t)(user_blob_end - user_blob_start);
	if (blob_len > 0x1000u)
		panic("usermode_launcher: blob > one page");

	src = user_blob_start;
	dst = (uint8_t *)pmm_kva_from_pa(code_pa);
	for (i = 0; i < blob_len; i++)
		dst[i] = src[i];

	/* Zero the stack page so a backtrace does not walk garbage. */
	kva = (uint64_t *)pmm_kva_from_pa(stack_pa);
	for (i = 0; i < 512; i++)
		kva[i] = 0;

	/*
	 * The kstack top, for interrupts from ring 3 (this CPU's
	 * TSS.rsp0) and for SYSCALL (cp_kernel_rsp): SYSCALL switches no
	 * stack and never reads the TSS, so the stub needs its own copy.
	 */
	{
		uint64_t	ksp;

		ksp = (uint64_t)current_thread->th_kstack_base +
		    current_thread->th_kstack_size;
		tss_set_rsp0(ksp);
		cpu_set_kernel_rsp(ksp);
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

/*
 * Ring-3 entry for a fork(2) child: usermode_enter with the GPRs zeroed.
 * The child resumes after the parent's `syscall' with %rax = 0.  The
 * libSystem _fork wrapper (user/libsystem.c) pushes the six callee-saved
 * registers on the user stack, which the child inherits copy-on-write,
 * and pops them after the syscall in both processes; the rest are
 * caller-saved, so zeroing them leaks nothing and breaks nothing.
 */
__attribute__((noreturn))
static void
usermode_enter_forked(uint64_t user_rip, uint64_t user_rsp)
{

	__asm__ __volatile__ (
	    "pushq $0x23		\n"	/* SS               */
	    "pushq %0			\n"	/* RSP              */
	    "pushq $0x202		\n"	/* RFLAGS           */
	    "pushq $0x2B		\n"	/* CS               */
	    "pushq %1			\n"	/* RIP              */
	    "xorl %%eax, %%eax		\n"	/* fork() -> 0      */
	    "xorl %%ebx, %%ebx		\n"
	    "xorl %%ecx, %%ecx		\n"
	    "xorl %%edx, %%edx		\n"
	    "xorl %%esi, %%esi		\n"
	    "xorl %%edi, %%edi		\n"
	    "xorl %%ebp, %%ebp		\n"
	    "xorl %%r8d, %%r8d		\n"
	    "xorl %%r9d, %%r9d		\n"
	    "xorl %%r10d, %%r10d	\n"
	    "xorl %%r11d, %%r11d	\n"
	    "xorl %%r12d, %%r12d	\n"
	    "xorl %%r13d, %%r13d	\n"
	    "xorl %%r14d, %%r14d	\n"
	    "xorl %%r15d, %%r15d	\n"
	    "iretq			\n"
	    :
	    : "r"(user_rsp), "r"(user_rip)
	    : "memory");

	__builtin_unreachable();
}

/*
 * The parent's user rip/rsp, carried to the fork child's launcher.
 * Allocated by arch_darwin_fork, freed by the launcher before its iretq.
 */
struct darwin_fork_arg {
	uint64_t	fa_rip;
	uint64_t	fa_rsp;
};

/*
 * Launcher thread for a fork child, the child task's only thread.  The
 * address space was duplicated before thread_start; this registers the
 * kstack for syscall/IRQ entry and drops to ring 3 at the parent's rip/rsp.
 */
static void
darwin_fork_child_launcher(void *arg)
{
	struct darwin_fork_arg	*fa;
	uint64_t		 ksp;
	uint64_t		 rip;
	uint64_t		 rsp;

	fa  = (struct darwin_fork_arg *)arg;
	rip = fa->fa_rip;
	rsp = fa->fa_rsp;
	kfree(fa);

	ksp = (uint64_t)current_thread->th_kstack_base +
	    current_thread->th_kstack_size;
	tss_set_rsp0(ksp);
	cpu_set_kernel_rsp(ksp);

	usermode_enter_forked(rip, rsp);
}

/*
 * arch_darwin_fork: the fork(2) engine.  Clones the calling Darwin task --
 * copy-on-write address space (vm_map_fork_share), file table
 * (darwin_files_fork_copy), dylib bump pointer, parentage -- and starts a
 * thread that enters ring 3 at the parent's user rip/rsp with %rax = 0.
 * Returns the child's pid (its task id) or a negative SYS_E_*.  A failure
 * task_derefs the half-built child, whose teardown reclaims the rest.
 */
long
arch_darwin_fork(struct syscall_frame *f)
{
	struct darwin_fork_arg	*fa;
	struct task		*child;
	struct task		*parent;
	struct thread		*th;
	size_t			 si;
	long			 pid;

	parent = current_thread->th_task;

	fa = kmalloc(sizeof(*fa));
	if (fa == NULL)
		return (SYS_E_NOMEM);
	fa->fa_rip = f->sf_user_rip;
	fa->fa_rsp = f->sf_user_rsp;

	child = task_create(parent->t_name);
	if (child == NULL) {
		kfree(fa);
		return (SYS_E_NOMEM);
	}

	/*
	 * Identity first: the child's first syscall dispatches on
	 * t_personality, and wait4/getppid key on t_darwin_ppid.
	 */
	child->t_personality       = TASK_PERSONALITY_DARWIN;
	child->t_darwin_ppid       = parent->t_id;
	child->t_darwin_dylib_next = parent->t_darwin_dylib_next;

	/*
	 * The working directory and umask are inherited (POSIX); a shell's
	 * `cd /etc && ls' runs ls in a forked child.
	 */
	for (si = 0; si < DARWIN_PATH_MAX; si++)
		child->t_darwin_cwd[si] = parent->t_darwin_cwd[si];
	child->t_darwin_umask = parent->t_darwin_umask;

	/*
	 * Signal dispositions and the blocked mask are inherited (POSIX).
	 * The handler and trampoline VAs stay valid because the address
	 * space is duplicated below; execve is what resets them to SIG_DFL.
	 */
	for (si = 0; si < DARWIN_NSIG; si++)
		child->t_sig_handler[si] = parent->t_sig_handler[si];
	child->t_sig_tramp = parent->t_sig_tramp;
	child->t_sig_mask  = parent->t_sig_mask;

	if (!vm_map_fork_share(parent->t_map, parent->t_pmap,
	    child->t_map, child->t_pmap)) {
		task_deref(child);
		kfree(fa);
		return (SYS_E_NOMEM);
	}

	if (darwin_files_fork_copy(parent, child) != 0) {
		task_deref(child);
		kfree(fa);
		return (SYS_E_NOMEM);
	}

	th = thread_create(child, darwin_fork_child_launcher, fa, "fork");
	if (th == NULL) {
		task_deref(child);
		kfree(fa);
		return (SYS_E_NOMEM);
	}
	pid = (long)child->t_id;
	thread_start(th);

	/* Drop the creator's ref, as in arch_spawn_user. */
	task_deref(child);
	return (pid);
}

/*
 * arch_darwin_execve: replace the calling task's user image.  The caller
 * (kern/darwin.c) has done the validation (registry lookup, argv copyin);
 * from here on the old image is gone.
 *
 * Teardown mirrors task death -- release the anonymous frames, drop every
 * map entry -- but keeps the task, its fd table, its port space and this
 * thread.  Unmapping the user half from here is safe: this runs on the
 * kernel stack and the kernel half is untouched.  usermode_setup_image
 * then builds the new image; syscall_entry.S restores user rip/rsp/rflags
 * from the frame, so rewriting it and returning 0 is the jump.
 *
 * A setup failure has nothing to return to: the task exits with wait4
 * status 127, the shell's "command could not be run".
 */
long
arch_darwin_execve(const unsigned char *image, unsigned long image_size,
    int argc, char **argv, int envc, char **envp, struct syscall_frame *f)
{
	struct task	*t;
	uint64_t	 rip;
	uint64_t	 rsp;
	long		 rv;

	t = current_thread->th_task;

	vm_map_release_anon(t->t_map, t->t_pmap);
	vm_map_reset(t->t_map);
	t->t_darwin_dylib_next = 0;

	rv = usermode_setup_image(t, image, (size_t)image_size, t->t_name,
	    argc, argv, envc, envp, &rip, &rsp);
	if (rv < 0) {
		kprintf("darwin: execve setup failed rv=%ld, task exits\n",
		    rv);
		darwin_zombie_record(t->t_id, t->t_darwin_ppid, 127 << 8);
		thread_exit();
		/* NOTREACHED */
	}

	f->sf_user_rip    = rip;
	f->sf_user_rsp    = rsp;
	f->sf_user_rflags = 0x202;
	f->sf_arg0 = 0;
	f->sf_arg1 = 0;
	f->sf_arg2 = 0;
	f->sf_arg3 = 0;
	f->sf_arg4 = 0;
	f->sf_arg5 = 0;
	return (0);
}
