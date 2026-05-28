/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * elf2macho -- host build tool that rewraps a freestanding ring-3 ELF64
 * as a thin x86-64 Mach-O (optionally a single-slice fat/universal
 * archive).  It exists because this build host has no Darwin cross
 * toolchain (no clang/ld64), yet the kernel's Mach-O loader (kern/macho.c)
 * needs a genuine, spec-shaped Mach-O to parse.  In the default and `fat`
 * modes the program inside the container is an ordinary style9 binary --
 * same libstyle9 crt0, same SYS_* numbers -- so it proves the *container*
 * path (S1) without pretending to be a Darwin-ABI binary.
 *
 * The `macos` mode additionally stamps an LC_BUILD_VERSION naming
 * PLATFORM_MACOS, which flips the loaded task to the kernel's Darwin syscall
 * personality (S2).  It is used by the freestanding user/darwinhello stub,
 * which issues genuine class-encoded Apple syscalls (write, getpid,
 * task_self_trap, exit) rather than style9 ones.  Binary-exact mach_msg /
 * MIG / a real libSystem remain later steps.
 *
 *	elf2macho       <in.elf> <out.macho>	thin x86-64 Mach-O
 *	elf2macho fat   <in.elf> <out.macho>	fat archive, one x86-64 slice
 *	elf2macho macos <in.elf> <out.macho>	thin + LC_BUILD_VERSION macOS
 *
 * The translation is mechanical: one LC_SEGMENT_64 per PT_LOAD (vmaddr,
 * vmsize, fileoff, filesize, prot all carried across verbatim) plus one
 * LC_UNIXTHREAD whose rip is e_entry.  The Mach-O header + load commands
 * sit at file offset 0 and are deliberately NOT covered by any segment:
 * the kernel loader maps strictly by segment vmaddr (exactly as elf_load
 * does), so e_entry stays valid and the header never needs to be resident.
 * That is faithful for our loader; it is not how ld64 lays out __TEXT, and
 * is not meant to feed Apple's dyld.
 *
 * Wire structs come from kern/macho.h and kern/elf.h (compiled with
 * -Ikern), so the converter and the kernel never drift -- the _Static_assert
 * size checks in those headers fire in this host compile too.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "elf.h"
#include "macho.h"

/*
 * Initial user %rsp recorded in LC_UNIXTHREAD.  Advisory only: style9's
 * launcher (arch/amd64/usermode.c build_user_arg_stack) lays down the
 * real argc/argv frame and passes its own %rsp to usermode_enter, so the
 * loader ignores this field.  Kept equal to USER_STACK_TOP for a sane,
 * spec-shaped thread state.  Mirror of arch/amd64/usermode.h.
 */
#define	USER_STACK_TOP	0x40010000ULL

#define	MAX_SEGS	16

struct localseg {
	uint64_t	vaddr;
	uint64_t	memsz;
	uint64_t	filesz;
	uint64_t	foff;
	uint32_t	flags;
};

static void
die(const char *msg)
{

	fprintf(stderr, "elf2macho: %s\n", msg);
	exit(1);
}

static void
put_be32(uint8_t *p, uint32_t v)
{

	p[0] = (uint8_t)(v >> 24);
	p[1] = (uint8_t)(v >> 16);
	p[2] = (uint8_t)(v >> 8);
	p[3] = (uint8_t)v;
}

static uint8_t *
read_file(const char *path, size_t *size_out)
{
	uint8_t	*buf;
	FILE	*f;
	long	 n;

	f = fopen(path, "rb");
	if (f == NULL)
		die("cannot open input");
	if (fseek(f, 0, SEEK_END) != 0)
		die("fseek failed");
	n = ftell(f);
	if (n < 0)
		die("ftell failed");
	rewind(f);

	buf = (uint8_t *)malloc((size_t)n);
	if (buf == NULL)
		die("out of memory reading input");
	if (fread(buf, 1, (size_t)n, f) != (size_t)n)
		die("short read on input");
	fclose(f);

	*size_out = (size_t)n;
	return (buf);
}

static void
write_file(const char *path, const uint8_t *buf, size_t size)
{
	FILE	*f;

	f = fopen(path, "wb");
	if (f == NULL)
		die("cannot open output");
	if (fwrite(buf, 1, size, f) != size)
		die("short write on output");
	fclose(f);
}

/*
 * Parse the input ELF64 into a PT_LOAD list + entry point.  Validation is
 * deliberately strict: the converter only ever sees output of our own
 * user.ld link, so anything off-pattern is a build bug worth failing on.
 */
static int
parse_elf(const uint8_t *buf, size_t size, struct localseg *segs,
    uint64_t *entry_out)
{
	const struct elf64_ehdr	*eh;
	const struct elf64_phdr	*ph;
	uint64_t		 pho;
	int			 nseg;
	uint16_t		 i;

	if (size < sizeof(*eh))
		die("input smaller than ELF header");
	eh = (const struct elf64_ehdr *)buf;

	if (eh->e_ident[0] != ELF_MAG0 || eh->e_ident[1] != ELF_MAG1 ||
	    eh->e_ident[2] != ELF_MAG2 || eh->e_ident[3] != ELF_MAG3)
		die("input is not ELF");
	if (eh->e_ident[4] != ELFCLASS64 || eh->e_ident[5] != ELFDATA2LSB)
		die("input is not ELFCLASS64/LSB");
	if (eh->e_type != ET_EXEC)
		die("input is not ET_EXEC");
	if (eh->e_machine != EM_X86_64)
		die("input is not EM_X86_64");
	if (eh->e_phentsize != sizeof(struct elf64_phdr))
		die("unexpected e_phentsize");
	if ((uint64_t)eh->e_phoff +
	    (uint64_t)eh->e_phnum * sizeof(struct elf64_phdr) > size)
		die("program headers run past end of file");

	nseg = 0;
	for (i = 0; i < eh->e_phnum; i++) {
		pho = eh->e_phoff + (uint64_t)i * sizeof(struct elf64_phdr);
		ph  = (const struct elf64_phdr *)(buf + pho);
		if (ph->p_type != PT_LOAD)
			continue;
		if (ph->p_memsz == 0)
			continue;
		if (ph->p_filesz > ph->p_memsz)
			die("PT_LOAD filesz > memsz");
		if (ph->p_offset + ph->p_filesz > size)
			die("PT_LOAD file range past end of file");
		if (nseg >= MAX_SEGS)
			die("too many PT_LOAD segments");

		segs[nseg].vaddr  = ph->p_vaddr;
		segs[nseg].memsz  = ph->p_memsz;
		segs[nseg].filesz = ph->p_filesz;
		segs[nseg].foff   = ph->p_offset;
		segs[nseg].flags  = ph->p_flags;
		nseg++;
	}
	if (nseg == 0)
		die("no PT_LOAD segments");

	*entry_out = eh->e_entry;
	return (nseg);
}

/* Map ELF PF_R/W/X to the Mach-O vm_prot bits an LC_SEGMENT_64 carries. */
static int32_t
macho_prot(uint32_t pflags)
{
	int32_t	prot;

	prot = 0;
	if (pflags & PF_R)
		prot |= MACHO_VM_PROT_READ;
	if (pflags & PF_W)
		prot |= MACHO_VM_PROT_WRITE;
	if (pflags & PF_X)
		prot |= MACHO_VM_PROT_EXECUTE;
	return (prot);
}

/* Cosmetic Mach-O segment name keyed off the ELF permission bits. */
static const char *
seg_name(uint32_t pflags)
{

	if (pflags & PF_X)
		return ("__TEXT");
	if (pflags & PF_W)
		return ("__DATA");
	return ("__RODATA");
}

/*
 * Build the whole thin Mach-O image in a freshly malloc'd buffer: header,
 * one LC_SEGMENT_64 per PT_LOAD, one LC_UNIXTHREAD, then each segment's
 * file bytes packed behind the load commands.  Returns the buffer and
 * writes its length through size_out; caller frees / wraps it.
 */
static uint8_t *
build_thin(const struct localseg *segs, int nseg, uint64_t entry,
    const uint8_t *elf, int darwin, size_t *size_out)
{
	struct mach_header_64		 mh;
	struct mach_thread_command	 tc;
	struct mach_x86_thread_state64	 ts;
	struct mach_build_version_command bv;
	uint8_t				*out;
	uint64_t			 foff[MAX_SEGS];
	uint64_t			 sizeofcmds;
	uint64_t			 hdrtotal;
	uint64_t			 total;
	uint64_t			 off;
	int				 k;

	sizeofcmds = (uint64_t)nseg * sizeof(struct mach_segment_command_64) +
	    sizeof(struct mach_thread_command) +
	    sizeof(struct mach_x86_thread_state64);
	if (darwin)
		sizeofcmds += sizeof(struct mach_build_version_command);
	hdrtotal = sizeof(struct mach_header_64) + sizeofcmds;

	/* Segment file data is packed directly after the load commands. */
	total = hdrtotal;
	for (k = 0; k < nseg; k++) {
		foff[k] = total;
		total  += segs[k].filesz;
	}

	out = (uint8_t *)calloc(1, (size_t)total);
	if (out == NULL)
		die("out of memory building Mach-O");

	memset(&mh, 0, sizeof(mh));
	mh.magic      = MACHO_MAGIC_64;
	mh.cputype    = MACHO_CPU_TYPE_X86_64;
	mh.cpusubtype = 3;			/* CPU_SUBTYPE_X86_64_ALL */
	mh.filetype   = MACHO_MH_EXECUTE;
	mh.ncmds      = (uint32_t)(darwin ? nseg + 2 : nseg + 1);
	mh.sizeofcmds = (uint32_t)sizeofcmds;
	mh.flags      = 0;
	memcpy(out, &mh, sizeof(mh));

	off = sizeof(mh);
	for (k = 0; k < nseg; k++) {
		struct mach_segment_command_64	 sg;
		const char			*nm;
		size_t				 nl;

		memset(&sg, 0, sizeof(sg));
		sg.cmd      = MACHO_LC_SEGMENT_64;
		sg.cmdsize  = sizeof(sg);
		nm = seg_name(segs[k].flags);
		nl = strlen(nm);
		if (nl > sizeof(sg.segname))
			nl = sizeof(sg.segname);
		memcpy(sg.segname, nm, nl);	/* memset above zero-pads */
		sg.vmaddr   = segs[k].vaddr;
		sg.vmsize   = segs[k].memsz;
		sg.fileoff  = foff[k];
		sg.filesize = segs[k].filesz;
		sg.maxprot  = macho_prot(segs[k].flags);
		sg.initprot = sg.maxprot;
		sg.nsects   = 0;
		sg.flags    = 0;
		memcpy(out + off, &sg, sizeof(sg));
		off += sizeof(sg);
	}

	memset(&tc, 0, sizeof(tc));
	tc.cmd     = MACHO_LC_UNIXTHREAD;
	tc.cmdsize = sizeof(tc) + sizeof(ts);
	tc.flavor  = MACHO_x86_THREAD_STATE64;
	tc.count   = MACHO_x86_THREAD_STATE64_COUNT;
	memcpy(out + off, &tc, sizeof(tc));
	off += sizeof(tc);

	memset(&ts, 0, sizeof(ts));
	ts.rip = entry;
	ts.rsp = USER_STACK_TOP;
	memcpy(out + off, &ts, sizeof(ts));
	off += sizeof(ts);

	/*
	 * S2: an LC_BUILD_VERSION naming PLATFORM_MACOS tells the kernel loader
	 * to run this task under the Darwin syscall personality.  Emitted right
	 * after LC_UNIXTHREAD; like every command it lives in [0, hdrtotal) and
	 * is not covered by any segment.
	 */
	if (darwin) {
		memset(&bv, 0, sizeof(bv));
		bv.cmd      = MACHO_LC_BUILD_VERSION;
		bv.cmdsize  = (uint32_t)sizeof(bv);
		bv.platform = MACHO_PLATFORM_MACOS;
		memcpy(out + off, &bv, sizeof(bv));
		off += sizeof(bv);
	}

	/*
	 * Lay each segment's file bytes down at the fileoff recorded in its
	 * load command; a pure-bss segment (filesz==0) copies nothing and
	 * the loader zero-fills it.
	 */
	for (k = 0; k < nseg; k++) {
		if (segs[k].filesz > 0)
			memcpy(out + foff[k], elf + segs[k].foff,
			    (size_t)segs[k].filesz);
	}

	*size_out = (size_t)total;
	return (out);
}

/*
 * Wrap a thin slice in a one-arch fat/universal archive.  The fat header
 * + fat_arch are BIG-ENDIAN on disk (Mach-O convention); the slice is
 * aligned to 2^14 == 0x4000, matching what lipo emits for x86-64.
 */
static uint8_t *
wrap_fat(const uint8_t *thin, size_t thin_size, size_t *size_out)
{
	uint8_t		*out;
	uint8_t		*p;
	uint64_t	 sliceoff;
	uint64_t	 total;

	sliceoff = 0x4000;	/* 2^14, the x86-64 fat alignment */
	total    = sliceoff + thin_size;

	out = (uint8_t *)calloc(1, (size_t)total);
	if (out == NULL)
		die("out of memory building fat archive");

	p = out;
	put_be32(p + 0, MACHO_FAT_MAGIC);		/* fat_header.magic      */
	put_be32(p + 4, 1);				/* fat_header.nfat_arch  */
	p += sizeof(struct mach_fat_header);

	put_be32(p + 0,  MACHO_CPU_TYPE_X86_64);	/* fat_arch.cputype      */
	put_be32(p + 4,  3);				/* fat_arch.cpusubtype   */
	put_be32(p + 8,  (uint32_t)sliceoff);		/* fat_arch.offset       */
	put_be32(p + 12, (uint32_t)thin_size);		/* fat_arch.size         */
	put_be32(p + 16, 14);				/* fat_arch.align (2^14) */

	memcpy(out + sliceoff, thin, thin_size);

	*size_out = (size_t)total;
	return (out);
}

int
main(int argc, char **argv)
{
	struct localseg	 segs[MAX_SEGS];
	const char	*in;
	const char	*out;
	uint8_t		*elf;
	uint8_t		*thin;
	uint8_t		*image;
	uint64_t	 entry;
	size_t		 elf_size;
	size_t		 thin_size;
	size_t		 image_size;
	int		 nseg;
	int		 fat;
	int		 darwin;

	fat    = 0;
	darwin = 0;
	if (argc == 4 && strcmp(argv[1], "fat") == 0) {
		fat = 1;
		in  = argv[2];
		out = argv[3];
	} else if (argc == 4 && strcmp(argv[1], "macos") == 0) {
		darwin = 1;
		in     = argv[2];
		out    = argv[3];
	} else if (argc == 3) {
		in  = argv[1];
		out = argv[2];
	} else {
		fprintf(stderr,
		    "usage: %s [fat|macos] <in.elf> <out.macho>\n", argv[0]);
		return (2);
	}

	elf  = read_file(in, &elf_size);
	nseg = parse_elf(elf, elf_size, segs, &entry);

	thin = build_thin(segs, nseg, entry, elf, darwin, &thin_size);

	if (fat) {
		image = wrap_fat(thin, thin_size, &image_size);
		free(thin);
	} else {
		image      = thin;
		image_size = thin_size;
	}

	write_file(out, image, image_size);

	fprintf(stderr,
	    "elf2macho: %s -> %s (%s, %d segment%s, entry=0x%llx, %zu bytes)\n",
	    in, out, fat ? "fat/x86_64" :
	    (darwin ? "thin/x86_64+macos" : "thin/x86_64"), nseg,
	    nseg == 1 ? "" : "s", (unsigned long long)entry, image_size);

	free(elf);
	free(image);
	return (0);
}
