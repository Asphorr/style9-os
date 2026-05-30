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
 * Where a main image whose linked __TEXT lies outside the user window gets
 * relocated to (see load_thin).  In [VM_USER_VA_LO, VM_USER_VA_HI) and clear
 * of dyld (0x60000000), the dylib region (0x70000000+), and the user stack
 * (0x4000F000); leaves ~256 MiB of headroom below dyld for a large image.
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
 * Load a Mach-O image already resident in kernel memory into `target`.
 * A fat/universal archive is dispatched to the slice picker; a thin
 * image goes straight to the segment mapper.  Returns the entry RIP in
 * `*entry_out` and MACHO_E_OK, or a negative MACHO_E_* on failure --
 * the same contract as elf_load(), so usermode_elf_launcher can treat
 * the two formats interchangeably behind a 4-byte magic sniff.
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
 * Pick the CPU_TYPE_X86_64 slice out of a fat/universal archive and load
 * it as a thin image.  Fat headers are BIG-ENDIAN on disk regardless of
 * the slices they carry, so every field is read through be32().  The
 * selected slice is loaded by load_thin directly (not via macho_load),
 * so a pathologically nested fat-in-fat archive is simply rejected as a
 * bad thin magic rather than recursing.
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
	 * Cap the arch count: a real universal binary carries a handful of
	 * slices, and an absurd value almost certainly means we mis-sniffed
	 * a non-fat image as fat.  Bound the scan rather than trust it.
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
 * Load a thin (single-architecture) 64-bit Mach-O.  Validates the header,
 * walks the load commands bounded by sizeofcmds, maps every LC_SEGMENT_64
 * into the target address space, and resolves the entry point from
 * LC_UNIXTHREAD (rip taken verbatim) or LC_MAIN (entryoff added to the
 * mach-header base, which is how dyld computes it).  LC_UNIXTHREAD wins if
 * both are present.
 *
 * An image whose __TEXT is linked outside the user window [VM_USER_VA_LO,
 * VM_USER_VA_HI) -- e.g. a real Apple binary at 0x100000000, which we cannot
 * relink at the source -- is relocated wholesale to MACHO_IMAGE_BASE by a load
 * bias added to every segment vmaddr and to the entry.  Our own
 * -pagezero_size'd binaries already sit in the window and take a zero bias.
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
			 * The first file-offset-0, non-empty segment is __TEXT
			 * -- the image base.  If its linked vmaddr is outside
			 * the user window (a real Apple binary lives at
			 * 0x100000000), relocate the whole image down to
			 * MACHO_IMAGE_BASE: choose the bias here, before
			 * mapping __TEXT, so every segment lands biased.  Only
			 * __PAGEZERO precedes __TEXT and it is never mapped
			 * (initprot 0), so its zero bias is harmless.  dyld
			 * re-derives the slide from the biased mach-header we
			 * report, keeping its chained-fixup walk correct.
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
	 * Stamp the task's syscall personality from the platform the image
	 * declared.  A PLATFORM_MACOS LC_BUILD_VERSION opts the task into the
	 * Darwin ABI -- class-encoded syscalls routed through darwin_dispatch;
	 * anything else (including no LC_BUILD_VERSION) leaves it native
	 * style9.  Set before the entry resolves so the tag holds regardless
	 * of which entry command the binary carries.
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
 * Bring one LC_SEGMENT_64 into the target task's address space.  A
 * structural twin of elf.c's load_segment().  `bias` is added to the
 * segment's vmaddr before mapping: 0 for an MH_EXECUTE loaded at its linked
 * address, the load base for a relocatable MH_DYLIB (see macho_map_dylib):
 *	- skip no-access guard segments (__PAGEZERO has initprot 0 and a
 *	  4 GiB vmsize -- mapping it would be both pointless and ruinous),
 *	- round [bias+vmaddr, bias+vmaddr+vmsize) out to whole pages,
 *	- allocate + map each page U=1 with R/W/X from initprot,
 *	- zero the freshly-allocated frame (covers the bss tail where
 *	  vmsize > filesize),
 *	- copy filesize bytes from `image + fileoff` to the segment start
 *	  via the kernel-VA alias of the underlying frame (works even when
 *	  the segment is not user-writable -- the alias is RW through the
 *	  boot identity map, the user leaf keeps its initprot).
 */
static int
load_segment(struct task *target, const uint8_t *image, size_t image_size,
    const struct mach_segment_command_64 *sg, uint64_t bias)
{
	uint64_t	va, va_start, va_end;
	uint64_t	seg_va;
	uint64_t	pa;
	uint64_t	src_off;
	uint64_t	remaining;
	uint64_t	cur_va;
	uint64_t	page_off;
	uint64_t	chunk;
	uint8_t		*kva;
	uint32_t	prot;
	size_t		i;

	if (sg->initprot == 0 || sg->vmsize == 0)
		return (MACHO_E_OK);
	if (sg->filesize > sg->vmsize)
		return (MACHO_E_BADCMD);
	if (sg->fileoff + sg->filesize > image_size)
		return (MACHO_E_TRUNCATED);
	seg_va = bias + sg->vmaddr;
	if (seg_va + sg->vmsize < seg_va)
		return (MACHO_E_BADCMD);

	prot = VM_PROT_USER;
	if (sg->initprot & MACHO_VM_PROT_READ)
		prot |= VM_PROT_READ;
	if (sg->initprot & MACHO_VM_PROT_WRITE)
		prot |= VM_PROT_WRITE;
	if (sg->initprot & MACHO_VM_PROT_EXECUTE)
		prot |= VM_PROT_EXEC;

	va_start = seg_va & ~(uint64_t)PAGE_MASK;
	va_end   = (seg_va + sg->vmsize + PAGE_MASK) &
	    ~(uint64_t)PAGE_MASK;

	for (va = va_start; va < va_end; va += PAGE_SIZE) {
		pa = pmm_alloc_page();
		if (pa == PA_INVALID)
			return (MACHO_E_NOMEM);

		kva = (uint8_t *)pmm_kva_from_pa(pa);
		for (i = 0; i < PAGE_SIZE; i++)
			kva[i] = 0;

		if (!pmap_enter(target->t_pmap, va, pa, prot))
			return (MACHO_E_MAP);
	}

	if (!vm_map_enter(target->t_map, va_start, va_end - va_start,
	    (uint8_t)prot, VME_F_ANON))
		return (MACHO_E_MAP);

	src_off   = sg->fileoff;
	remaining = sg->filesize;
	cur_va    = seg_va;

	while (remaining > 0) {
		page_off = cur_va & PAGE_MASK;
		chunk    = PAGE_SIZE - page_off;
		if (chunk > remaining)
			chunk = remaining;

		pa = pmap_extract(target->t_pmap, cur_va & ~(uint64_t)PAGE_MASK);
		if (pa == PA_INVALID)
			return (MACHO_E_MAP);
		kva = (uint8_t *)pmm_kva_from_pa(pa & ~(uint64_t)PAGE_MASK);

		for (i = 0; i < chunk; i++)
			kva[page_off + i] = image[src_off + i];

		src_off   += chunk;
		cur_va    += chunk;
		remaining -= chunk;
	}

	return (MACHO_E_OK);
}

/*
 * Map a relocatable MH_DYLIB into `target` at `bias`.  Validates the header,
 * walks the load commands, and drops every LC_SEGMENT_64 at bias+vmaddr via
 * the shared load_segment (so __LINKEDIT -- carrying the export trie + chained
 * fixups our dyld reads back -- is mapped just like __TEXT).  No entry point,
 * no LC_MAIN, no personality stamp: a dylib is data to be bound into an
 * already-running task, not a program to enter.  *out_span gets the page-
 * rounded VA span consumed from `bias` so the caller can place the next dylib.
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
 * Read a big-endian uint32 from a fat header field.  The kernel runs
 * little-endian on x86-64, so every fat/universal field needs the swap;
 * thin Mach-O bodies are native little-endian and are read directly.
 */
static uint32_t
be32(uint32_t v)
{

	return (((v & 0x000000FFu) << 24) | ((v & 0x0000FF00u) << 8) |
	    ((v & 0x00FF0000u) >> 8) | ((v & 0xFF000000u) >> 24));
}
