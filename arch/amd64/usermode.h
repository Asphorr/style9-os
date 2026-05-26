/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_USERMODE_H_
#define	_MACHINE_USERMODE_H_

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
 * usermode_run_hello_elf: load the kernel-embedded hello.elf
 * (linked in via objcopy as _binary_obj_hello_elf_start ... _end) and
 * iretq to its e_entry on a fresh user stack.
 */
void	usermode_run_hello_elf(void);

/*
 * usermode_enter: never returns to its caller.  Pushes a synthetic
 * iretq frame (SS, RSP, RFLAGS, CS, RIP) for user-mode and iretq's.
 * Called from the launcher thread once the page mappings + RSP0
 * bookkeeping are in place.
 */
void	usermode_enter(uint64_t user_rip, uint64_t user_rsp)
	    __attribute__((noreturn));

#endif /* !_MACHINE_USERMODE_H_ */
