/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_MACHO_H_
#define	_SYS_MACHO_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Minimal Mach-O (thin x86-64, plus fat/universal slice picking) loader
 * for ring-3 programs -- the sibling of the ELF loader in elf.h.
 *
 * Apple ships every ring-3 binary as Mach-O; teaching the kernel to map
 * the container was the first rung of XNU binary compatibility (S1: the
 * container only).  By default the ABI INSIDE the container is unchanged --
 * the program still uses libstyle9's crt0 and the style9 SYS_* numbers,
 * so a Mach-O loaded by macho_load runs through the exact same launcher,
 * stack frame, and syscall path as an ELF.
 *
 * The exception is the syscall personality (S2): an image that declares
 * PLATFORM_MACOS through an LC_BUILD_VERSION is tagged TASK_PERSONALITY_DARWIN
 * by macho_load, and syscall_dispatch then routes that task through
 * darwin_dispatch (kern/darwin.c), which decodes Apple's class-encoded
 * syscalls (Mach traps, BSD calls) and their carry-flag error convention.
 * Binary-exact mach_msg / MIG and a real libSystem remain later steps.
 *
 * S4 adds dynamic linking: an image that carries an LC_LOAD_DYLINKER is
 * reported through macho_load_result.needs_dyld, and the launcher maps our
 * clean-room dyld (user/dyld.c) alongside it and enters through dyld, which
 * binds the image against our libSystem (user/libsystem.c).
 *
 * macho_load() parses an image already resident in kernel memory.  When
 * the image is a fat/universal archive it selects the CPU_TYPE_X86_64
 * slice and recurses on it; the thin path then drops every
 * LC_SEGMENT_64 into the target task's address space at its requested VA
 * (R/W/X from initprot, all U=1) and returns the entry RIP read from
 * LC_UNIXTHREAD or LC_MAIN.  Each segment is recorded in task->t_map
 * alongside the hardware install into task->t_pmap -- identical bookkeeping
 * to elf_load(), so the launcher treats the two formats interchangeably.
 *
 * The layouts below are imposed by the Mach-O file format (mach-o/loader.h
 * and mach-o/fat.h in Apple's headers); reordering fields desynchronises
 * the parser from what a Mach-O linker -- or our tools/elf2macho host
 * converter -- writes.  Fat headers are stored BIG-ENDIAN on disk; the
 * parser byte-swaps them (everything else is little-endian native).
 */

struct task;

/* Container magics (native byte order as read from a little-endian host). */
#define	MACHO_MAGIC_64		0xFEEDFACFu	/* 64-bit thin, native      */
#define	MACHO_CIGAM_64		0xCFFAEDFEu	/* 64-bit thin, byte-swapped */
#define	MACHO_FAT_MAGIC		0xCAFEBABEu	/* fat header, big-endian    */
#define	MACHO_FAT_CIGAM		0xBEBAFECAu	/* fat header as read LE     */

/* CPU types (the 0x01000000 bit is CPU_ARCH_ABI64). */
#define	MACHO_CPU_TYPE_X86_64	0x01000007

/* mach_header_64.filetype */
#define	MACHO_MH_EXECUTE	0x2
#define	MACHO_MH_DYLIB		0x6

/* load_command.cmd values we honour (LC_REQ_DYLD rides bit 31 of LC_MAIN). */
#define	MACHO_LC_REQ_DYLD	0x80000000u
#define	MACHO_LC_SEGMENT_64	0x19
#define	MACHO_LC_UNIXTHREAD	0x5
#define	MACHO_LC_LOAD_DYLINKER	0xE
#define	MACHO_LC_MAIN		(0x28u | MACHO_LC_REQ_DYLD)
#define	MACHO_LC_BUILD_VERSION	0x32

/*
 * LC_BUILD_VERSION.platform.  macho_load reads this to choose the task's
 * syscall ABI personality: PLATFORM_MACOS flips the task to the Darwin
 * personality (S2 -- the kernel honours Apple's class-encoded syscalls);
 * any other value, or no LC_BUILD_VERSION at all, leaves it native style9.
 */
#define	MACHO_PLATFORM_MACOS	1

/* LC_UNIXTHREAD flavor + register count for x86-64. */
#define	MACHO_x86_THREAD_STATE64	4
#define	MACHO_x86_THREAD_STATE64_COUNT	42	/* 21 uint64 = 42 uint32 */

/* segment_command_64 initprot/maxprot bits (vm_prot_t on disk). */
#define	MACHO_VM_PROT_READ	0x1
#define	MACHO_VM_PROT_WRITE	0x2
#define	MACHO_VM_PROT_EXECUTE	0x4

/* WIRE FORMAT.  Mach-O-imposed (mach_header_64). */
struct mach_header_64 {
	uint32_t	magic;
	uint32_t	cputype;
	uint32_t	cpusubtype;
	uint32_t	filetype;
	uint32_t	ncmds;
	uint32_t	sizeofcmds;
	uint32_t	flags;
	uint32_t	reserved;
};

/* WIRE FORMAT.  Common prefix of every load command. */
struct mach_load_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
};

/* WIRE FORMAT.  LC_SEGMENT_64 (section headers, if any, follow inline). */
struct mach_segment_command_64 {
	uint32_t	cmd;
	uint32_t	cmdsize;
	char		segname[16];
	uint64_t	vmaddr;
	uint64_t	vmsize;
	uint64_t	fileoff;
	uint64_t	filesize;
	int32_t		maxprot;
	int32_t		initprot;
	uint32_t	nsects;
	uint32_t	flags;
};

/*
 * WIRE FORMAT.  LC_UNIXTHREAD header; the flavor-specific register block
 * (struct mach_x86_thread_state64 for flavor x86_THREAD_STATE64) follows.
 */
struct mach_thread_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint32_t	flavor;
	uint32_t	count;
};

/* WIRE FORMAT.  x86_THREAD_STATE64 register block (21 * 8 = 168 bytes). */
struct mach_x86_thread_state64 {
	uint64_t	rax, rbx, rcx, rdx;
	uint64_t	rdi, rsi, rbp, rsp;
	uint64_t	r8, r9, r10, r11;
	uint64_t	r12, r13, r14, r15;
	uint64_t	rip, rflags;
	uint64_t	cs, fs, gs;
};

/* WIRE FORMAT.  LC_MAIN (entryoff is a file offset into the image). */
struct mach_entry_point_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint64_t	entryoff;
	uint64_t	stacksize;
};

/*
 * WIRE FORMAT.  LC_LOAD_DYLINKER -- names the dynamic linker (e.g.
 * "/usr/lib/dyld").  `name` is an lc_str: a byte offset from the start of
 * this command to the inline NUL-terminated path that follows.  The path is
 * read past but not honoured -- the kernel maps its own embedded dyld -- so
 * only the command's presence matters.
 */
struct mach_dylinker_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint32_t	name;
};

/*
 * WIRE FORMAT.  LC_BUILD_VERSION (per-tool build_tool_version entries, if
 * any, follow inline).  We honour only `platform`; minos/sdk/ntools are read
 * past but ignored.
 */
struct mach_build_version_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint32_t	platform;
	uint32_t	minos;
	uint32_t	sdk;
	uint32_t	ntools;
};

/* WIRE FORMAT.  fat/universal header + per-arch entry (BIG-ENDIAN on disk). */
struct mach_fat_header {
	uint32_t	magic;
	uint32_t	nfat_arch;
};

struct mach_fat_arch {
	uint32_t	cputype;
	uint32_t	cpusubtype;
	uint32_t	offset;
	uint32_t	size;
	uint32_t	align;
};

_Static_assert(sizeof(struct mach_header_64) == 32, "mach_header_64 must be 32 bytes");
_Static_assert(sizeof(struct mach_load_command) == 8, "load_command must be 8 bytes");
_Static_assert(sizeof(struct mach_segment_command_64) == 72, "segment_command_64 must be 72 bytes");
_Static_assert(sizeof(struct mach_thread_command) == 16, "thread_command must be 16 bytes");
_Static_assert(sizeof(struct mach_x86_thread_state64) == 168, "x86_thread_state64 must be 168 bytes");
_Static_assert(sizeof(struct mach_entry_point_command) == 24, "entry_point_command must be 24 bytes");
_Static_assert(sizeof(struct mach_dylinker_command) == 12, "dylinker_command must be 12 bytes");
_Static_assert(sizeof(struct mach_build_version_command) == 24, "build_version_command must be 24 bytes");
_Static_assert(sizeof(struct mach_fat_header) == 8, "fat_header must be 8 bytes");
_Static_assert(sizeof(struct mach_fat_arch) == 20, "fat_arch must be 20 bytes");

#define	MACHO_E_OK		0
#define	MACHO_E_TRUNCATED	(-1)
#define	MACHO_E_BADMAG		(-2)
#define	MACHO_E_BADCPU		(-3)	/* wrong cputype / no x86-64 slice */
#define	MACHO_E_BADTYPE		(-4)	/* not MH_EXECUTE                  */
#define	MACHO_E_NOMEM		(-5)
#define	MACHO_E_MAP		(-6)
#define	MACHO_E_BADCMD		(-7)	/* malformed load command          */
#define	MACHO_E_NOENTRY		(-8)	/* no LC_UNIXTHREAD / LC_MAIN       */

/*
 * Outcome of macho_load: the entry RIP the image declares (LC_UNIXTHREAD /
 * LC_MAIN), the VA its mach_header was mapped at (the fileoff-0 segment base,
 * which the launcher hands to dyld as the main-image handle), and whether the
 * image carries an LC_LOAD_DYLINKER -- i.e. is dynamically linked and must be
 * entered through dyld rather than at its own entry.
 */
struct macho_load_result {
	uint64_t	entry;
	uint64_t	image_base;
	bool		needs_dyld;
};

int	macho_load(struct task *target, const void *image, size_t image_size,
	    struct macho_load_result *out);

/*
 * Map a Mach-O dynamic library (MH_DYLIB) into `target` at `bias`, which is
 * added to every segment's vmaddr -- a dylib is linked relocatable (base 0),
 * so the caller chooses where it lands.  Unlike macho_load this resolves no
 * entry point and touches no syscall personality; a dylib has neither.  The
 * total page-rounded VA span consumed from `bias` upward is returned through
 * *out_span.  Returns MACHO_E_OK or a negative MACHO_E_*.
 *
 * Backs the clean-room dyld's "map image by path" backchannel (kern/darwin.c):
 * dyld reads a dependency path out of the main image's LC_LOAD_DYLIB, asks the
 * kernel to map it here, then binds against the export trie it reads back.
 */
int	macho_map_dylib(struct task *target, const void *image,
	    size_t image_size, uint64_t bias, uint64_t *out_span);

#endif /* !_SYS_MACHO_H_ */
