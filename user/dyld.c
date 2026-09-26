/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * dyld -- style9's clean-room dynamic linker, not Apple's: written to the
 * published Mach-O format (LC_LOAD_DYLINKER, LC_DYLD_CHAINED_FIXUPS, the
 * export trie).  Built as a freestanding MH_EXECUTE by clang + ld64.lld at a
 * fixed base (0x60000000), so it needs no self-relocation and the kernel
 * loader maps it as-is.  No libSystem: it issues class-encoded Darwin
 * syscalls directly.
 *
 * The kernel maps the main image plus this dyld into one task and enters
 * _dyld_start with a dyld4-shaped handoff stack:
 *
 *	[ main mach_header ]	<- %rsp
 *	[ argc ]
 *	[ argv[0] ... ][ NULL ]
 *	[ envp[0] ... ][ NULL ]
 *	[ apple NULL ]
 *
 * dyld_main parses the main image's load commands, maps the closure of its
 * LC_LOAD_DYLIBs through the map_image backchannel (our stand-in for
 * open()+mmap()), applies every image's fixups -- chained fixups, or the
 * classic LC_DYLD_INFO opcode streams -- binding imports against the
 * dependencies' export tries, and jumps to the main image's LC_MAIN entry.
 *
 * dyld must carry no bound or rebased pointers of its own, since nobody runs
 * its fixups: no global pointer tables, no `static const char *x = "..."',
 * only code, immediates and RIP-relative arrays and literals.  (A stray
 * rebase would be harmless at slide 0; a bind would be fatal.)
 *
 * Compiled -fno-builtin so the compiler cannot lower a body into a libc call.
 */

/* Fixed-width types from compiler builtins: no SDK or <stdint.h> needed. */
typedef __UINT8_TYPE__		uint8_t;
typedef __UINT16_TYPE__		uint16_t;
typedef __UINT32_TYPE__		uint32_t;
typedef __UINT64_TYPE__		uint64_t;
typedef __INT32_TYPE__		int32_t;
typedef __INT64_TYPE__		int64_t;
typedef __UINTPTR_TYPE__	uintptr_t;

/* ---- Mach-O on-disk structures (subset we parse) ------------------------ */

#define	MACHO_MAGIC_64		0xFEEDFACFu

#define	LC_REQ_DYLD		0x80000000u
#define	LC_SEGMENT_64		0x19
#define	LC_LOAD_DYLIB		0xC
#define	LC_MAIN			(0x28u | LC_REQ_DYLD)
#define	LC_DYLD_CHAINED_FIXUPS	(0x34u | LC_REQ_DYLD)
#define	LC_DYLD_EXPORTS_TRIE	(0x33u | LC_REQ_DYLD)
#define	LC_DYLD_INFO		0x22u
#define	LC_DYLD_INFO_ONLY	(0x22u | LC_REQ_DYLD)

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

struct load_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
};

struct segment_command_64 {
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

struct dylib_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint32_t	name;		/* lc_str: byte offset from cmd start */
	uint32_t	timestamp;
	uint32_t	current_version;
	uint32_t	compat_version;
};

struct entry_point_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint64_t	entryoff;
	uint64_t	stacksize;
};

struct linkedit_data_command {
	uint32_t	cmd;
	uint32_t	cmdsize;
	uint32_t	dataoff;
	uint32_t	datasize;
};

/*
 * The two 64-bit chained-pointer formats we bind.  A DYLD_CHAINED_PTR_64
 * rebase holds an unslid vmaddr (our ld64.lld binaries); _OFFSET holds an
 * offset from the image's mach_header (Apple's linker for x86-64, e.g.
 * figlet).  Bind records are identical; only the rebase target differs.
 */
#define	DYLD_CHAINED_PTR_64		2
#define	DYLD_CHAINED_PTR_64_OFFSET	6
#define	DYLD_CHAINED_PTR_START_NONE	0xFFFF

/* ---- syscalls ----------------------------------------------------------- */

/*
 * One class-encoded Darwin syscall, up to three args (rdi, rsi, rdx).  The
 * class is the high byte of `nr' (0x2000000 = BSD); `syscall' clobbers
 * rcx/r11.
 */
static long
dsys(long nr, long a, long b, long c)
{
	long	ret;

	__asm__ __volatile__("syscall"
	    : "=a"(ret)
	    : "a"(nr), "D"(a), "S"(b), "d"(c)
	    : "rcx", "r11", "memory");
	return (ret);
}

/*
 * map_image (style9-private class 0x2A): map the dylib at `path' into this
 * task; returns its base, or 0 on failure.  Failure uses the BSD carry
 * convention, so the carry flag is captured right after `syscall'.
 */
static uint64_t
map_image(const char *path)
{
	uint64_t	ret;
	unsigned char	cf;

	__asm__ __volatile__(
	    "syscall\n\t"
	    "setc %1\n"
	    : "=a"(ret), "=r"(cf)
	    : "a"((long)0x2A000001), "D"((long)path)
	    : "rcx", "r11", "memory");
	return (cf ? (uint64_t)0 : ret);
}

/* ---- diagnostics (write to stderr; no libc) ----------------------------- */

static unsigned long
d_strlen(const char *s)
{
	unsigned long	n;

	n = 0;
	while (s[n] != '\0')
		n++;
	return (n);
}

static void
d_puts(const char *s)
{

	dsys(0x2000004, 2, (long)s, (long)d_strlen(s));	/* write(2, s, len) */
}

static void
d_puthex(uint64_t v)
{
	char	buf[19];
	int	i;
	int	d;

	buf[0] = '0';
	buf[1] = 'x';
	for (i = 0; i < 16; i++) {
		d = (int)((v >> ((15 - i) * 4)) & 0xF);
		buf[2 + i] = (char)(d < 10 ? '0' + d : 'a' + (d - 10));
	}
	buf[18] = '\n';
	dsys(0x2000004, 2, (long)buf, 19);
}

/* ---- little-endian readers (alignment-safe) ----------------------------- */

static uint16_t
rd16(const uint8_t *p)
{

	return ((uint16_t)(p[0] | (p[1] << 8)));
}

static uint32_t
rd32(const uint8_t *p)
{

	return ((uint32_t)p[0] | ((uint32_t)p[1] << 8) |
	    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24));
}

static uint64_t
rd64(const uint8_t *p)
{

	return ((uint64_t)rd32(p) | ((uint64_t)rd32(p + 4) << 32));
}

static uint64_t
uleb(const uint8_t **pp, const uint8_t *end)
{
	const uint8_t	*p;
	uint64_t	 r;
	int		 shift;
	uint8_t		 b;

	p = *pp;
	r = 0;
	shift = 0;
	while (p < end) {
		b = *p++;
		r |= (uint64_t)(b & 0x7F) << shift;
		if ((b & 0x80) == 0)
			break;
		shift += 7;
		if (shift >= 64)
			break;
	}
	*pp = p;
	return (r);
}

/* Signed LEB128 -- the form a classic bind stream uses for SET_ADDEND_SLEB. */
static int64_t
sleb(const uint8_t **pp, const uint8_t *end)
{
	const uint8_t	*p;
	int64_t		 r;
	int		 shift;
	uint8_t		 b;

	p = *pp;
	r = 0;
	shift = 0;
	b = 0;
	while (p < end) {
		b = *p++;
		r |= (int64_t)(b & 0x7F) << shift;
		shift += 7;
		if ((b & 0x80) == 0)
			break;
		if (shift >= 64)
			break;
	}
	if (shift < 64 && (b & 0x40) != 0)
		r |= -((int64_t)1 << shift);	/* sign-extend */
	*pp = p;
	return (r);
}

/* ---- parsed image ------------------------------------------------------- */

#define	IMAGE_MAX_SEGS	8
#define	IMAGE_MAX_DEPS	16	/* LC_LOAD_DYLIB entries we record per image  */
#define	LINKSET_MAX	8	/* images in one closure: main + dependencies */

struct image {
	uint64_t	mh;		/* runtime mach_header address       */
	uint64_t	slide;		/* mh - preferred __TEXT vmaddr      */
	uint64_t	text_vmaddr;	/* preferred vmaddr of fileoff-0 seg */
	uint64_t	entryoff;	/* LC_MAIN                           */
	const uint8_t	*fixups;	/* runtime ptr to chained fixups blob */
	uint64_t	 fixups_size;
	const uint8_t	*trie;		/* runtime ptr to export trie blob   */
	uint64_t	 trie_size;
	const uint8_t	*rebase;	/* classic LC_DYLD_INFO rebase opcodes */
	uint64_t	 rebase_size;
	const uint8_t	*bind;		/* classic non-lazy bind opcodes      */
	uint64_t	 bind_size;
	const uint8_t	*lazy;		/* classic lazy bind opcodes          */
	uint64_t	 lazy_size;
	const char	*deps[IMAGE_MAX_DEPS];	/* LC_LOAD_DYLIB names (runtime) */
	int		 ndeps;
	int		 nsegs;
	int		 have_text;
	struct {
		uint64_t	vmaddr;
		uint64_t	fileoff;
		uint64_t	filesize;
	} segs[IMAGE_MAX_SEGS];
};

/*
 * The loaded set: index 0 is the main image, 1..n-1 are mapped dependencies.
 * path[i] is the LC_LOAD_DYLIB string a dependency was mapped under (NULL for
 * the main image), so a bind's lib_ordinal -> dependency name -> loaded image
 * resolves by a string compare against this table.
 */
struct linkset {
	struct image	im[LINKSET_MAX];
	const char	*path[LINKSET_MAX];
	int		 n;
};

/* Map a file offset (e.g. a __LINKEDIT dataoff) to its runtime address. */
static uint64_t
fileoff_to_runtime(const struct image *im, uint64_t fo)
{
	int	i;

	for (i = 0; i < im->nsegs; i++) {
		if (fo >= im->segs[i].fileoff &&
		    fo < im->segs[i].fileoff + im->segs[i].filesize) {
			return (im->mh + (im->segs[i].vmaddr - im->text_vmaddr) +
			    (fo - im->segs[i].fileoff));
		}
	}
	return (0);
}

static void
parse_image(uint64_t mh, struct image *im)
{
	const struct mach_header_64	*h;
	const uint8_t			*base;
	uint64_t			 off;
	uint32_t			 cf_dataoff, cf_datasize;
	uint32_t			 tr_dataoff, tr_datasize;
	uint32_t			 rb_off, rb_size;
	uint32_t			 bd_off, bd_size;
	uint32_t			 lz_off, lz_size;
	uint32_t			 i;

	h = (const struct mach_header_64 *)(uintptr_t)mh;
	base = (const uint8_t *)(uintptr_t)mh;

	im->mh = mh;
	im->slide = 0;
	im->text_vmaddr = 0;
	im->entryoff = 0;
	im->fixups = 0;
	im->fixups_size = 0;
	im->trie = 0;
	im->trie_size = 0;
	im->rebase = 0;
	im->rebase_size = 0;
	im->bind = 0;
	im->bind_size = 0;
	im->lazy = 0;
	im->lazy_size = 0;
	im->ndeps = 0;
	im->nsegs = 0;
	im->have_text = 0;
	cf_dataoff = cf_datasize = 0;
	tr_dataoff = tr_datasize = 0;
	rb_off = rb_size = 0;
	bd_off = bd_size = 0;
	lz_off = lz_size = 0;

	off = sizeof(*h);
	for (i = 0; i < h->ncmds; i++) {
		const struct load_command	*lc;

		lc = (const struct load_command *)(base + off);
		switch (lc->cmd) {
		case LC_SEGMENT_64: {
			const struct segment_command_64	*sg;

			sg = (const struct segment_command_64 *)lc;
			if (im->nsegs < IMAGE_MAX_SEGS) {
				im->segs[im->nsegs].vmaddr = sg->vmaddr;
				im->segs[im->nsegs].fileoff = sg->fileoff;
				im->segs[im->nsegs].filesize = sg->filesize;
				im->nsegs++;
			}
			if (sg->fileoff == 0 && sg->filesize > 0 &&
			    !im->have_text) {
				im->text_vmaddr = sg->vmaddr;
				im->have_text = 1;
			}
			break;
		}
		case LC_DYLD_CHAINED_FIXUPS: {
			const struct linkedit_data_command	*ld;

			ld = (const struct linkedit_data_command *)lc;
			cf_dataoff = ld->dataoff;
			cf_datasize = ld->datasize;
			break;
		}
		case LC_DYLD_EXPORTS_TRIE: {
			const struct linkedit_data_command	*ld;

			ld = (const struct linkedit_data_command *)lc;
			tr_dataoff = ld->dataoff;
			tr_datasize = ld->datasize;
			break;
		}
		case LC_DYLD_INFO:
		case LC_DYLD_INFO_ONLY: {
			const uint8_t	*q;

			/*
			 * Classic (pre-chained-fixups) metadata, as in libgmp:
			 * the rebase/bind/lazy opcode streams and, absent an
			 * LC_DYLD_EXPORTS_TRIE, the export trie.  Offsets are
			 * dyld_info_command's (loader.h).
			 */
			q = (const uint8_t *)lc;
			rb_off  = rd32(q + 8);
			rb_size = rd32(q + 12);
			bd_off  = rd32(q + 16);
			bd_size = rd32(q + 20);
			lz_off  = rd32(q + 32);
			lz_size = rd32(q + 36);
			if (tr_datasize == 0) {
				tr_dataoff  = rd32(q + 40);
				tr_datasize = rd32(q + 44);
			}
			break;
		}
		case LC_LOAD_DYLIB: {
			const struct dylib_command	*dl;

			dl = (const struct dylib_command *)lc;
			if (im->ndeps < IMAGE_MAX_DEPS)
				im->deps[im->ndeps++] =
				    (const char *)((const uint8_t *)lc +
				    dl->name);
			break;
		}
		case LC_MAIN: {
			const struct entry_point_command	*ep;

			ep = (const struct entry_point_command *)lc;
			im->entryoff = ep->entryoff;
			break;
		}
		default:
			break;
		}
		off += lc->cmdsize;
	}

	im->slide = mh - im->text_vmaddr;
	if (cf_datasize != 0) {
		im->fixups = (const uint8_t *)(uintptr_t)
		    fileoff_to_runtime(im, cf_dataoff);
		im->fixups_size = cf_datasize;
	}
	if (tr_datasize != 0) {
		im->trie = (const uint8_t *)(uintptr_t)
		    fileoff_to_runtime(im, tr_dataoff);
		im->trie_size = tr_datasize;
	}
	if (rb_size != 0) {
		im->rebase = (const uint8_t *)(uintptr_t)
		    fileoff_to_runtime(im, rb_off);
		im->rebase_size = rb_size;
	}
	if (bd_size != 0) {
		im->bind = (const uint8_t *)(uintptr_t)
		    fileoff_to_runtime(im, bd_off);
		im->bind_size = bd_size;
	}
	if (lz_size != 0) {
		im->lazy = (const uint8_t *)(uintptr_t)
		    fileoff_to_runtime(im, lz_off);
		im->lazy_size = lz_size;
	}
}

/* ---- export trie -------------------------------------------------------- */

/*
 * Resolve `sym' in the export trie at [trie, end): the export's offset from
 * the image base, or 0 if absent.  Follows edges that prefix the rest of
 * the symbol to a terminal node and reads its (flags, address) pair,
 * assuming a regular export (re-exports and resolvers are not handled).
 */
static uint64_t
trie_lookup(const uint8_t *trie, const uint8_t *end, const char *sym)
{
	const uint8_t	*p;
	const char	*s;

	if (trie == 0 || trie >= end)
		return (0);
	p = trie;
	s = sym;
	for (;;) {
		const uint8_t	*child;
		uint64_t	 term_size;
		uint8_t		 nchild;
		uint8_t		 ci;
		const uint8_t	*next_node;

		if (p >= end)
			return (0);
		term_size = uleb(&p, end);
		if (*s == '\0') {
			if (term_size == 0)
				return (0);
			(void)uleb(&p, end);		/* flags  */
			return (uleb(&p, end));		/* address */
		}
		child = p + term_size;
		if (child >= end)
			return (0);
		nchild = *child++;
		next_node = 0;
		for (ci = 0; ci < nchild; ci++) {
			const char	*edge;
			const uint8_t	*ap;
			uint64_t	 k;
			uint64_t	 elen;
			uint64_t	 child_off;

			edge = (const char *)child;
			k = 0;
			while (edge[k] != '\0' && s[k] != '\0' &&
			    edge[k] == s[k])
				k++;
			elen = 0;
			while (edge[elen] != '\0')
				elen++;
			ap = child + elen + 1;
			child_off = uleb(&ap, end);
			if (edge[k] == '\0') {		/* whole edge matched */
				s += elen;
				next_node = trie + child_off;
				break;
			}
			child = ap;			/* try the next child */
		}
		if (next_node == 0)
			return (0);
		p = next_node;
	}
}

/* ---- dependency resolution ---------------------------------------------- */

static int
streq(const char *a, const char *b)
{

	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return (*a == *b);
}

/*
 * Index of the loaded image registered under `path`, or -1.  The main image
 * (slot 0) carries a NULL path, so it never matches a dependency name.
 */
static int
find_path(const struct linkset *ls, const char *path)
{
	int	i;

	if (path == 0)
		return (-1);
	for (i = 0; i < ls->n; i++)
		if (ls->path[i] != 0 && streq(ls->path[i], path))
			return (i);
	return (-1);
}

/*
 * Resolve import `name' of image `im' to a runtime address, or 0.  A
 * positive lib_ordinal is a 1-based index into the image's own LC_LOAD_DYLIB
 * list (two-level namespace), searched alone; other ordinals -- self (0),
 * flat lookup (0xFE) -- search the whole closure.  So one bind table can
 * draw from several dylibs (gfactor from libgmp and libSystem).
 */
static uint64_t
resolve_sym(const struct linkset *ls, const struct image *im,
    uint64_t lib_ord, const char *name)
{
	const struct image	*lib;
	uint64_t		 sym;
	int			 i;
	int			 li;

	if (lib_ord >= 1 && (int)lib_ord <= im->ndeps) {
		li = find_path(ls, im->deps[lib_ord - 1]);
		if (li < 0)
			return (0);
		lib = &ls->im[li];
		sym = trie_lookup(lib->trie, lib->trie + lib->trie_size, name);
		return (sym == 0 ? (uint64_t)0 : lib->mh + sym);
	}

	for (i = 0; i < ls->n; i++) {
		lib = &ls->im[i];
		sym = trie_lookup(lib->trie, lib->trie + lib->trie_size, name);
		if (sym != 0)
			return (lib->mh + sym);
	}
	return (0);
}

/* ---- chained fixups ----------------------------------------------------- */

/*
 * Apply the image's LC_DYLD_CHAINED_FIXUPS chain: rebases add the slide,
 * binds go through resolve_sym() by each import's lib_ordinal.
 */
static void
apply_fixups(const struct image *im, const struct linkset *ls)
{
	const uint8_t	*blob;
	const uint8_t	*starts;
	const char	*symbols;
	const uint8_t	*imports;
	uint32_t	 starts_off;
	uint32_t	 imports_off;
	uint32_t	 symbols_off;
	uint32_t	 seg_count;
	uint32_t	 s;

	if (im->fixups == 0)
		return;
	blob = im->fixups;
	starts_off  = rd32(blob + 4);
	imports_off = rd32(blob + 8);
	symbols_off = rd32(blob + 12);
	starts  = blob + starts_off;
	imports = blob + imports_off;
	symbols = (const char *)(blob + symbols_off);
	seg_count = rd32(starts + 0);

	for (s = 0; s < seg_count; s++) {
		const uint8_t	*sis;
		uint64_t	 segment_offset;
		uint32_t	 sio;
		uint16_t	 pointer_format;
		uint16_t	 page_size;
		uint16_t	 page_count;
		uint16_t	 pi;

		sio = rd32(starts + 4 + s * 4);
		if (sio == 0)
			continue;
		sis = starts + sio;
		page_size      = rd16(sis + 4);
		pointer_format = rd16(sis + 6);
		segment_offset = rd64(sis + 8);
		page_count     = rd16(sis + 20);
		if (pointer_format != DYLD_CHAINED_PTR_64 &&
		    pointer_format != DYLD_CHAINED_PTR_64_OFFSET) {
			d_puts("dyld: unsupported pointer_format\n");
			continue;
		}

		for (pi = 0; pi < page_count; pi++) {
			uint64_t	addr;
			uint16_t	start;
			int		guard;

			start = rd16(sis + 22 + pi * 2);
			if (start == DYLD_CHAINED_PTR_START_NONE)
				continue;
			addr = im->mh + segment_offset +
			    (uint64_t)pi * page_size + start;
			for (guard = 0; guard < 100000; guard++) {
				uint64_t	*slot;
				uint64_t	 val;
				uint64_t	 next;

				slot = (uint64_t *)(uintptr_t)addr;
				val = *slot;
				next = (val >> 51) & 0xFFF;
				if ((val >> 63) & 1) {		/* bind */
					const char	*name;
					uint64_t	 imp_index;
					uint64_t	 addend;
					uint64_t	 noff;
					uint64_t	 lib_ord;
					uint64_t	 base;
					uint32_t	 imp;
					int		 weak;

					imp_index = val & 0xFFFFFF;
					addend  = (val >> 24) & 0xFF;
					imp = rd32(imports + imp_index * 4);
					lib_ord = imp & 0xFF;
					weak = (int)((imp >> 8) & 1);
					noff = (imp >> 9) & 0x7FFFFF;
					name = symbols + noff;
					base = resolve_sym(ls, im, lib_ord, name);
					if (base == 0) {
						if (!weak) {
							d_puts("dyld: unresolved ");
							d_puts(name);
							d_puts("\n");
						}
						*slot = 0;
					} else {
						*slot = base + addend;
					}
				} else {			/* rebase */
					uint64_t	high8;
					uint64_t	target;
					uint64_t	value;

					target = val & 0xFFFFFFFFFULL;
					high8  = (val >> 36) & 0xFF;
					/*
					 * PTR_64: unslid vmaddr + slide.
					 * PTR_64_OFFSET: runtime base + offset.
					 * high8 goes in the top byte.
					 */
					if (pointer_format ==
					    DYLD_CHAINED_PTR_64_OFFSET)
						value = im->mh + target;
					else
						value = target + im->slide;
					*slot = value | (high8 << 56);
				}
				if (next == 0)
					break;
				addr += next * 4;
			}
		}
	}
}

/* ---- classic LC_DYLD_INFO fixups (rebase + bind opcode streams) --------- */

/* Resolve and patch one classic bind at runtime address `addr`. */
static void
bind_one(const struct image *im, const struct linkset *ls, uint64_t lib_ord,
    const char *name, int64_t addend, int weak, uint64_t addr)
{
	uint64_t	*slot;
	uint64_t	 base;

	slot = (uint64_t *)(uintptr_t)addr;
	base = resolve_sym(ls, im, lib_ord, name);
	if (base == 0) {
		if (!weak) {
			d_puts("dyld: unresolved ");
			d_puts(name != 0 ? name : "(null)");
			d_puts("\n");
		}
		*slot = 0;
	} else {
		*slot = base + (uint64_t)addend;
	}
}

/*
 * Run a classic rebase opcode stream: slide every pointer the dylib's own
 * segments hold (libgmp's GOT and tables).  The rebase type is ignored;
 * x86-64 dylibs emit only POINTER.
 */
static void
apply_rebases(const struct image *im)
{
	const uint8_t	*p;
	const uint8_t	*end;
	uint64_t	 addr;
	uint64_t	 count;
	uint64_t	 skip;
	uint64_t	 i;
	uint8_t		 op;
	uint8_t		 imm;

	if (im->rebase == 0 || im->rebase_size == 0)
		return;
	p = im->rebase;
	end = p + im->rebase_size;
	addr = im->mh;
	while (p < end) {
		op  = (uint8_t)(*p & 0xF0);
		imm = (uint8_t)(*p & 0x0F);
		p++;
		switch (op) {
		case 0x00:		/* DONE (separator / padding) */
		case 0x10:		/* SET_TYPE_IMM */
			break;
		case 0x20:		/* SET_SEGMENT_AND_OFFSET_ULEB */
			addr = im->segs[imm].vmaddr + im->slide + uleb(&p, end);
			break;
		case 0x30:		/* ADD_ADDR_ULEB */
			addr += uleb(&p, end);
			break;
		case 0x40:		/* ADD_ADDR_IMM_SCALED */
			addr += (uint64_t)imm * 8;
			break;
		case 0x50:		/* DO_REBASE_IMM_TIMES */
			for (i = 0; i < imm; i++) {
				*(uint64_t *)(uintptr_t)addr += im->slide;
				addr += 8;
			}
			break;
		case 0x60:		/* DO_REBASE_ULEB_TIMES */
			count = uleb(&p, end);
			for (i = 0; i < count; i++) {
				*(uint64_t *)(uintptr_t)addr += im->slide;
				addr += 8;
			}
			break;
		case 0x70:		/* DO_REBASE_ADD_ADDR_ULEB */
			*(uint64_t *)(uintptr_t)addr += im->slide;
			addr += 8 + uleb(&p, end);
			break;
		case 0x80:		/* DO_REBASE_ULEB_TIMES_SKIPPING_ULEB */
			count = uleb(&p, end);
			skip = uleb(&p, end);
			for (i = 0; i < count; i++) {
				*(uint64_t *)(uintptr_t)addr += im->slide;
				addr += 8 + skip;
			}
			break;
		default:
			d_puts("dyld: bad rebase op\n");
			return;
		}
	}
}

/*
 * Run a classic bind or lazy-bind opcode stream.  Each DO_BIND resolves the
 * current symbol by the last selected ordinal and patches the pointer.  DONE
 * is only a separator (the lazy stream has one per symbol), so this runs to
 * `end'; lazy binds are resolved eagerly here.
 */
static void
apply_binds(const struct image *im, const struct linkset *ls,
    const uint8_t *p, uint64_t size)
{
	const uint8_t	*end;
	const char	*sym;
	uint64_t	 addr;
	uint64_t	 lib_ord;
	uint64_t	 count;
	uint64_t	 skip;
	uint64_t	 i;
	int64_t		 addend;
	uint8_t		 op;
	uint8_t		 imm;
	int		 weak;

	if (p == 0 || size == 0)
		return;
	end = p + size;
	sym = 0;
	addr = im->mh;
	lib_ord = 0;
	addend = 0;
	weak = 0;
	while (p < end) {
		op  = (uint8_t)(*p & 0xF0);
		imm = (uint8_t)(*p & 0x0F);
		p++;
		switch (op) {
		case 0x00:		/* DONE (separator) */
			break;
		case 0x10:		/* SET_DYLIB_ORDINAL_IMM */
			lib_ord = imm;
			break;
		case 0x20:		/* SET_DYLIB_ORDINAL_ULEB */
			lib_ord = uleb(&p, end);
			break;
		case 0x30:		/* SET_DYLIB_SPECIAL_IMM */
			lib_ord = 0xFE;	/* self/flat -> search the closure */
			break;
		case 0x40:		/* SET_SYMBOL_TRAILING_FLAGS_IMM */
			weak = (int)(imm & 1);		/* WEAK_IMPORT */
			sym = (const char *)p;
			while (p < end && *p != 0)
				p++;
			if (p < end)
				p++;			/* skip the NUL */
			break;
		case 0x50:		/* SET_TYPE_IMM */
			break;
		case 0x60:		/* SET_ADDEND_SLEB */
			addend = sleb(&p, end);
			break;
		case 0x70:		/* SET_SEGMENT_AND_OFFSET_ULEB */
			addr = im->segs[imm].vmaddr + im->slide + uleb(&p, end);
			break;
		case 0x80:		/* ADD_ADDR_ULEB */
			addr += uleb(&p, end);
			break;
		case 0x90:		/* DO_BIND */
			bind_one(im, ls, lib_ord, sym, addend, weak, addr);
			addr += 8;
			break;
		case 0xA0:		/* DO_BIND_ADD_ADDR_ULEB */
			bind_one(im, ls, lib_ord, sym, addend, weak, addr);
			addr += 8 + uleb(&p, end);
			break;
		case 0xB0:		/* DO_BIND_ADD_ADDR_IMM_SCALED */
			bind_one(im, ls, lib_ord, sym, addend, weak, addr);
			addr += 8 + (uint64_t)imm * 8;
			break;
		case 0xC0:		/* DO_BIND_ULEB_TIMES_SKIPPING_ULEB */
			count = uleb(&p, end);
			skip = uleb(&p, end);
			for (i = 0; i < count; i++) {
				bind_one(im, ls, lib_ord, sym, addend, weak,
				    addr);
				addr += 8 + skip;
			}
			break;
		default:
			d_puts("dyld: bad bind op\n");
			return;
		}
	}
}

/*
 * Link one image: chained fixups when present (our libSystem, the Apple
 * executables), otherwise the classic LC_DYLD_INFO opcode streams (libgmp).
 * Rebases first, then non-lazy and lazy binds.
 */
static void
link_image(const struct image *im, const struct linkset *ls)
{

	if (im->fixups != 0) {
		apply_fixups(im, ls);
		return;
	}
	apply_rebases(im);
	apply_binds(im, ls, im->bind, im->bind_size);
	apply_binds(im, ls, im->lazy, im->lazy_size);
}

/* ---- entry -------------------------------------------------------------- */

/*
 * dyld_main: the C body of the linker.  `sp' is the handoff stack pointer
 * from _dyld_start, pointing at [main_mh][argc][argv...].
 */
void
dyld_main(uint64_t *sp)
{
	struct linkset			 ls;
	const struct mach_header_64	*mh;
	char				**argv;
	char				**envp;
	char				**apple;
	char				**w;
	uint64_t			 main_mh;
	uint64_t			 base;
	uint64_t			 entry;
	uint64_t			 exit_addr;
	uint64_t			 set_addr;
	int				 argc;
	int				 i;
	int				 k;
	int				 rc;

	main_mh = sp[0];
	argc = (int)sp[1];
	argv = (char **)&sp[2];
	envp = argv + argc + 1;
	w = envp;
	while (*w != 0)
		w++;
	apple = w + 1;

	d_puts("dyld: link main_mh=");
	d_puthex(main_mh);

	mh = (const struct mach_header_64 *)(uintptr_t)main_mh;
	if (mh->magic != MACHO_MAGIC_64) {
		d_puts("dyld: bad main magic\n");
		dsys(0x2000001, 71, 0, 0);
	}

	/* Slot 0 is the main image; it owns no dependency path. */
	ls.n = 1;
	ls.path[0] = 0;
	parse_image(main_mh, &ls.im[0]);
	if (ls.im[0].ndeps == 0) {
		d_puts("dyld: main names no LC_LOAD_DYLIB\n");
		dsys(0x2000001, 72, 0, 0);
	}

	/*
	 * Map the dependency closure.  `ls' is its own worklist: ls.n grows as
	 * images are mapped and the loop reaches them too.  A path already
	 * mapped is reused, so libgmp's libSystem is main's copy.
	 */
	for (i = 0; i < ls.n; i++) {
		for (k = 0; k < ls.im[i].ndeps; k++) {
			const char	*path;

			path = ls.im[i].deps[k];
			if (find_path(&ls, path) >= 0)
				continue;
			if (ls.n >= LINKSET_MAX) {
				d_puts("dyld: dependency closure too large\n");
				dsys(0x2000001, 74, 0, 0);
			}
			base = map_image(path);
			if (base == 0) {
				d_puts("dyld: map_image failed for ");
				d_puts(path);
				d_puts("\n");
				dsys(0x2000001, 73, 0, 0);
			}
			d_puts("dyld: mapped ");
			d_puts(path);
			d_puts(" @ ");
			d_puthex(base);
			parse_image(base, &ls.im[ls.n]);
			ls.path[ls.n] = path;
			ls.n++;
		}
	}

	/*
	 * Apply every image's fixups.  Order does not matter: a bind reads only
	 * its target's export trie, parsed above, not its applied state.
	 */
	for (i = 0; i < ls.n; i++)
		link_image(&ls.im[i], &ls);

	/*
	 * Give the C library the program name before main.  Darwin has no crt0
	 * for this; libSystem's initialiser takes argv[0] off this same stack.
	 * Without it diagnostics carry the wrong program name.
	 */
	set_addr = resolve_sym(&ls, &ls.im[0], 0xFE, "_setprogname");
	if (set_addr != 0 && argc > 0)
		((void (*)(const char *))(uintptr_t)set_addr)(argv[0]);

	/*
	 * Likewise the environment: libSystem's `environ' must point at the
	 * envp the kernel wrote after argv before main reads a variable.
	 */
	set_addr = resolve_sym(&ls, &ls.im[0], 0xFE, "_s9_environ_init");
	if (set_addr != 0)
		((void (*)(char **))(uintptr_t)set_addr)(envp);

	entry = main_mh + ls.im[0].entryoff;
	d_puts("dyld: enter main @ ");
	d_puthex(entry);

	rc = ((int (*)(int, char **, char **, char **))(uintptr_t)entry)(
	    argc, argv, envp, apple);

	/*
	 * main returned.  Under LC_MAIN that means exit(3), which runs the
	 * atexit handlers first; coreutils flushes its output from one, so the
	 * raw syscall would lose buffered output.  Call the closure's exit(),
	 * falling back to the syscall if there is none.
	 */
	exit_addr = resolve_sym(&ls, &ls.im[0], 0xFE, "_exit");
	if (exit_addr != 0)
		((void (*)(int))(uintptr_t)exit_addr)(rc);

	dsys(0x2000001, rc, 0, 0);	/* fallback: raw _exit if no exit(3) */
	for (;;)
		;
}

/*
 * _dyld_start: the kernel's entry point.  Pass %rsp (the handoff stack) as
 * the argument, 16-align, and call dyld_main, which does not return.
 */
__asm__(
	".text\n"
	".globl _dyld_start\n"
	"_dyld_start:\n"
	"\tmovq %rsp, %rdi\n"
	"\tandq $-16, %rsp\n"
	"\tcall _dyld_main\n"
	"\tud2\n"
);
