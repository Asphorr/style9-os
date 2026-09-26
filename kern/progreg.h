/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_PROGREG_H_
#define	_SYS_PROGREG_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Program registry: the ring-3 programs embedded in the kernel image
 * (objcopy wraps each ELF or Mach-O into a .rodata blob; see the Makefile),
 * named so SYS_SPAWN can resolve a name to (image, size) for the loader.
 * The Darwin personality also presents it as the synthetic /bin.
 */

/*
 * Room for the registered programs.  register_one panics on a full table
 * rather than drop an entry, which would look like a program failing to
 * load.
 */
#define	PROGREG_MAX		64
#define	PROGREG_NAME_MAX	24

/*
 * Argument limits for SYS_SPAWN_ARGS and execve(2).  The syscall layer
 * copies argv into a kernel-side flat block bounded by these, and the
 * launcher builds it onto the child's initial stack
 * (arch/amd64/usermode.c).  A native program's stack is one page, which
 * 16 pointers + 512 string bytes + framing fit easily.
 */
#define	SPAWN_ARGV_MAX		16
#define	SPAWN_ARG_BYTES_MAX	512

/*
 * The environment a Darwin execve(2) carries, with its own caps.  The dyld
 * handoff frame lives in the top page of the new stack, so argv, envp and
 * the pointer block must fit in 4 KiB: (16 + 32 + 5) quadwords = 424
 * bytes, plus 512 + 2048 of strings = 2984, leaving room for alignment.
 * The sum is checked (darwin_frame_fits), and a frame that will not fit is
 * refused with E2BIG.
 */
#define	SPAWN_ENV_MAX		32
#define	SPAWN_ENV_BYTES_MAX	2048

struct progreg_entry {
	const char	*pr_name;	/* lookup key                   */
	const uint8_t	*pr_image;	/* objcopy'd image start        */
	size_t		 pr_size;	/* image size in bytes          */
};

void	progreg_init(void);

/*
 * Lookup by name; returns NULL if no program is registered under that
 * name.  The returned pointer is stable for the lifetime of the kernel
 * (entries live in BSS + .rodata).
 */
const struct progreg_entry *progreg_find(const char *name);

/*
 * Snapshot the registry into the caller's array.  Returns how many
 * entries were written, at most `max'.  Used to list what can be spawned
 * (kern/cmds.c).
 */
size_t	progreg_snapshot(struct progreg_entry *out, size_t max);

/*
 * Entry `idx` in registration order, or NULL past the end.  The Darwin
 * personality's synthetic /bin (kern/darwin.c) enumerates it.
 */
const struct progreg_entry *progreg_at(size_t idx);

/*
 * Spawn the named program: a fresh task, its ELF or Mach-O image loaded,
 * and a ring-3 thread that iretqs to the entry point.  Returns the new
 * task_id, or a negative SYS_E_* (SYS_E_INVAL for an unknown name).
 */
long	progreg_spawn(const char *name);

/*
 * Also installs a SEND right in the child at MACH_PORT_PARENT (name 3,
 * after TASK_SELF and BOOTSTRAP).  The caller's extra SEND ref on
 * `inject_port` moves into the child, or is dropped on failure.  NULL
 * behaves as progreg_spawn.
 */
struct port;
long	progreg_spawn_with_port(const char *name, struct port *inject_port);

/*
 * Also installs a SEND right on the child's task-self port in
 * `caller_space`, writing its name to `*out_taskport_name`.  Backs
 * SYS_SPAWN_RETURNS_TASKPORT; the shell passes the name to SYS_TASK_KILL
 * (Ctrl-C, its `kill' builtin).  Both pointers are required (SYS_E_INVAL
 * if NULL).  On failure the port name is left untouched.
 */
struct port_space;
#include "port.h"		/* mach_port_name_t */
long	progreg_spawn_returning_taskport(const char *name,
	    struct port_space *caller_space,
	    mach_port_name_t *out_taskport_name);

/*
 * Full spawn, backing SYS_SPAWN_ARGS: a name, the child's arguments, and
 * the optional taskport install (NULL/NULL to skip).  `argv` is a
 * kernel-owned flat block -- (argc+1) char * slots, the last NULL,
 * pointing into packed strings that follow -- or NULL when argc is 0.
 * `argv` is consumed: freed on failure, or by the launcher once the
 * child's stack is built; the caller must not touch it afterwards.
 */
long	progreg_spawn_args(const char *name, int argc, char **argv,
	    struct port_space *caller_space,
	    mach_port_name_t *out_taskport_name);

#endif /* !_SYS_PROGREG_H_ */
