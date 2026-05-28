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
 * Program registry.
 *
 * The kernel currently ships every ring-3 program embedded in its
 * own image (objcopy wraps each user ELF into a .rodata blob; see
 * the "_elf.o" rule in the Makefile).  progreg is the small table
 * that names those blobs so SYS_SPAWN can resolve a string name to
 * an (image, size) pair and hand them to the ELF loader.
 *
 * When a filesystem lands, progreg shrinks to a back-compat shim
 * pointing at /sbin/NAME via vnode lookups; the user-facing
 * SYS_SPAWN ABI does not move.
 */

#define	PROGREG_MAX		16
#define	PROGREG_NAME_MAX	24

struct progreg_entry {
	const char	*pr_name;	/* lookup key                   */
	const uint8_t	*pr_image;	/* objcopy'd ELF bytes start    */
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
 * entries were written; never more than `max`.  Used by the shell to
 * answer "what can I spawn?".
 */
size_t	progreg_snapshot(struct progreg_entry *out, size_t max);

/*
 * Spawn the named program: build a fresh task, load its ELF image into
 * the new pmap + vm_map, attach a ring-3 thread that iretq's to the
 * entry point.  Returns the new task_id on success, or a negative
 * SYS_E_* code on failure.
 */
long	progreg_spawn(const char *name);

/*
 * Variant that also injects a SEND right into the child's port_space at
 * MACH_PORT_PARENT (well-known name 3 = next-free slot after TASK_SELF
 * and BOOTSTRAP).  The caller must have already taken one extra SEND
 * ref on `inject_port` -- that ref is transferred into the child's
 * port_space.  On any failure path the ref is dropped.  `inject_port`
 * NULL means "behave identically to progreg_spawn".
 */
struct port;
long	progreg_spawn_with_port(const char *name, struct port *inject_port);

#endif /* !_SYS_PROGREG_H_ */
