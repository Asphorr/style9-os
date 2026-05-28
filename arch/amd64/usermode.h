/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_USERMODE_H_
#define	_MACHINE_USERMODE_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Ring-3 bring-up.
 *
 * usermode_run_first_blob() spawns a kernel thread that:
 *	- allocates user code + stack pages and maps them U=1,
 *	- copies the inline user_blob (arch/amd64/user_blob.S) into
 *	  the code page,
 *	- installs the per-thread kernel-stack top in the TSS and
 *	  in syscall_kernel_rsp,
 *	- iretq's to the user RIP with user CS/SS/RSP.
 *
 * The blob makes two syscalls (SYS_PRINT, SYS_EXIT) and exits.  The
 * kernel will reap the thread on the next idle pass.
 */

#define	USER_CODE_VA	0x40000000ULL	/* just past the 1 GiB boot map */
#define	USER_STACK_VA	0x4000F000ULL	/* high in the first user page run */
#define	USER_STACK_TOP	(USER_STACK_VA + 0x1000ULL)

void	usermode_run_first_blob(void);

/*
 * arch_spawn_user: create a task, load the named ELF blob into it via
 * the program registry's image pointer, and start a user thread that
 * iretq's into ring 3 on e_entry.  Wired up by progreg_spawn() in
 * kern/progreg.c; ring-3 callers reach it via SYS_SPAWN.
 *
 * `inject_port` is optional: when non-NULL, the launcher installs a
 * SEND right on the port into the child's port_space at name
 * MACH_PORT_PARENT before transitioning to ring 3.  Caller must hold
 * one SEND ref; the ref is transferred into the child on success and
 * dropped on any failure path.  Used by SYS_SPAWN_WITH_PORT to hand
 * a private channel to a child task.
 *
 * Returns the new task's t_id on success, negative SYS_E_* on failure.
 */
struct port;
long	arch_spawn_user(const char *name, const uint8_t *image,
	    size_t image_size, struct port *inject_port);

/*
 * usermode_enter: never returns to its caller.  Pushes a synthetic
 * iretq frame (SS, RSP, RFLAGS, CS, RIP) for user-mode and iretq's.
 * Called from the launcher thread once the page mappings + RSP0
 * bookkeeping are in place.
 */
void	usermode_enter(uint64_t user_rip, uint64_t user_rsp)
	    __attribute__((noreturn));

#endif /* !_MACHINE_USERMODE_H_ */
