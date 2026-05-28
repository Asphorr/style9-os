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
#include "panic.h"
#include "port.h"
#include "port_internal.h"
#include "progreg.h"
#include "syscall.h"

/*
 * Symbols emitted by objcopy when it wraps each user.elf into its
 * %_elf.o.  The wrapper uses the FILE NAME with non-alphanumeric
 * characters mapped to underscores, so e.g. hello.elf becomes
 * _binary_hello_elf_start etc.
 */
extern uint8_t	_binary_hello_elf_start[];
extern uint8_t	_binary_hello_elf_end[];

extern uint8_t	_binary_clock_elf_start[];
extern uint8_t	_binary_clock_elf_end[];

extern uint8_t	_binary_tasks_elf_start[];
extern uint8_t	_binary_tasks_elf_end[];

extern uint8_t	_binary_sh_elf_start[];
extern uint8_t	_binary_sh_elf_end[];

extern uint8_t	_binary_excchild_elf_start[];
extern uint8_t	_binary_excchild_elf_end[];

extern uint8_t	_binary_excchild_ud_elf_start[];
extern uint8_t	_binary_excchild_ud_elf_end[];

extern uint8_t	_binary_excchild_thr_elf_start[];
extern uint8_t	_binary_excchild_thr_elf_end[];

extern uint8_t	_binary_excchild_resume_elf_start[];
extern uint8_t	_binary_excchild_resume_elf_end[];

extern uint8_t	_binary_lsmp_elf_start[];
extern uint8_t	_binary_lsmp_elf_end[];

extern uint8_t	_binary_vmmap_elf_start[];
extern uint8_t	_binary_vmmap_elf_end[];

extern uint8_t	_binary_echod_elf_start[];
extern uint8_t	_binary_echod_elf_end[];

extern uint8_t	_binary_launchctl_elf_start[];
extern uint8_t	_binary_launchctl_elf_end[];

extern uint8_t	_binary_loopchild_elf_start[];
extern uint8_t	_binary_loopchild_elf_end[];

extern uint8_t	_binary_selfkill_elf_start[];
extern uint8_t	_binary_selfkill_elf_end[];

extern uint8_t	_binary_top_elf_start[];
extern uint8_t	_binary_top_elf_end[];

extern uint8_t	_binary_heartbeatd_elf_start[];
extern uint8_t	_binary_heartbeatd_elf_end[];

/*
 * Bridge into the arch-specific user-thread spawn path.  Lives in
 * arch/amd64/usermode.c; declared here so progreg_spawn doesn't have
 * to pull in machine headers.  Returns the new task's t_id or a
 * SYS_E_* negative.  `inject_port` is optional: when non-NULL the
 * launcher installs a SEND right on it into the child's port_space
 * at name MACH_PORT_PARENT before transitioning to ring 3.  Caller
 * must hold one SEND ref on inject_port for the duration -- that ref
 * is transferred into the child's name table on success and dropped
 * on any failure path.
 */
struct port;
struct port_space;
extern long	arch_spawn_user(const char *name,
		    const uint8_t *image, size_t image_size,
		    struct port *inject_port,
		    struct port_space *caller_space,
		    mach_port_name_t *out_taskport_name);

/*
 * Lock key:
 *	(c) const after progreg_init
 */
static struct progreg_entry	entries[PROGREG_MAX];	/* (c) */
static size_t			nentries;		/* (c) */

static void
register_one(const char *name, const uint8_t *start, const uint8_t *end)
{
	struct progreg_entry	*e;

	if (nentries >= PROGREG_MAX)
		panic("progreg_init: registry full at '%s'", name);
	if (start == NULL || end == NULL || end < start)
		panic("progreg_init: bad image for '%s'", name);

	e = &entries[nentries++];
	e->pr_name  = name;
	e->pr_image = start;
	e->pr_size  = (size_t)(end - start);
}

void
progreg_init(void)
{

	nentries = 0;
	register_one("hello",
	    _binary_hello_elf_start, _binary_hello_elf_end);
	register_one("clock",
	    _binary_clock_elf_start, _binary_clock_elf_end);
	register_one("tasks",
	    _binary_tasks_elf_start, _binary_tasks_elf_end);
	register_one("sh",
	    _binary_sh_elf_start, _binary_sh_elf_end);
	register_one("excchild",
	    _binary_excchild_elf_start, _binary_excchild_elf_end);
	register_one("excchild_ud",
	    _binary_excchild_ud_elf_start, _binary_excchild_ud_elf_end);
	register_one("excchild_thr",
	    _binary_excchild_thr_elf_start, _binary_excchild_thr_elf_end);
	register_one("excchild_resume",
	    _binary_excchild_resume_elf_start, _binary_excchild_resume_elf_end);
	register_one("lsmp",
	    _binary_lsmp_elf_start, _binary_lsmp_elf_end);
	register_one("vmmap",
	    _binary_vmmap_elf_start, _binary_vmmap_elf_end);
	register_one("echod",
	    _binary_echod_elf_start, _binary_echod_elf_end);
	register_one("launchctl",
	    _binary_launchctl_elf_start, _binary_launchctl_elf_end);
	register_one("loopchild",
	    _binary_loopchild_elf_start, _binary_loopchild_elf_end);
	register_one("selfkill",
	    _binary_selfkill_elf_start, _binary_selfkill_elf_end);
	register_one("top",
	    _binary_top_elf_start, _binary_top_elf_end);
	register_one("heartbeatd",
	    _binary_heartbeatd_elf_start, _binary_heartbeatd_elf_end);

	kprintf("progreg: %zu programs registered\n", nentries);
}

const struct progreg_entry *
progreg_find(const char *name)
{
	size_t	i, j;

	if (name == NULL)
		return (NULL);

	for (i = 0; i < nentries; i++) {
		const char	*reg = entries[i].pr_name;
		for (j = 0; ; j++) {
			if (reg[j] != name[j])
				break;
			if (reg[j] == '\0')
				return (&entries[i]);
		}
	}
	return (NULL);
}

size_t
progreg_snapshot(struct progreg_entry *out, size_t max)
{
	size_t	i, n;

	if (out == NULL || max == 0)
		return (0);

	n = nentries < max ? nentries : max;
	for (i = 0; i < n; i++)
		out[i] = entries[i];
	return (n);
}

long
progreg_spawn(const char *name)
{

	return (progreg_spawn_with_port(name, NULL));
}

long
progreg_spawn_with_port(const char *name, struct port *inject_port)
{
	const struct progreg_entry	*e;

	e = progreg_find(name);
	if (e == NULL) {
		if (inject_port != NULL)
			port_deref(inject_port, MACH_PORT_RIGHT_SEND);
		return (SYS_E_INVAL);
	}
	return (arch_spawn_user(e->pr_name, e->pr_image, e->pr_size,
	    inject_port, NULL, NULL));
}

/*
 * progreg_spawn_returning_taskport: spawn a registered program AND
 * install a SEND right on the new task's task-self port in
 * `caller_space`.  The resulting port name is written back through
 * `out_taskport_name` -- caller can then use it as the argument to
 * SYS_TASK_KILL or any other task-port-capability operation.
 *
 * Powers SYS_SPAWN_RETURNS_TASKPORT.  The shell uses this for every
 * child it tracks: storing {task_id, taskport_name} per child gives
 * sh.c the capability needed to terminate a foreground job on
 * Ctrl-C, or to implement a `kill` builtin.
 */
long
progreg_spawn_returning_taskport(const char *name,
    struct port_space *caller_space, mach_port_name_t *out_taskport_name)
{
	const struct progreg_entry	*e;

	if (caller_space == NULL || out_taskport_name == NULL)
		return (SYS_E_INVAL);
	e = progreg_find(name);
	if (e == NULL)
		return (SYS_E_INVAL);
	return (arch_spawn_user(e->pr_name, e->pr_image, e->pr_size,
	    NULL, caller_space, out_taskport_name));
}
