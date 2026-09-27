/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "port_internal.h"
#include "progreg.h"
#include "syscall.h"

/*
 * Symbols objcopy emits when it wraps each user image: the file name with
 * non-alphanumerics mapped to '_', so hello.elf becomes
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

extern uint8_t	_binary_oolchild_elf_start[];
extern uint8_t	_binary_oolchild_elf_end[];

extern uint8_t	_binary_selfkill_elf_start[];
extern uint8_t	_binary_selfkill_elf_end[];

extern uint8_t	_binary_top_elf_start[];
extern uint8_t	_binary_top_elf_end[];

extern uint8_t	_binary_heartbeatd_elf_start[];
extern uint8_t	_binary_heartbeatd_elf_end[];

extern uint8_t	_binary_argecho_elf_start[];
extern uint8_t	_binary_argecho_elf_end[];

extern uint8_t	_binary_crasher_elf_start[];
extern uint8_t	_binary_crasher_elf_end[];

/*
 * A native program rewrapped as Mach-O by tools/elf2macho; the launcher
 * sniffs the magic and uses macho_load.  One thin image, one single-slice
 * fat archive, to cover both loader paths.
 */
extern uint8_t	_binary_machotest_macho_start[];
extern uint8_t	_binary_machotest_macho_end[];

extern uint8_t	_binary_machotest_fat_macho_start[];
extern uint8_t	_binary_machotest_fat_macho_end[];

/*
 * Darwin-personality probes: Mach-Os declaring PLATFORM_MACOS that issue
 * Apple class-encoded syscalls directly (user/darwinhello.S, a
 * freestanding stub, and user/darwinmsg.c).
 */
extern uint8_t	_binary_darwinhello_macho_start[];
extern uint8_t	_binary_darwinhello_macho_end[];

extern uint8_t	_binary_darwinmsg_macho_start[];
extern uint8_t	_binary_darwinmsg_macho_end[];

/*
 * dyldhello: a clang/ld64.lld dynamic Mach-O importing from
 * /usr/lib/libSystem.B.dylib, with /usr/lib/dyld as LC_LOAD_DYLINKER.
 * dyld itself is not registered: it is mapped by the launcher, never
 * spawned by name.
 */
extern uint8_t	_binary_dyldhello_macho_start[];
extern uint8_t	_binary_dyldhello_macho_end[];

/*
 * dyldbig: dyldhello linked at Apple's default base (__TEXT at
 * 0x100000000), exercising macho_load's relocate-low path.
 */
extern uint8_t	_binary_dyldbig_macho_start[];
extern uint8_t	_binary_dyldbig_macho_end[];

/*
 * Real Apple x86-64 binaries (Homebrew bottles in extern/, not our
 * builds): Apple-toolchain dynamic Mach-Os with chained fixups and __TEXT
 * at 0x100000000, relocated low by macho_load and bound by our dyld.
 *
 * figlet: libSystem only.
 */
extern uint8_t	_binary_figlet_macho_start[];
extern uint8_t	_binary_figlet_macho_end[];

/*
 * dirlist: our Darwin-ABI probe that walks the volume through libSystem's
 * opendir/readdir/stat, ahead of tree.
 */
extern uint8_t	_binary_dirlist_macho_start[];
extern uint8_t	_binary_dirlist_macho_end[];

/* tree(1), real: descends directories via opendir/readdir/lstat. */
extern uint8_t	_binary_tree_macho_start[];
extern uint8_t	_binary_tree_macho_end[];

/*
 * guname (GNU coreutils' uname), real: prints the Darwin identity the
 * kernel reports (kern/darwin.c).
 */
extern uint8_t	_binary_guname_macho_start[];
extern uint8_t	_binary_guname_macho_end[];

/*
 * gcat (GNU coreutils' cat), real: reads file bytes (open, fstat, a
 * page-aligned read loop, write), end to end from APFS extents.
 */
extern uint8_t	_binary_gcat_macho_start[];
extern uint8_t	_binary_gcat_macho_end[];

/*
 * gls (GNU coreutils' ls), real: prints inode metadata (mode, owner, link
 * count, dates) and walks directories with fstatat(dirfd(dirp), name).
 */
extern uint8_t	_binary_gls_macho_start[];
extern uint8_t	_binary_gls_macho_end[];

/*
 * timeprobe: our probe that the wall clock is plausible, advances and
 * never runs backwards, as seen from ring 3.
 */
extern uint8_t	_binary_timeprobe_macho_start[];
extern uint8_t	_binary_timeprobe_macho_end[];

/*
 * mmaptest: probes mmap(2) and demand paging: maps more than the machine
 * has, has the kernel write to an untouched page, and compares a file
 * mapping with read(2).
 */
extern uint8_t	_binary_mmaptest_macho_start[];
extern uint8_t	_binary_mmaptest_macho_end[];

/*
 * filewrite: probes volume writes: create, read back through another fd,
 * overwrite, append, truncate, unlink, rename over an open file, and a
 * built-in's refusal to be written.  It also makes a directory, creates a
 * file in it, and checks rmdir refuses while it is non-empty.
 */
extern uint8_t	_binary_filewrite_macho_start[];
extern uint8_t	_binary_filewrite_macho_end[];

/* lockprobe: probes fcntl record locks between a parent and its children. */
extern uint8_t	_binary_lockprobe_macho_start[];
extern uint8_t	_binary_lockprobe_macho_end[];

/* cpuprobe: probes the CPU time getrusage and wait4 report. */
extern uint8_t	_binary_cpuprobe_macho_start[];
extern uint8_t	_binary_cpuprobe_macho_end[];

/*
 * ttyprobe: probes the terminal: a raw setting reaches the kernel and
 * reads back, a file and a pipe answer ENOTTY, the window size is the
 * screen's, and in raw mode a key with no newline reaches the reader.
 */
extern uint8_t	_binary_ttyprobe_macho_start[];
extern uint8_t	_binary_ttyprobe_macho_end[];

/*
 * gstty (GNU coreutils' stty), real: reads the whole termios, names each
 * flag and sets them back, an Apple-built oracle for our terminal.
 */
extern uint8_t	_binary_gstty_macho_start[];
extern uint8_t	_binary_gstty_macho_end[];

/*
 * gmkdir and grmdir (GNU coreutils' mkdir and rmdir), real: need real
 * modes (chmod, an applied umask).
 */
extern uint8_t	_binary_gmkdir_macho_start[];
extern uint8_t	_binary_gmkdir_macho_end[];

extern uint8_t	_binary_grmdir_macho_start[];
extern uint8_t	_binary_grmdir_macho_end[];

/*
 * gmake (GNU make 4.4.1), real: runs other programs, waits on descriptor
 * readiness and passes its children an environment.
 * makedemo.sh, a dash script carried like demo.sh, writes the project it
 * builds at boot.
 */
extern uint8_t	_binary_gmake_macho_start[];
extern uint8_t	_binary_gmake_macho_end[];

extern uint8_t	_binary_makedemo_sh_macho_start[];
extern uint8_t	_binary_makedemo_sh_macho_end[];

/*
 * sqlite3 (the SQLite 3.53.4 shell), real: a database on the APFS volume,
 * linking libz and readline besides libSystem.  sqldemo.sh drives it at
 * boot.
 */
extern uint8_t	_binary_sqlite3_macho_start[];
extern uint8_t	_binary_sqlite3_macho_end[];

extern uint8_t	_binary_sqldemo_sh_macho_start[];
extern uint8_t	_binary_sqldemo_sh_macho_end[];

/*
 * gfactor (GNU coreutils' factor), real: also links libgmp, so dyld maps
 * the closure gfactor -> libgmp -> libSystem and binds each import against
 * the dylib its lib_ordinal names; big values exercise libgmp.
 */
extern uint8_t	_binary_gfactor_macho_start[];
extern uint8_t	_binary_gfactor_macho_end[];

/*
 * Processes.  pipefork: our probe for fork/execve/wait4/pipe/dup2.  genv
 * and gtimeout (GNU coreutils 9.11), real: env execs its command in place;
 * timeout forks, wait4s and kills it.
 */
extern uint8_t	_binary_pipefork_macho_start[];
extern uint8_t	_binary_pipefork_macho_end[];

extern uint8_t	_binary_genv_macho_start[];
extern uint8_t	_binary_genv_macho_end[];

extern uint8_t	_binary_gtimeout_macho_start[];
extern uint8_t	_binary_gtimeout_macho_end[];

/*
 * dash 0.5.13.4, real: a POSIX shell, also /bin/sh on the Darwin side
 * (darwin_bin_find).  demo.sh is a plain-text script registered so the
 * synthetic /bin can stat and open it for dash; execve refuses it by
 * magic, as a kernel without #! support would.
 */
extern uint8_t	_binary_dash_macho_start[];
extern uint8_t	_binary_dash_macho_end[];

extern uint8_t	_binary_demo_sh_macho_start[];
extern uint8_t	_binary_demo_sh_macho_end[];

/*
 * The arch spawn path (arch/amd64/usermode.c), declared here to avoid
 * machine headers.  Returns the new task's t_id or a negative SYS_E_*.
 * An optional `inject_port' is installed as a SEND right at
 * MACH_PORT_PARENT in the child; the caller's SEND ref on it moves into
 * the child on success and is dropped on failure.  See usermode.h for the
 * other arguments.
 */
struct port;
struct port_space;
extern long	arch_spawn_user(const char *name,
		    const uint8_t *image, size_t image_size,
		    struct port *inject_port,
		    struct port_space *caller_space,
		    mach_port_name_t *out_taskport_name,
		    int argc, char **argv);

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
	register_one("oolchild",
	    _binary_oolchild_elf_start, _binary_oolchild_elf_end);
	register_one("selfkill",
	    _binary_selfkill_elf_start, _binary_selfkill_elf_end);
	register_one("top",
	    _binary_top_elf_start, _binary_top_elf_end);
	register_one("heartbeatd",
	    _binary_heartbeatd_elf_start, _binary_heartbeatd_elf_end);
	register_one("argecho",
	    _binary_argecho_elf_start, _binary_argecho_elf_end);
	register_one("crasher",
	    _binary_crasher_elf_start, _binary_crasher_elf_end);
	register_one("machotest",
	    _binary_machotest_macho_start, _binary_machotest_macho_end);
	register_one("machotest_fat",
	    _binary_machotest_fat_macho_start, _binary_machotest_fat_macho_end);
	register_one("darwinhello",
	    _binary_darwinhello_macho_start, _binary_darwinhello_macho_end);
	register_one("darwinmsg",
	    _binary_darwinmsg_macho_start, _binary_darwinmsg_macho_end);
	register_one("dyldhello",
	    _binary_dyldhello_macho_start, _binary_dyldhello_macho_end);
	register_one("dyldbig",
	    _binary_dyldbig_macho_start, _binary_dyldbig_macho_end);
	register_one("figlet",
	    _binary_figlet_macho_start, _binary_figlet_macho_end);
	register_one("dirlist",
	    _binary_dirlist_macho_start, _binary_dirlist_macho_end);
	register_one("tree",
	    _binary_tree_macho_start, _binary_tree_macho_end);
	register_one("guname",
	    _binary_guname_macho_start, _binary_guname_macho_end);
	register_one("gcat",
	    _binary_gcat_macho_start, _binary_gcat_macho_end);
	register_one("gls",
	    _binary_gls_macho_start, _binary_gls_macho_end);
	register_one("timeprobe",
	    _binary_timeprobe_macho_start, _binary_timeprobe_macho_end);
	register_one("mmaptest",
	    _binary_mmaptest_macho_start, _binary_mmaptest_macho_end);
	register_one("filewrite",
	    _binary_filewrite_macho_start, _binary_filewrite_macho_end);
	register_one("lockprobe",
	    _binary_lockprobe_macho_start, _binary_lockprobe_macho_end);
	register_one("cpuprobe",
	    _binary_cpuprobe_macho_start, _binary_cpuprobe_macho_end);
	register_one("ttyprobe",
	    _binary_ttyprobe_macho_start, _binary_ttyprobe_macho_end);
	register_one("gstty",
	    _binary_gstty_macho_start, _binary_gstty_macho_end);
	register_one("gmkdir",
	    _binary_gmkdir_macho_start, _binary_gmkdir_macho_end);
	register_one("grmdir",
	    _binary_grmdir_macho_start, _binary_grmdir_macho_end);
	register_one("gmake",
	    _binary_gmake_macho_start, _binary_gmake_macho_end);
	register_one("makedemo.sh",
	    _binary_makedemo_sh_macho_start, _binary_makedemo_sh_macho_end);
	register_one("sqlite3",
	    _binary_sqlite3_macho_start, _binary_sqlite3_macho_end);
	register_one("sqldemo.sh",
	    _binary_sqldemo_sh_macho_start, _binary_sqldemo_sh_macho_end);
	register_one("gfactor",
	    _binary_gfactor_macho_start, _binary_gfactor_macho_end);
	register_one("pipefork",
	    _binary_pipefork_macho_start, _binary_pipefork_macho_end);
	register_one("genv",
	    _binary_genv_macho_start, _binary_genv_macho_end);
	register_one("gtimeout",
	    _binary_gtimeout_macho_start, _binary_gtimeout_macho_end);
	register_one("dash",
	    _binary_dash_macho_start, _binary_dash_macho_end);
	register_one("demo.sh",
	    _binary_demo_sh_macho_start, _binary_demo_sh_macho_end);

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

const struct progreg_entry *
progreg_at(size_t idx)
{

	if (idx >= nentries)
		return (NULL);
	return (&entries[idx]);
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
	    inject_port, NULL, NULL, 0, NULL));
}

/*
 * progreg_spawn_returning_taskport: spawn a registered program and
 * install a SEND right on its task-self port in `caller_space`, the name
 * written to `out_taskport_name` (usable with SYS_TASK_KILL).  Backs
 * SYS_SPAWN_RETURNS_TASKPORT; sh.c keeps {task_id, taskport_name} per
 * child to kill a foreground job on Ctrl-C.
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
	    NULL, caller_space, out_taskport_name, 0, NULL));
}

/*
 * progreg_spawn_args: spawn `name` with a command line.  `argv` is a
 * kernel-owned flat block (progreg.h) or NULL when argc is 0;
 * `caller_space'/`out_taskport_name' are the optional taskport install
 * (NULL/NULL to skip).  `argv' is consumed: arch_spawn_user frees it on
 * failure, the launcher on success, and an unknown name frees it here.
 */
long
progreg_spawn_args(const char *name, int argc, char **argv,
    struct port_space *caller_space, mach_port_name_t *out_taskport_name)
{
	const struct progreg_entry	*e;

	e = progreg_find(name);
	if (e == NULL) {
		if (argv != NULL)
			kfree(argv);
		return (SYS_E_INVAL);
	}
	return (arch_spawn_user(e->pr_name, e->pr_image, e->pr_size,
	    NULL, caller_space, out_taskport_name, argc, argv));
}
