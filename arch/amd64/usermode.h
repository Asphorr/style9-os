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
 * usermode_run_first_blob() (currently uncalled) spawns a kernel thread
 * that maps a user code and stack page, copies in user_blob
 * (arch/amd64/user_blob.S), installs its kstack top in this CPU's TSS and
 * cp_kernel_rsp, and iretqs to it.  The blob makes SYS_PRINT and SYS_EXIT;
 * the idle thread reaps the rest.
 */

#define	USER_CODE_VA	0x40000000ULL	/* just past the 1 GiB boot map */

/*
 * The initial user stack, and so the ceiling on a native program's image:
 * text, data and bss must end below USER_STACK_VA, just under 16 MiB above
 * USER_CODE_VA.  Inside the user window [0x40000000, 0x80000000) and well
 * below the Darwin stack and main image, which meet at 0x50000000.
 * user/user.ld asserts the image fits, so outgrowing it is a link error,
 * not a panic when the stack mapping collides at spawn.
 */
#define	USER_STACK_VA	0x40FFF000ULL
#define	USER_STACK_TOP	(USER_STACK_VA + 0x1000ULL)

/*
 * A dynamically-linked Darwin image gets a bigger stack than a style9
 * ELF's one page: real Apple binaries build large frames and probe with
 * ____chkstk_darwin.  It grows down from just below the main image
 * (MACHO_IMAGE_BASE, 0x50000000), clear of it, dyld (0x60000000) and the
 * dylibs (0x70000000+).
 */
#define	DARWIN_STACK_TOP	0x50000000ULL
#define	DARWIN_STACK_PAGES	64		/* 256 KiB */

void	usermode_run_first_blob(void);

/*
 * arch_spawn_user: create a task, load the program registry's image (ELF
 * or Mach-O) into it, and start a thread that enters ring 3 at its entry.
 * Called by the progreg_spawn* family in kern/progreg.c (SYS_SPAWN and
 * friends).
 *
 * `inject_port', if non-NULL, carries one SEND ref the caller holds; the
 * launcher installs it in the child's space at MACH_PORT_PARENT, and any
 * failure drops it (SYS_SPAWN_WITH_PORT).
 *
 * `caller_space' and `out_taskport_name', if both non-NULL, get a SEND
 * right on the new task's task-self port and its name
 * (SYS_SPAWN_RETURNS_TASKPORT: parent-managed children).
 *
 * `argv', if non-NULL, is a kernel-owned flattened block (argc char *
 * slots pointing into the packed strings that follow).  The launcher lays
 * it out SysV-style on the child's stack and frees it; every failure path
 * here frees it too.
 *
 * Returns the new task's t_id, or a negative SYS_E_*.
 */
struct port;
struct port_space;

#include "port.h"		/* mach_port_name_t */
long	arch_spawn_user(const char *name, const uint8_t *image,
	    size_t image_size, struct port *inject_port,
	    struct port_space *caller_space,
	    mach_port_name_t *out_taskport_name,
	    int argc, char **argv);

/*
 * usermode_enter: push a synthetic iretq frame (SS, RSP, RFLAGS, CS, RIP)
 * and iretq to ring 3; never returns.  For launcher threads, once the
 * mappings and RSP0 are in place.
 */
void	usermode_enter(uint64_t user_rip, uint64_t user_rsp)
	    __attribute__((noreturn));

#endif /* !_MACHINE_USERMODE_H_ */
