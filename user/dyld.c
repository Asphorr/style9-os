/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * dyld -- style9's clean-room dynamic linker for the S4 Darwin rung.  This is
 * NOT Apple's dyld: an original implementation written to the published Mach-O
 * format (LC_LOAD_DYLINKER, LC_DYLD_CHAINED_FIXUPS, the export trie).  Built as
 * a freestanding MH_EXECUTE by the real Darwin toolchain (clang + ld64.lld) at
 * a fixed base (0x60000000) so it needs no self-relocation at load and the
 * existing kernel loader maps it as-is.  No libSystem: it issues style9's
 * class-encoded Darwin syscalls directly.
 *
 * The kernel maps the main image plus this dyld into one task and enters here
 * (_dyld_start) with a dyld4-shaped handoff stack:
 *
 *	[ main mach_header ]	<- %rsp
 *	[ argc ]
 *	[ argv[0] ... ][ NULL ]
 *	[ envp NULL ]
 *	[ apple NULL ]
 *
 * MILESTONE M2: the real link.  _dyld_start reads the main header off the
 * stack; dyld_main parses its load commands, reads the dependency path out of
 * its LC_LOAD_DYLIB, asks the kernel to map that dylib (the map_image
 * backchannel, our stand-in for open()+mmap()), parses the dylib's export
 * trie, then walks the main image's LC_DYLD_CHAINED_FIXUPS chain -- applying
 * rebases (add slide) and binds (resolve the import against the dylib's trie,
 * patch the GOT slot).  Finally it jumps to the main image's LC_MAIN entry, so
 * the program runs its own code through our linker against our libSystem.
 *
 * Constraint that keeps this self-hosting: dyld must carry NO bound or rebased
 * pointers of its own (nobody runs ITS fixup chain).  So: no global pointer
 * tables, no `static const char *x = "..."` -- only code, immediates, and char
 * arrays / string literals referenced RIP-relative.  Everything below obeys
 * that.  (Even a stray rebase would be harmless at slide 0, but a bind would
 * be fatal, and dyld imports nothing.)
 *
 * Compiled -fno-builtin so the compiler cannot lower a body into a libc call.
 */

/*
 * Fixed-width types straight from the compiler builtins -- no <stdint.h>, so
 * there is no dependency on an SDK or a hosted include path.
 */
typedef __UINT8_TYPE__		uint8_t;
typedef __UINT16_TYPE__		uint16_t;
typedef __UINT32_TYPE__		uint32_t;
typedef __UINT64_TYPE__		uint64_t;
typedef __INT32_TYPE__		int32_t;
typedef __UINTPTR_TYPE__	uintptr_t;

/* ---- Mach-O on-disk structures (subset we parse) ------------------------ */

#define	MACHO_MAGIC_64		0xFEEDFACFu

#define	LC_REQ_DYLD		0x80000000u
#define	LC_SEGMENT_64		0x19
#define	LC_LOAD_DYLIB		0xC
#define	LC_MAIN			(0x28u | LC_REQ_DYLD)
#define	LC_DYLD_CHAINED_FIXUPS	(0x34u | LC_REQ_DYLD)
#define	LC_DYLD_EXPORTS_TRIE	(0x33u | LC_REQ_DYLD)

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
 * The two 64-bit chained-pointer formats we bind.  DYLD_CHAINED_PTR_64 stores
 * an unslid vmaddr in a rebase (our own ld64.lld binaries); _OFFSET stores an
 * offset from the image's mach_header (what Apple's linker emits for x86-64
 * binaries -- e.g. figlet).  Bind records are bit-identical across the two; the
 * rebase target interpretation is the only thing that differs (see apply_fixups).
 */
#define	DYLD_CHAINED_PTR_64		2
#define	DYLD_CHAINED_PTR_64_OFFSET	6
#define	DYLD_CHAINED_PTR_START_NONE	0xFFFF

/* ---- syscalls ----------------------------------------------------------- */

/*
 * One class-encoded Darwin syscall.  The class is the high byte of `nr`
 * (0x2000000 = BSD/Unix).  Darwin x86-64 passes args in rdi, rsi, rdx (this
 * 3-arg helper); `syscall` clobbers rcx/r11.
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
 * map_image (style9-private class 0x2A): ask the kernel to map the dylib named
 * by `path` into this task; returns the base it was mapped at, or 0 on
 * failure.  The kernel signals failure with the BSD carry convention, so this
 * captures the carry flag right after `syscall` (exactly as libSystem reads a
 * BSD error) and folds it into a 0 return.
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

/* ---- parsed image ------------------------------------------------------- */

#define	IMAGE_MAX_SEGS	8

struct image {
	uint64_t	mh;		/* runtime mach_header address       */
	uint64_t	slide;		/* mh - preferred __TEXT vmaddr      */
	uint64_t	text_vmaddr;	/* preferred vmaddr of fileoff-0 seg */
	uint64_t	entryoff;	/* LC_MAIN                           */
	const uint8_t	*fixups;	/* runtime ptr to chained fixups blob */
	uint64_t	 fixups_size;
	const uint8_t	*trie;		/* runtime ptr to export trie blob   */
	uint64_t	 trie_size;
	const char	*dylib_path;	/* first LC_LOAD_DYLIB name (runtime) */
	int		 nsegs;
	int		 have_text;
	struct {
		uint64_t	vmaddr;
		uint64_t	fileoff;
		uint64_t	filesize;
	} segs[IMAGE_MAX_SEGS];
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
	im->dylib_path = 0;
	im->nsegs = 0;
	im->have_text = 0;
	cf_dataoff = cf_datasize = 0;
	tr_dataoff = tr_datasize = 0;

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
		case LC_LOAD_DYLIB: {
			const struct dylib_command	*dl;

			dl = (const struct dylib_command *)lc;
			if (im->dylib_path == 0)
				im->dylib_path =
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
}

/* ---- export trie -------------------------------------------------------- */

/*
 * Resolve `sym` in the export trie at [trie, end).  Returns the export's
 * address (an offset from the image's base), or 0 if absent.  Walks edges
 * that prefix the remaining symbol until the symbol is fully consumed at a
 * terminal node, then reads its (flags, address) ULEB pair -- assuming a
 * regular export, which is all our flat libSystem produces.
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

/* ---- chained fixups ----------------------------------------------------- */

/*
 * Apply `im`'s LC_DYLD_CHAINED_FIXUPS chain.  Rebases get `im`'s slide added;
 * binds resolve the imported symbol against `lib`'s export trie and patch the
 * slot.  M2 has a single dependency, so every bind's lib_ordinal is libSystem
 * and `lib` is it; a Tier-1 graph would index an array of libs by ordinal.
 */
static void
apply_fixups(const struct image *im, const struct image *lib)
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
					uint64_t	 ordinal;
					uint64_t	 addend;
					uint64_t	 noff;
					uint64_t	 sym;
					uint32_t	 imp;

					ordinal = val & 0xFFFFFF;
					addend  = (val >> 24) & 0xFF;
					imp = rd32(imports + ordinal * 4);
					noff = (imp >> 9) & 0x7FFFFF;
					name = symbols + noff;
					sym = trie_lookup(lib->trie,
					    lib->trie + lib->trie_size, name);
					if (sym == 0) {
						d_puts("dyld: unresolved ");
						d_puts(name);
						d_puts("\n");
						*slot = 0;
					} else {
						*slot = lib->mh + sym + addend;
					}
				} else {			/* rebase */
					uint64_t	high8;
					uint64_t	target;
					uint64_t	value;

					target = val & 0xFFFFFFFFFULL;
					high8  = (val >> 36) & 0xFF;
					/*
					 * PTR_64 holds an unslid vmaddr (add the
					 * slide); PTR_64_OFFSET holds an offset
					 * from this image's mach_header (add the
					 * runtime base).  Both fold high8 into the
					 * pointer's top byte.
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

/* ---- entry -------------------------------------------------------------- */

/*
 * dyld_main: the C body of the linker.  `sp` is the raw handoff stack pointer
 * captured by _dyld_start, pointing at [main_mh][argc][argv...].
 */
void
dyld_main(uint64_t *sp)
{
	struct image			 main_im;
	struct image			 lib_im;
	const struct mach_header_64	*mh;
	char				**argv;
	char				**envp;
	char				**apple;
	char				**w;
	uint64_t			 main_mh;
	uint64_t			 libbase;
	uint64_t			 entry;
	int				 argc;
	int				 rc;

	main_mh = sp[0];
	argc = (int)sp[1];
	argv = (char **)&sp[2];
	envp = argv + argc + 1;
	w = envp;
	while (*w != 0)
		w++;
	apple = w + 1;

	d_puts("dyld: M2 link, main_mh=");
	d_puthex(main_mh);

	mh = (const struct mach_header_64 *)(uintptr_t)main_mh;
	if (mh->magic != MACHO_MAGIC_64) {
		d_puts("dyld: bad main magic\n");
		dsys(0x2000001, 71, 0, 0);
	}

	parse_image(main_mh, &main_im);
	if (main_im.dylib_path == 0) {
		d_puts("dyld: main names no LC_LOAD_DYLIB\n");
		dsys(0x2000001, 72, 0, 0);
	}
	d_puts("dyld: dependency ");
	d_puts(main_im.dylib_path);
	d_puts("\n");

	libbase = map_image(main_im.dylib_path);
	if (libbase == 0) {
		d_puts("dyld: map_image failed\n");
		dsys(0x2000001, 73, 0, 0);
	}
	d_puts("dyld: libSystem mapped @ ");
	d_puthex(libbase);

	parse_image(libbase, &lib_im);

	/*
	 * libSystem first (its own rebases; it has no binds), then the main
	 * image bound against libSystem's exports.  Mapping at the preferred
	 * base would make libSystem's rebases no-ops, but applying them keeps
	 * the path correct for any future relocated dependency.
	 */
	apply_fixups(&lib_im, &lib_im);
	apply_fixups(&main_im, &lib_im);

	entry = main_mh + main_im.entryoff;
	d_puts("dyld: enter main @ ");
	d_puthex(entry);

	rc = ((int (*)(int, char **, char **, char **))(uintptr_t)entry)(
	    argc, argv, envp, apple);

	dsys(0x2000001, rc, 0, 0);	/* exit(rc) if main ever returns */
	for (;;)
		;
}

/*
 * _dyld_start: the raw entry the kernel jumps to.  %rsp is the handoff stack;
 * capture it as the sole argument, 16-align for the SysV call, and hand off to
 * dyld_main, which does not return.
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
