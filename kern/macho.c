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
#include "macho.h"
#include "pmap.h"
#include "pmm.h"
#include "task.h"
#include "vm.h"

/*
 * Where a main image whose linked __TEXT lies outside the user window is
 * relocated to (see load_thin).  In [VM_USER_VA_LO, VM_USER_VA_HI), below
 * dyld (0x60000000, leaving ~256 MiB) and the dylibs (0x70000000+); the
 * Darwin stack grows down from here (DARWIN_STACK_TOP).
 */
#define	MACHO_IMAGE_BASE	0x50000000ULL

static int		load_thin(struct task *target, const uint8_t *image,
			    size_t image_size, struct macho_load_result *out);
static int		load_fat(struct task *target, const uint8_t *image,
			    size_t image_size, struct macho_load_result *out);
static int		load_segment(struct task *target, const uint8_t *image,
			    size_t image_size,
			    const struct mach_segment_command_64 *sg,
			    uint64_t bias);
static uint32_t		be32(uint32_t v);

/*
 * Load a Mach-O image resident in kernel memory into `target`: a fat
 * archive goes to the slice picker, a thin image to load_thin.  Fills
 * `*out' and returns MACHO_E_OK, or a negative MACHO_E_*; the launcher
 * picks this or elf_load() by the 4-byte magic.
 */
int
macho_load(struct task *target, const void *image, size_t image_size,
    struct macho_load_result *out)
{
	const uint8_t	*bytes;
	uint32_t	 magic;

	if (target == NULL || image == NULL || out == NULL)
		return (MACHO_E_TRUNCATED);
	if (image_size < sizeof(uint32_t))
		return (MACHO_E_TRUNCATED);

	out->entry      = 0;
	out->image_base = 0;
	out->needs_dyld = false;

	bytes = (const uint8_t *)image;
	magic = *(const uint32_t *)image;

	if (magic == MACHO_FAT_MAGIC || magic == MACHO_FAT_CIGAM)
		return (load_fat(target, bytes, image_size, out));
	return (load_thin(target, bytes, image_size, out));
}

/*
 * Pick the CPU_TYPE_X86_64 slice of a fat archive and load it thin.  Fat
 * headers are big-endian, so every field goes through be32().  The slice
 * goes to load_thin, not macho_load, so fat-in-fat is rejected as a bad
 * magic rather than recursed into.
 */
static int
load_fat(struct task *target, const uint8_t *image, size_t image_size,
    struct macho_load_result *out)
{
	const struct mach_fat_arch	*fa;
	const struct mach_fat_header	*fh;
	size_t				 aoff;
	uint32_t			 cputype;
	uint32_t			 i;
	uint32_t			 narch;
	uint32_t			 offset;
	uint32_t			 size;

	if (image_size < sizeof(*fh))
		return (MACHO_E_TRUNCATED);
	fh = (const struct mach_fat_header *)image;
	narch = be32(fh->nfat_arch);

	/*
	 * A real universal binary has a handful of slices; an absurd count
	 * means a mis-sniffed non-fat image.
	 */
	if (narch == 0 || narch > 64)
		return (MACHO_E_BADCMD);

	for (i = 0; i < narch; i++) {
		aoff = sizeof(*fh) + (size_t)i * sizeof(*fa);
		if (aoff + sizeof(*fa) > image_size)
			return (MACHO_E_TRUNCATED);
		fa = (const struct mach_fat_arch *)(image + aoff);

		cputype = be32(fa->cputype);
		if (cputype != MACHO_CPU_TYPE_X86_64)
			continue;

		offset = be32(fa->offset);
		size   = be32(fa->size);
		if ((uint64_t)offset + size > image_size)
			return (MACHO_E_TRUNCATED);
		if (size < sizeof(struct mach_header_64))
			return (MACHO_E_TRUNCATED);

		kprintf("macho: fat archive, %u slices -- selected x86_64 "
		    "at off=%u size=%u\n",
		    (unsigned)narch, (unsigned)offset, (unsigned)size);
		return (load_thin(target, image + offset, size, out));
	}

	return (MACHO_E_BADCPU);	/* no x86-64 slice in this archive */
}

/*
 * Load a thin 64-bit Mach-O: validate the header, walk the load commands
 * within sizeofcmds, map every LC_SEGMENT_64, and take the entry from
 * LC_UNIXTHREAD (rip verbatim) or LC_MAIN (entryoff from the mach-header
 * base, as dyld computes it); LC_UNIXTHREAD wins if both are present.
 *
 * An image whose __TEXT is linked outside [VM_USER_VA_LO, VM_USER_VA_HI)
 * (a real Apple binary at 0x100000000) is slid to MACHO_IMAGE_BASE by a
 * bias added to every vmaddr and the entry.  Our own -pagezero_size'd
 * binaries sit in the window and take a zero bias.
 */
static int
load_thin(struct task *target, const uint8_t *image, size_t image_size,
    struct macho_load_result *out)
{
	const struct mach_header_64	*mh;
	uint64_t			 base_vmaddr;
	uint64_t			 entry;
	uint64_t			 load_bias;
	uint64_t			 main_entryoff;
	size_t				 end;
	size_t				 off;
	uint32_t			 i;
	int				 rv;
	bool				 darwin_platform;
	bool				 have_base;
	bool				 have_main;
	bool				 have_thread;

	if (image_size < sizeof(*mh))
		return (MACHO_E_TRUNCATED);
	mh = (const struct mach_header_64 *)image;

	if (mh->magic != MACHO_MAGIC_64)
		return (MACHO_E_BADMAG);
	if (mh->cputype != MACHO_CPU_TYPE_X86_64)
		return (MACHO_E_BADCPU);
	if (mh->filetype != MACHO_MH_EXECUTE)
		return (MACHO_E_BADTYPE);
	if ((uint64_t)sizeof(*mh) + mh->sizeofcmds > image_size)
		return (MACHO_E_TRUNCATED);

	base_vmaddr     = 0;
	entry           = 0;
	load_bias       = 0;
	main_entryoff   = 0;
	darwin_platform = false;
	have_base       = false;
	have_main       = false;
	have_thread     = false;

	end = sizeof(*mh) + mh->sizeofcmds;
	off = sizeof(*mh);

	for (i = 0; i < mh->ncmds; i++) {
		const struct mach_load_command	*lc;
		uint32_t			 cmd;
		uint32_t			 cmdsize;

		if (off + sizeof(*lc) > end)
			return (MACHO_E_BADCMD);
		lc      = (const struct mach_load_command *)(image + off);
		cmd     = lc->cmd;
		cmdsize = lc->cmdsize;
		if (cmdsize < sizeof(*lc) || off + cmdsize > end)
			return (MACHO_E_BADCMD);

		switch (cmd) {
		case MACHO_LC_SEGMENT_64: {
			const struct mach_segment_command_64	*sg;

			if (cmdsize < sizeof(*sg))
				return (MACHO_E_BADCMD);
			sg = (const struct mach_segment_command_64 *)
			    (image + off);
			/*
			 * The first non-empty fileoff-0 segment is __TEXT, the
			 * image base; choose the bias here, before mapping it.
			 * Only __PAGEZERO precedes it and is never mapped
			 * (initprot 0), so its zero bias is harmless.  dyld
			 * derives the slide from the biased mach-header we
			 * report, which keeps its chained-fixup walk right.
			 */
			if (sg->fileoff == 0 && sg->filesize > 0 && !have_base) {
				if (sg->vmaddr < VM_USER_VA_LO ||
				    sg->vmaddr >= VM_USER_VA_HI)
					load_bias = MACHO_IMAGE_BASE - sg->vmaddr;
				base_vmaddr = sg->vmaddr + load_bias;
				have_base   = true;
			}
			rv = load_segment(target, image, image_size, sg,
			    load_bias);
			if (rv != MACHO_E_OK)
				return (rv);
			break;
		}
		case MACHO_LC_UNIXTHREAD: {
			const struct mach_thread_command	*tc;
			const struct mach_x86_thread_state64	*ts;

			if (cmdsize < sizeof(*tc) + sizeof(*ts))
				return (MACHO_E_BADCMD);
			tc = (const struct mach_thread_command *)(image + off);
			if (tc->flavor != MACHO_x86_THREAD_STATE64 ||
			    tc->count != MACHO_x86_THREAD_STATE64_COUNT)
				return (MACHO_E_BADCMD);
			ts = (const struct mach_x86_thread_state64 *)
			    (image + off + sizeof(*tc));
			entry       = ts->rip;
			have_thread = true;
			break;
		}
		case MACHO_LC_MAIN: {
			const struct mach_entry_point_command	*ep;

			if (cmdsize < sizeof(*ep))
				return (MACHO_E_BADCMD);
			ep = (const struct mach_entry_point_command *)
			    (image + off);
			main_entryoff = ep->entryoff;
			have_main     = true;
			break;
		}
		case MACHO_LC_BUILD_VERSION: {
			const struct mach_build_version_command	*bv;

			if (cmdsize < sizeof(*bv))
				return (MACHO_E_BADCMD);
			bv = (const struct mach_build_version_command *)
			    (image + off);
			if (bv->platform == MACHO_PLATFORM_MACOS)
				darwin_platform = true;
			break;
		}
		case MACHO_LC_LOAD_DYLINKER:
			if (cmdsize < sizeof(struct mach_dylinker_command))
				return (MACHO_E_BADCMD);
			out->needs_dyld = true;
			break;
		default:
			/* LC_SYMTAB, LC_DYSYMTAB, LC_UUID, etc.: not needed. */
			break;
		}

		off += cmdsize;
	}

	/*
	 * Stamp the syscall personality from the declared platform, before
	 * the entry is resolved, whichever entry command the image carries.
	 */
	target->t_personality = darwin_platform ?
	    TASK_PERSONALITY_DARWIN : TASK_PERSONALITY_STYLE9;

	out->image_base = base_vmaddr;

	if (have_thread) {
		out->entry = entry + load_bias;
		return (MACHO_E_OK);
	}
	if (have_main && have_base) {
		out->entry = base_vmaddr + main_entryoff;
		return (MACHO_E_OK);
	}
	return (MACHO_E_NOENTRY);
}

/*
 * Map one LC_SEGMENT_64 into the target at bias + vmaddr (bias is 0 for an
 * executable at its linked address, or the slide or dylib base; see
 * load_thin and macho_map_dylib).  No-access guard segments are skipped
 * (__PAGEZERO: initprot 0, 4 GiB vmsize).  The rest is the Mach-O part --
 * bias and initprot translation; vm_map_image, shared with kern/elf.c,
 * does the mapping, zero-fills the bss tail and borrows the kernel's own
 * frames where it can (vm/vm.h).
 */
static int
load_segment(struct task *target, const uint8_t *image, size_t image_size,
    const struct mach_segment_command_64 *sg, uint64_t bias)
{
	uint64_t	seg_va;
	uint32_t	prot;

	if (sg->initprot == 0 || sg->vmsize == 0)
		return (MACHO_E_OK);
	if (sg->filesize > sg->vmsize)
		return (MACHO_E_BADCMD);
	if (sg->fileoff + sg->filesize > image_size)
		return (MACHO_E_TRUNCATED);
	seg_va = bias + sg->vmaddr;
	if (seg_va + sg->vmsize < seg_va)
		return (MACHO_E_BADCMD);

	/*
	 * initprot, not maxprot, decides the mapping and so whether pages can
	 * be borrowed.  __DATA_CONST, where dyld writes fixups, is writable
	 * and stays copied.
	 */
	prot = VM_PROT_USER;
	if (sg->initprot & MACHO_VM_PROT_READ)
		prot |= VM_PROT_READ;
	if (sg->initprot & MACHO_VM_PROT_WRITE)
		prot |= VM_PROT_WRITE;
	if (sg->initprot & MACHO_VM_PROT_EXECUTE)
		prot |= VM_PROT_EXEC;

	switch (vm_map_image(target->t_map, target->t_pmap, seg_va,
	    sg->vmsize, sg->filesize, image + sg->fileoff, (uint8_t)prot)) {
	case VM_IMAGE_OK:
		return (MACHO_E_OK);
	case VM_IMAGE_NOMEM:
		return (MACHO_E_NOMEM);
	default:
		return (MACHO_E_MAP);
	}
}

/*
 * Map a relocatable MH_DYLIB into `target` at `bias`: every LC_SEGMENT_64
 * via load_segment, __LINKEDIT included (dyld reads the export trie and
 * chained fixups from it).  No entry point or personality stamp.
 * *out_span gets the page-rounded span used, to place the next dylib.
 */
int
macho_map_dylib(struct task *target, const void *image, size_t image_size,
    uint64_t bias, uint64_t *out_span)
{
	const struct mach_header_64	*mh;
	const uint8_t			*bytes;
	uint64_t			 span_end;
	size_t				 end;
	size_t				 off;
	uint32_t			 i;
	int				 rv;

	if (target == NULL || image == NULL || out_span == NULL)
		return (MACHO_E_TRUNCATED);
	if (image_size < sizeof(*mh))
		return (MACHO_E_TRUNCATED);

	bytes = (const uint8_t *)image;
	mh = (const struct mach_header_64 *)image;
	if (mh->magic != MACHO_MAGIC_64)
		return (MACHO_E_BADMAG);
	if (mh->cputype != MACHO_CPU_TYPE_X86_64)
		return (MACHO_E_BADCPU);
	if (mh->filetype != MACHO_MH_DYLIB)
		return (MACHO_E_BADTYPE);
	if ((uint64_t)sizeof(*mh) + mh->sizeofcmds > image_size)
		return (MACHO_E_TRUNCATED);

	span_end = bias;
	end = sizeof(*mh) + mh->sizeofcmds;
	off = sizeof(*mh);

	for (i = 0; i < mh->ncmds; i++) {
		const struct mach_load_command	*lc;
		uint32_t			 cmd;
		uint32_t			 cmdsize;

		if (off + sizeof(*lc) > end)
			return (MACHO_E_BADCMD);
		lc      = (const struct mach_load_command *)(bytes + off);
		cmd     = lc->cmd;
		cmdsize = lc->cmdsize;
		if (cmdsize < sizeof(*lc) || off + cmdsize > end)
			return (MACHO_E_BADCMD);

		if (cmd == MACHO_LC_SEGMENT_64) {
			const struct mach_segment_command_64	*sg;
			uint64_t				 seg_end;

			if (cmdsize < sizeof(*sg))
				return (MACHO_E_BADCMD);
			sg = (const struct mach_segment_command_64 *)
			    (bytes + off);
			rv = load_segment(target, bytes, image_size, sg, bias);
			if (rv != MACHO_E_OK)
				return (rv);
			seg_end = bias + sg->vmaddr + sg->vmsize;
			if (seg_end > span_end)
				span_end = seg_end;
		}

		off += cmdsize;
	}

	*out_span = (span_end - bias + PAGE_MASK) & ~(uint64_t)PAGE_MASK;
	return (MACHO_E_OK);
}

/*
 * Byte-swap a big-endian fat header field; thin Mach-O is little-endian
 * and read directly.
 */
static uint32_t
be32(uint32_t v)
{

	return (((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	    ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24));
}
