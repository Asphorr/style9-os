/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * libSystem.B.dylib -- a clean-room, minimal libSystem for the S4 Darwin
 * binary-compatibility rung.  This is NOT Apple's libSystem: it exports the
 * same symbol NAMES an Apple-ABI binary imports (an interface, not
 * copyrightable code) and implements each on top of style9's class-encoded
 * Darwin syscalls (kern/darwin.c).  Built by the real Darwin toolchain
 * (clang -target x86_64-apple-macos + ld64.lld -dylib) and bound at runtime
 * by our own dyld (user/dyld.c) -- no Apple bits anywhere in the chain.
 *
 * The export set was grown to satisfy a real Apple CLI binary (figlet, a
 * Homebrew x86-64 macOS bottle): the 43 symbols it imports from
 * /usr/lib/libSystem.B.dylib -- stdio, malloc, string/mem, ctype/locale,
 * getopt, the stack protector, and one stat variant.  Everything that needs a
 * filesystem (fopen, stat) fails cleanly (NULL / -1) since style9 exposes no
 * VFS to ring 3 yet; everything else is a complete, self-contained
 * implementation over write(2)/exit(2).
 *
 * Compiled -fno-builtin so the compiler cannot lower a body into a libc call
 * (memcpy/memset) the no-libc link could not resolve, and cannot turn our own
 * memcpy/memset into self-recursion.
 */

#pragma clang diagnostic ignored "-Wmissing-field-initializers"

typedef __UINT8_TYPE__		uint8_t;
typedef __UINT16_TYPE__		uint16_t;
typedef __UINT32_TYPE__		uint32_t;
typedef __UINT64_TYPE__		uint64_t;
typedef __INT32_TYPE__		int32_t;
typedef __INT64_TYPE__		int64_t;
typedef __SIZE_TYPE__		size_t;
typedef __PTRDIFF_TYPE__	ssize_t;
typedef __WCHAR_TYPE__		wchar_t;

#define	NULL	((void *)0)
#define	EOF	(-1)

/* ---- raw Darwin syscalls ------------------------------------------------ */

/*
 * One class-encoded Darwin syscall.  The class is the high byte of `nr`
 * (0x2000000 = BSD/Unix).  This 3-arg helper places args in rdi/rsi/rdx; the
 * full Darwin x86-64 convention continues r10/r8/r9 (NOT rcx, which `syscall`
 * clobbers).  The kernel's result, with Apple's carry-flag error convention,
 * comes back in rax.
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

long
write(int fd, const void *buf, unsigned long n)
{
	return (dsys(0x2000004, fd, (long)buf, (long)n));	/* BSD write */
}

/*
 * atexit handlers, run by exit(3) in LIFO order.  guname registers gnulib's
 * close_stdout here to flush stdout at teardown; our stdio writes are already
 * unbuffered, so it is belt-and-braces, but running the handlers keeps exit(3)
 * faithful.  _exit(2) (below) deliberately skips them.  Programs that register
 * none (figlet, tree -- neither imports atexit) see identical behaviour, so
 * this is a no-op for them.
 */
#define	ATEXIT_MAX	32
static void	(*atexit_fns[ATEXIT_MAX])(void);
static int	atexit_n;

/*
 * errno's storage.  Apple's <errno.h> defines errno as (*__error()), so a
 * binary reads it through the accessor below rather than as a symbol; one
 * shared cell is enough for a system with one thread per process.  It lives
 * this far up the file because the calls that SET it -- mmap, fstatat --
 * are scattered through it, and a variable used in six places should be
 * declared once at the top rather than forward-declared into the middle.
 */
static int	g_errno;

void
exit(int code)
{
	int	i;

	for (i = atexit_n - 1; i >= 0; i--)
		atexit_fns[i]();
	dsys(0x2000001, code, 0, 0);				/* BSD exit  */
	for (;;)
		;
}

int
getpid(void)
{
	return ((int)dsys(0x2000014, 0, 0, 0));			/* BSD getpid */
}

/*
 * A BSD-class syscall that honours Apple's carry-flag error convention: on
 * error the kernel sets CF and returns a positive errno, otherwise CF is clear
 * and %rax is the result.  Capture CF right after `syscall` (exactly as
 * libSystem reads a BSD error) and fold a failure into the -1 the fd routines
 * below expect.  write/exit/getpid above ignore errors, so they use the
 * lighter dsys; the file calls need the error bit.
 *
 * The errno store is not decoration.  This used to throw the code away and
 * return only -1, which is enough for a caller that wants to know THAT
 * something failed and useless to one that must know WHAT: a read interrupted
 * by a signal and a read on a closed descriptor were the same answer, so
 * nothing could retry the first and give up on the second.  It surfaced the
 * day a test asserted EINTR and got 0.
 */
static long
bsd_call(long nr, long a, long b, long c)
{
	long		ret;
	unsigned char	cf;

	__asm__ __volatile__(
	    "syscall\n\t"
	    "setc %1\n"
	    : "=a"(ret), "=r"(cf)
	    : "a"(nr), "D"(a), "S"(b), "d"(c)
	    : "rcx", "r11", "memory");
	if (cf) {
		g_errno = (int)ret;
		return (-1);
	}
	return (ret);
}

static int
s_open(const char *path, int flags)
{
	return ((int)bsd_call(0x2000005, (long)path, flags, 0));   /* BSD open */
}

static long
s_read(int fd, void *buf, unsigned long n)
{
	return (bsd_call(0x2000003, fd, (long)buf, (long)n));	  /* BSD read */
}

static int
s_close(int fd)
{
	return ((int)bsd_call(0x2000006, fd, 0, 0));		  /* BSD close */
}

static long
s_lseek(int fd, long off, int whence)
{
	return (bsd_call(0x20000C7, fd, off, whence));		  /* BSD lseek */
}

/* ---- string / memory ---------------------------------------------------- */

size_t
strlen(const char *s)
{
	const char	*p;

	p = s;
	while (*p != '\0')
		p++;
	return ((size_t)(p - s));
}

char *
strcpy(char *dst, const char *src)
{
	char	*d;

	d = dst;
	while ((*d++ = *src++) != '\0')
		;
	return (dst);
}

char *
strcat(char *dst, const char *src)
{
	char	*d;

	d = dst;
	while (*d != '\0')
		d++;
	while ((*d++ = *src++) != '\0')
		;
	return (dst);
}

char *
strchr(const char *s, int c)
{

	for (;; s++) {
		if (*s == (char)c)
			return ((char *)(unsigned long)s);
		if (*s == '\0')
			return (NULL);
	}
}

char *
strrchr(const char *s, int c)
{
	const char	*last;

	last = NULL;
	for (;; s++) {
		if (*s == (char)c)
			last = s;
		if (*s == '\0')
			return ((char *)(unsigned long)last);
	}
}

void *
memchr(const void *s, int c, size_t n)
{
	const unsigned char	*p;
	size_t			 i;

	p = (const unsigned char *)s;
	for (i = 0; i < n; i++) {
		if (p[i] == (unsigned char)c)
			return ((void *)(unsigned long)(p + i));
	}
	return (NULL);
}

void *
memcpy(void *dst, const void *src, size_t n)
{
	unsigned char		*d;
	const unsigned char	*s;
	size_t			 i;

	d = (unsigned char *)dst;
	s = (const unsigned char *)src;
	for (i = 0; i < n; i++)
		d[i] = s[i];
	return (dst);
}

void *
memset(void *b, int c, size_t n)
{
	unsigned char	*p;
	size_t		 i;

	p = (unsigned char *)b;
	for (i = 0; i < n; i++)
		p[i] = (unsigned char)c;
	return (b);
}

/* Apple spelling of bzero(); the import is ___bzero. */
void
__bzero(void *b, size_t n)
{

	(void)memset(b, 0, n);
}

/*
 * memset_pattern16: fill [b, b+len) with the 16-byte pattern at `pat`,
 * repeating it and truncating the final partial copy.  An Apple libc
 * extension figlet pulls in via its optimised string paths.
 */
void
memset_pattern16(void *b, const void *pat, size_t len)
{
	unsigned char		*d;
	const unsigned char	*p;
	size_t			 i;

	d = (unsigned char *)b;
	p = (const unsigned char *)pat;
	for (i = 0; i < len; i++)
		d[i] = p[i & 15];
}

/* ---- malloc: a bump allocator over mmap'd chunks ------------------------ */

/*
 * The heap is asked for, not declared.
 *
 * This used to be a 4 MiB array in BSS, because ring 3 had no way to ask the
 * kernel for memory.  That array was not free: the Mach-O loader allocates and
 * zeroes every page of a segment's bss tail when the image loads, so every
 * program here paid 4 MiB of real frames up front and a fork copied all of
 * them, whether it ever called malloc or not.
 *
 * Now malloc requests chunks with mmap(MAP_ANON) and bumps a cursor through
 * them.  The kernel hands back only the promise; each page becomes a frame
 * when it is first touched, so a program that allocates a kilobyte holds a
 * page, and one that allocates nothing holds nothing.
 *
 * Everything else is as it was.  Each block carries a 16-byte header holding
 * its usable size, so realloc() can copy the old contents forward; free() is
 * a no-op, since the programs hosted here allocate near-monotonically and
 * never depend on reclamation.  Chunks are page-aligned and every size is
 * rounded to 16, so returned pointers meet the alignment the SSE string paths
 * assume.  A request that does not fit the current chunk abandons its tail and
 * maps a new one -- the same bargain free() already makes, bounded by the
 * chunk size rather than by the whole heap.
 */
#define	CHUNK_BYTES	(256u * 1024u)

/* <sys/mman.h>, which this file is the stand-in for. */
#define	PROT_NONE	0x00
#define	PROT_READ	0x01
#define	PROT_WRITE	0x02
#define	PROT_EXEC	0x04
#define	MAP_SHARED	0x0001
#define	MAP_PRIVATE	0x0002
#define	MAP_FIXED	0x0010
#define	MAP_ANON	0x1000
#define	MAP_FAILED	((void *)-1)

/* Defined further down, next to errno; malloc is the first thing to need it. */
void	*mmap(void *addr, size_t len, int prot, int flags, int fd, long off);
int	 munmap(void *addr, size_t len);

static unsigned char	*heap_base;	/* current chunk, NULL before the first */
static size_t		 heap_size;
static size_t		 heap_off;	/* bump cursor within it               */

void *
malloc(size_t n)
{
	unsigned char	*chunk;
	size_t		 need;
	size_t		 want;
	size_t		 off;

	n = (n + 15u) & ~(size_t)15u;		/* 16-byte alignment */
	if (n == 0)
		n = 16;
	need = n + 16u;				/* + a 16-byte size header */
	if (need < n)
		return (NULL);

	if (heap_base == NULL || need > heap_size ||
	    heap_off > heap_size - need) {
		want = (need > CHUNK_BYTES) ?
		    ((need + 0xFFFu) & ~(size_t)0xFFFu) : CHUNK_BYTES;
		chunk = mmap(NULL, want, PROT_READ | PROT_WRITE,
		    MAP_ANON | MAP_PRIVATE, -1, 0);
		if (chunk == MAP_FAILED)
			return (NULL);
		heap_base = chunk;
		heap_size = want;
		heap_off  = 0;
	}
	off = heap_off;
	heap_off += need;
	*(size_t *)(void *)&heap_base[off] = n;	/* usable size, for realloc */
	return (&heap_base[off + 16]);
}

void
free(void *p)
{

	(void)p;
}

void *
realloc(void *p, size_t n)
{
	size_t	 old;
	void	*q;

	if (p == NULL)
		return (malloc(n));
	old = *(size_t *)(void *)((unsigned char *)p - 16);
	q = malloc(n);
	if (q == NULL)
		return (NULL);
	(void)memcpy(q, p, old < n ? old : n);
	return (q);			/* arena free is a no-op: old block leaks */
}

/*
 * The page size ring 3 sees.  A constant rather than a syscall: the kernel's
 * host port reports the same 4096 (mach/host.c), and a program asking this
 * wants a number to size a buffer with, not a fact about the machine.
 */
int
getpagesize(void)
{

	return (4096);
}

/*
 * posix_memalign: over-allocate and round up.  The subtlety is that realloc()
 * above reads a block's usable size from the 16 bytes IN FRONT of the pointer
 * it was given, so the header has to be re-stamped in front of the ALIGNED
 * pointer, not left in front of the raw one -- otherwise a realloc of an
 * aligned block would copy whatever happened to sit there.  Since a chunk is
 * page-aligned and every size rounds to 16, any gap it opens is a multiple of
 * 16, so the restamped header never reaches back into another block.
 */
int
posix_memalign(void **out, size_t align, size_t size)
{
	unsigned char	*raw;
	size_t		 addr;

	/* Apple's contract: a power of two, and at least sizeof(void *). */
	if (align < sizeof(void *) || (align & (align - 1)) != 0)
		return (22);				/* EINVAL */
	if (size == 0) {
		*out = NULL;
		return (0);
	}
	if (size > (size_t)-1 - align)
		return (12);				/* ENOMEM */
	raw = malloc(size + align);
	if (raw == NULL)
		return (12);				/* ENOMEM */
	addr = ((size_t)raw + align - 1u) & ~(align - 1u);
	*(size_t *)(void *)(addr - 16) = size;
	*out = (void *)addr;
	return (0);
}

/* ---- stdlib scraps ------------------------------------------------------ */

int
atoi(const char *s)
{
	int	neg;
	int	v;

	while (*s == ' ' || (*s >= '\t' && *s <= '\r'))
		s++;
	neg = 0;
	if (*s == '+' || *s == '-')
		neg = (*s++ == '-');
	v = 0;
	while (*s >= '0' && *s <= '9')
		v = v * 10 + (*s++ - '0');
	return (neg ? -v : v);
}

/*
 * getenv scans the exported environ (defined with _NSGetEnviron below) --
 * a forward declaration here since the storage lives with its accessor.
 */
extern char	**environ;

char *
getenv(const char *name)
{
	size_t	j;
	size_t	i;

	if (name == NULL || environ == NULL)
		return (NULL);
	for (i = 0; environ[i] != NULL; i++) {
		for (j = 0; name[j] != '\0' &&
		    environ[i][j] == name[j]; j++)
			;
		if (name[j] == '\0' && environ[i][j] == '=')
			return (&environ[i][j + 1]);
	}
	return (NULL);
}

/* ---- ctype / locale ----------------------------------------------------- */

/*
 * Apple's <ctype.h> inlines isspace()/isdigit()/... to read
 * _DefaultRuneLocale.__runetype[c] for ASCII (c < 128) and to call __maskrune()
 * otherwise; toupper()/tolower() call __toupper()/__tolower().  We must export
 * _DefaultRuneLocale with __runetype at the SAME offset Apple's header placed it
 * (60: magic[8]+encoding[32]+2 ptrs+invalid_rune) and fill it with the C-locale
 * classification, plus implement the three extern helpers.  __runetype is built
 * entirely at compile time (a macro fan-out over 0..255) since our dyld runs no
 * library initialisers.  The _CTYPE_* bit values are Apple's published ABI.
 */
#define	_CT_A	0x00000100u		/* alpha   */
#define	_CT_C	0x00000200u		/* control */
#define	_CT_D	0x00000400u		/* digit   */
#define	_CT_G	0x00000800u		/* graph   */
#define	_CT_L	0x00001000u		/* lower   */
#define	_CT_P	0x00002000u		/* punct   */
#define	_CT_S	0x00004000u		/* space   */
#define	_CT_U	0x00008000u		/* upper   */
#define	_CT_X	0x00010000u		/* xdigit  */
#define	_CT_B	0x00020000u		/* blank   */
#define	_CT_R	0x00040000u		/* print   */

#define	CDIG(c)		((c) >= '0' && (c) <= '9')
#define	CUPP(c)		((c) >= 'A' && (c) <= 'Z')
#define	CLOW(c)		((c) >= 'a' && (c) <= 'z')
#define	CXDG(c)		(CDIG(c) || ((c) >= 'a' && (c) <= 'f') || \
			    ((c) >= 'A' && (c) <= 'F'))
#define	CSPC(c)		((c) == ' ' || ((c) >= '\t' && (c) <= '\r'))
#define	CBLK(c)		((c) == ' ' || (c) == '\t')
#define	CCTL(c)		((c) < 0x20 || (c) == 0x7f)
#define	CALPHA(c)	(CUPP(c) || CLOW(c))
#define	CPUN(c)		((c) >= 0x21 && (c) <= 0x7e && !CALPHA(c) && !CDIG(c))
#define	CPRN(c)		((c) >= 0x20 && (c) <= 0x7e)
#define	CGPH(c)		((c) >= 0x21 && (c) <= 0x7e)

#define	RT(c)	( \
	(CALPHA(c) ? _CT_A : 0u) | (CCTL(c) ? _CT_C : 0u) | \
	(CDIG(c)   ? _CT_D : 0u) | (CGPH(c) ? _CT_G : 0u) | \
	(CLOW(c)   ? _CT_L : 0u) | (CPUN(c) ? _CT_P : 0u) | \
	(CSPC(c)   ? _CT_S : 0u) | (CUPP(c) ? _CT_U : 0u) | \
	(CXDG(c)   ? _CT_X : 0u) | (CBLK(c) ? _CT_B : 0u) | \
	(CPRN(c)   ? _CT_R : 0u))

#define	R4(c)	RT(c), RT((c) + 1), RT((c) + 2), RT((c) + 3)
#define	R16(c)	R4(c), R4((c) + 4), R4((c) + 8), R4((c) + 12)
#define	R64(c)	R16(c), R16((c) + 16), R16((c) + 32), R16((c) + 48)
#define	R256	R64(0), R64(64), R64(128), R64(192)

typedef int	__darwin_rune_t;

typedef struct {
	__darwin_rune_t	 __min;
	__darwin_rune_t	 __max;
	__darwin_rune_t	 __map;
	uint32_t	*__types;
} _RuneEntry;

typedef struct {
	int		 __nranges;
	_RuneEntry	*__ranges;
} _RuneRange;

typedef struct {
	char		__magic[8];
	char		__encoding[32];
	void	       *__sgetrune;
	void	       *__sputrune;
	__darwin_rune_t	__invalid_rune;
	uint32_t	__runetype[256];
	__darwin_rune_t	__maplower[256];
	__darwin_rune_t	__mapupper[256];
	_RuneRange	__runetype_ext;
	_RuneRange	__maplower_ext;
	_RuneRange	__mapupper_ext;
	void	       *__variable;
	int		__variable_len;
} _RuneLocale;

_RuneLocale _DefaultRuneLocale = {
	.__runetype = { R256 },
};

/*
 * __maskrune: the slow path of the ctype macros (c >= 128, or a non-inlined
 * call site).  Return the rune's classification masked by `f`.  Non-ASCII
 * runes carry no class in the C locale, so they mask to zero.  Returns
 * unsigned long so the full %rax is defined regardless of how the caller's
 * header prototyped it.
 */
unsigned long
__maskrune(int c, unsigned long f)
{

	if ((unsigned)c < 128)
		return ((unsigned long)_DefaultRuneLocale.__runetype[c] & f);
	return (0);
}

int
__toupper(int c)
{

	return ((c >= 'a' && c <= 'z') ? c - 32 : c);
}

int
__tolower(int c)
{

	return ((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

/* ---- wide-character scraps (wchar_t == int on this ABI) ----------------- */

size_t
wcslen(const wchar_t *s)
{
	const wchar_t	*p;

	p = s;
	while (*p != 0)
		p++;
	return ((size_t)(p - s));
}

wchar_t *
wcscpy(wchar_t *dst, const wchar_t *src)
{
	wchar_t	*d;

	d = dst;
	while ((*d++ = *src++) != 0)
		;
	return (dst);
}

wchar_t *
wcscat(wchar_t *dst, const wchar_t *src)
{
	wchar_t	*d;

	d = dst;
	while (*d != 0)
		d++;
	while ((*d++ = *src++) != 0)
		;
	return (dst);
}

/* ---- stdio -------------------------------------------------------------- */

/*
 * FILE is NOT entirely opaque to an Apple binary: Apple's <stdio.h> inlines
 * getc() as __sgetc() -- `(--fp->_r < 0 ? __srget(fp) : (int)(*fp->_p++))`
 * -- and feof()/fileno() the same way, so compiled-in macro expansions
 * dereference Apple's struct __sFILE field OFFSETS directly (gfactor's
 * stdin reader does exactly this; it imports __srget, the macro's refill
 * hook).  The head of our FILE therefore mirrors Apple's layout where the
 * macros look: _r at offset 8 is pinned <= 0 so EVERY inlined getc takes
 * the __srget path (which re-pins it) and our unbuffered read logic stays
 * the single point of truth; _w at 12 is pinned 0 (the __sputc inline would
 * call __swbuf, which nothing here imports -- the preflight would catch a
 * binary that does); _flags at 16 carries the real __SEOF bit for the
 * feof() inline; _file at 18 carries the fd for the fileno() inline.  The
 * buffer pointer _p (offset 0) is never dereferenced while _r/_w are kept
 * non-positive, so it doubles as our fd + eof storage.  Everything past
 * Apple's first 20 bytes is ours (the ungetc pushback).
 */
typedef struct __sFILE {
	int	fd;		/* offset 0: Apple's _p, never deref'd  */
	int	eof;		/* offset 4: high half of _p            */
	int	rspace;		/* offset 8: Apple's _r -- keep <= 0    */
	int	wspace;		/* offset 12: Apple's _w -- keep 0      */
	short	aflags;		/* offset 16: Apple's _flags (__SEOF)   */
	short	afile;		/* offset 18: Apple's _file (the fd)    */
	int	unget;		/* one-char ungetc() pushback; EOF == empty */
} FILE;

#define	APPLE_SEOF	0x0020		/* Apple's __SEOF flag bit */

static FILE	__stdin_file  = { 0, 0, 0, 0, 0, 0, EOF };
static FILE	__stdout_file = { 1, 0, 0, 0, 0, 1, EOF };
static FILE	__stderr_file = { 2, 0, 0, 0, 0, 2, EOF };

/* The stdio.h macros stdin/stdout/stderr expand to these exported pointers. */
FILE	*__stdinp  = &__stdin_file;
FILE	*__stdoutp = &__stdout_file;
FILE	*__stderrp = &__stderr_file;

int
putchar(int c)
{
	unsigned char	ch;

	ch = (unsigned char)c;
	write(1, &ch, 1);
	return (c);
}

int
puts(const char *s)
{

	write(1, s, strlen(s));
	write(1, "\n", 1);
	return (0);
}

int
getchar(void)
{
	unsigned char	ch;

	return (s_read(0, &ch, 1) == 1 ? (int)ch : EOF);
}

size_t
fwrite(const void *ptr, size_t size, size_t nmemb, FILE *fp)
{
	size_t	n;

	n = size * nmemb;
	if (fp == NULL || n == 0)
		return (0);
	write(fp->fd, ptr, n);
	return (nmemb);
}

size_t
fread(void *ptr, size_t size, size_t nmemb, FILE *fp)
{
	unsigned char	*p;
	size_t		 got;
	size_t		 want;

	if (fp == NULL || size == 0 || nmemb == 0)
		return (0);
	want = size * nmemb;
	p = (unsigned char *)ptr;
	got = 0;
	while (got < want) {
		long	r;

		r = s_read(fp->fd, p + got, want - got);
		if (r <= 0) {
			if (r == 0) {
				fp->eof = 1;
				fp->aflags |= APPLE_SEOF;
			}
			break;			/* EOF or error: short read */
		}
		got += (size_t)r;
	}
	return (got / size);
}

FILE *
fopen(const char *path, const char *mode)
{
	FILE	*fp;
	int	 fd;

	(void)mode;				/* read-only FS: always O_RDONLY */
	fd = s_open(path, 0);
	if (fd < 0)
		return (NULL);			/* absent / unreadable */
	fp = (FILE *)malloc(sizeof(*fp));
	if (fp == NULL) {
		s_close(fd);
		return (NULL);
	}
	fp->fd     = fd;
	fp->eof    = 0;
	fp->rspace = 0;
	fp->wspace = 0;
	fp->aflags = 0;
	fp->afile  = (short)fd;
	fp->unget  = EOF;
	return (fp);
}

int
fclose(FILE *fp)
{

	if (fp == NULL)
		return (-1);
	if (fp == &__stdin_file || fp == &__stdout_file ||
	    fp == &__stderr_file)
		return (0);			/* never close a std stream */
	s_close(fp->fd);
	free(fp);				/* arena free is a no-op */
	return (0);
}

int
fseek(FILE *fp, long off, int whence)
{

	if (fp == NULL)
		return (-1);
	if (s_lseek(fp->fd, off, whence) < 0)
		return (-1);
	fp->eof = 0;
	fp->aflags &= ~APPLE_SEOF;
	return (0);
}

long
ftell(FILE *fp)
{

	if (fp == NULL)
		return (-1);
	return (s_lseek(fp->fd, 0, 1));		/* SEEK_CUR */
}

FILE *
tmpfile(void)
{

	return (NULL);				/* no writable FS yet */
}

/* ---- formatted output (printf / fprintf / sprintf family) --------------- */

/*
 * A tiny emitter that targets EITHER an fd (printf/fprintf: buffered, flushed
 * when full, so long output costs few write(2)s) OR a caller buffer
 * (sprintf/snprintf: bounded, NUL-terminated).  `total` counts every char the
 * format produced -- the value the snprintf family returns -- regardless of
 * truncation; `n` is bytes pending in buf (fd) or already stored in dst.
 */
struct ob {
	int		fd;	/* >= 0: flush to fd; < 0: write into dst */
	char	       *dst;	/* caller buffer (fd < 0)                 */
	size_t		cap;	/* size of dst incl. NUL (fd < 0)         */
	size_t		total;	/* chars the format produced              */
	unsigned	n;
	char		buf[256];
};

static void
ob_flush(struct ob *o)
{

	if (o->fd >= 0 && o->n != 0) {
		write(o->fd, o->buf, o->n);
		o->n = 0;
	}
}

static void
ob_putc(struct ob *o, char c)
{

	o->total++;
	if (o->fd >= 0) {
		if (o->n == sizeof(o->buf))
			ob_flush(o);
		o->buf[o->n++] = c;
	} else if (o->dst != NULL && (size_t)o->n + 1 < o->cap) {
		o->dst[o->n++] = c;
	}
}

static void
ob_pad(struct ob *o, char c, int n)
{

	while (n-- > 0)
		ob_putc(o, c);
}

/* Emit an unsigned value in `base`; returns the digit count written. */
static int
ob_putu(struct ob *o, uint64_t v, unsigned base, int upper)
{
	const char	*digs;
	char		 tmp[24];
	int		 i;

	digs = upper ? "0123456789ABCDEF" : "0123456789abcdef";
	i = 0;
	do {
		tmp[i++] = digs[v % base];
		v /= base;
	} while (v != 0);
	while (--i >= 0)
		ob_putc(o, tmp[i]);
	return (0);
}

static int
ob_ndigits(uint64_t v, unsigned base)
{
	int	n;

	n = 1;
	while (v >= base) {
		v /= base;
		n++;
	}
	return (n);
}

static int
vfmt(int fd, char *dst, size_t cap, const char *fmt, __builtin_va_list ap)
{
	struct ob	o;
	const char	*p;

	o.fd = fd;
	o.dst = dst;
	o.cap = cap;
	o.total = 0;
	o.n = 0;
	for (p = fmt; *p != '\0'; p++) {
		int		left, lng, prec, wdig, width, zero;
		const char	*s;

		if (*p != '%') {
			ob_putc(&o, *p);
			continue;
		}
		p++;
		left = zero = 0;
		width = -1;
		prec = -1;
		lng = 0;
		/* flags */
		for (; ; p++) {
			if (*p == '-')
				left = 1;
			else if (*p == '0')
				zero = 1;
			else if (*p == '+' || *p == ' ' || *p == '#')
				;		/* accepted, not rendered */
			else
				break;
		}
		/* width (-1 = unspecified) */
		if (*p == '*') {
			width = __builtin_va_arg(ap, int);
			p++;
		} else {
			wdig = 0;
			for (width = 0; *p >= '0' && *p <= '9'; p++) {
				width = width * 10 + (*p - '0');
				wdig = 1;
			}
			if (!wdig)
				width = -1;
		}
		/* precision */
		if (*p == '.') {
			p++;
			if (*p == '*') {
				prec = __builtin_va_arg(ap, int);
				p++;
			} else {
				for (prec = 0; *p >= '0' && *p <= '9'; p++)
					prec = prec * 10 + (*p - '0');
			}
		}
		/* length modifiers */
		for (; *p == 'l' || *p == 'h' || *p == 'z' || *p == 'j' ||
		    *p == 't'; p++) {
			if (*p == 'l' || *p == 'z' || *p == 'j' || *p == 't')
				lng = 1;
		}

		switch (*p) {
		case 's': {
			int	len, pad, i;

			s = __builtin_va_arg(ap, const char *);
			if (s == NULL)
				s = "(null)";
			len = 0;
			while (s[len] != '\0' && (prec < 0 || len < prec))
				len++;
			pad = (width > len) ? width - len : 0;
			if (!left)
				ob_pad(&o, ' ', pad);
			for (i = 0; i < len; i++)
				ob_putc(&o, s[i]);
			if (left)
				ob_pad(&o, ' ', pad);
			break;
		}
		case 'c':
			ob_putc(&o, (char)__builtin_va_arg(ap, int));
			break;
		case 'd':
		case 'i': {
			int64_t	v;
			uint64_t mag;
			int	 nd, pad, neg;

			v = lng ? __builtin_va_arg(ap, int64_t) :
			    (int64_t)__builtin_va_arg(ap, int32_t);
			neg = (v < 0);
			mag = neg ? (uint64_t)(-v) : (uint64_t)v;
			nd = ob_ndigits(mag, 10) + (neg ? 1 : 0);
			pad = (width > nd) ? width - nd : 0;
			if (!left && !zero)
				ob_pad(&o, ' ', pad);
			if (neg)
				ob_putc(&o, '-');
			if (!left && zero)
				ob_pad(&o, '0', pad);
			ob_putu(&o, mag, 10, 0);
			if (left)
				ob_pad(&o, ' ', pad);
			break;
		}
		case 'u':
		case 'x':
		case 'X':
		case 'o': {
			uint64_t v;
			unsigned base;
			int	 nd, pad, up;

			v = lng ? __builtin_va_arg(ap, uint64_t) :
			    (uint64_t)__builtin_va_arg(ap, uint32_t);
			base = (*p == 'x' || *p == 'X') ? 16 :
			    (*p == 'o') ? 8 : 10;
			up = (*p == 'X');
			nd = ob_ndigits(v, base);
			pad = (width > nd) ? width - nd : 0;
			if (!left)
				ob_pad(&o, zero ? '0' : ' ', pad);
			ob_putu(&o, v, base, up);
			if (left)
				ob_pad(&o, ' ', pad);
			break;
		}
		case 'p':
			ob_putc(&o, '0');
			ob_putc(&o, 'x');
			ob_putu(&o, (uint64_t)(unsigned long)
			    __builtin_va_arg(ap, void *), 16, 0);
			break;
		case '%':
			ob_putc(&o, '%');
			break;
		case '\0':
			p--;			/* trailing %: stop cleanly */
			break;
		default:
			ob_putc(&o, '%');
			ob_putc(&o, *p);
			break;
		}
	}
	if (o.fd >= 0)
		ob_flush(&o);
	else if (o.dst != NULL && o.cap > 0)
		o.dst[o.n] = '\0';
	return ((int)o.total);
}

int
printf(const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	__builtin_va_start(ap, fmt);
	r = vfmt(1, NULL, 0, fmt, ap);
	__builtin_va_end(ap);
	return (r);
}

int
fprintf(FILE *fp, const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	__builtin_va_start(ap, fmt);
	r = vfmt(fp != NULL ? fp->fd : 2, NULL, 0, fmt, ap);
	__builtin_va_end(ap);
	return (r);
}

/*
 * The sprintf family routes through the same formatter with an fd of -1, so
 * the output lands in the caller's buffer (NUL-terminated, capped to `cap`).
 * vsnprintf is the common core; __sprintf_chk is clang's _FORTIFY_SOURCE
 * sprintf, whose object-size arg we treat as a snprintf cap so it cannot
 * overrun.  All return the length the format produced, snprintf-style.
 */
int
vsnprintf(char *dst, size_t cap, const char *fmt, __builtin_va_list ap)
{

	return (vfmt(-1, dst, cap, fmt, ap));
}

int
snprintf(char *dst, size_t cap, const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	__builtin_va_start(ap, fmt);
	r = vfmt(-1, dst, cap, fmt, ap);
	__builtin_va_end(ap);
	return (r);
}

int
sprintf(char *dst, const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	__builtin_va_start(ap, fmt);
	r = vfmt(-1, dst, (size_t)-1, fmt, ap);
	__builtin_va_end(ap);
	return (r);
}

int
__sprintf_chk(char *dst, int flag, size_t slen, const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	(void)flag;
	__builtin_va_start(ap, fmt);
	r = vfmt(-1, dst, slen, fmt, ap);
	__builtin_va_end(ap);
	return (r);
}

/* ---- minimal sscanf ----------------------------------------------------- */

/*
 * A small sscanf: enough to parse a figlet font header (a run of decimal
 * integers) plus %c/%s/%x.  Honours leading whitespace skipping, `*`
 * suppression, and an optional field width.  Returns the assigned-field count.
 */
static int
isspc(int c)
{

	return (c == ' ' || (c >= '\t' && c <= '\r'));
}

int
sscanf(const char *str, const char *fmt, ...)
{
	__builtin_va_list	ap;
	const char		*s;
	const char		*f;
	int			 assigned;

	__builtin_va_start(ap, fmt);
	s = str;
	assigned = 0;
	for (f = fmt; *f != '\0'; f++) {
		int	suppress, width;

		if (isspc(*f)) {
			while (isspc(*s))
				s++;
			continue;
		}
		if (*f != '%') {
			if (*s != *f) {
				break;
			}
			s++;
			continue;
		}
		f++;
		suppress = 0;
		if (*f == '*') {
			suppress = 1;
			f++;
		}
		for (width = 0; *f >= '0' && *f <= '9'; f++)
			width = width * 10 + (*f - '0');
		if (width == 0)
			width = 1 << 30;
		while (*f == 'l' || *f == 'h')
			f++;

		if (*f == 'c') {
			if (*s == '\0')
				break;
			if (!suppress)
				*__builtin_va_arg(ap, char *) = *s;
			s++;
			if (!suppress)
				assigned++;
			continue;
		}
		while (isspc(*s))
			s++;
		if (*s == '\0')
			break;

		if (*f == 'd' || *f == 'i' || *f == 'u' || *f == 'x') {
			long	v;
			int	neg, base, any, w;

			base = (*f == 'x') ? 16 : 10;
			neg = 0;
			w = width;
			if ((*s == '+' || *s == '-') && w > 0) {
				neg = (*s == '-');
				s++;
				w--;
			}
			v = 0;
			any = 0;
			for (; w > 0 && *s != '\0'; w--, s++) {
				int	d;

				if (*s >= '0' && *s <= '9')
					d = *s - '0';
				else if (base == 16 && *s >= 'a' && *s <= 'f')
					d = *s - 'a' + 10;
				else if (base == 16 && *s >= 'A' && *s <= 'F')
					d = *s - 'A' + 10;
				else
					break;
				v = v * base + d;
				any = 1;
			}
			if (!any)
				break;
			if (neg)
				v = -v;
			if (!suppress) {
				*__builtin_va_arg(ap, int *) = (int)v;
				assigned++;
			}
		} else if (*f == 's') {
			char	*out;
			int	 w;

			out = suppress ? NULL : __builtin_va_arg(ap, char *);
			w = width;
			while (w-- > 0 && *s != '\0' && !isspc(*s)) {
				if (out != NULL)
					*out++ = *s;
				s++;
			}
			if (out != NULL) {
				*out = '\0';
				assigned++;
			}
		} else {
			break;			/* unsupported conversion */
		}
	}
	__builtin_va_end(ap);
	return (assigned);
}

/* ---- getopt ------------------------------------------------------------- */

/*
 * The classic BSD getopt.  optind/optarg/opterr/optopt are exported globals --
 * figlet binds to optind/optarg and reads them between calls, while getopt (in
 * this library) writes the same storage.  `g_place` is our private scan cursor.
 * optreset is the BSD re-scan protocol: a shell's getopts builtin sets it (with
 * optind) to parse a fresh vector, and getopt clears it after dropping the
 * stale cursor.
 */
int	 opterr = 1;
int	 optind = 1;
int	 optopt = 0;
int	 optreset = 0;
char	*optarg = NULL;

static const char	*g_place = "";

int
getopt(int argc, char *const argv[], const char *optstring)
{
	const char	*oli;
	int		 c;

	if (optreset) {
		optreset = 0;
		g_place = "";
	}
	if (*g_place == '\0') {
		if (optind >= argc || argv[optind][0] != '-' ||
		    argv[optind][1] == '\0')
			return (-1);
		if (argv[optind][1] == '-' && argv[optind][2] == '\0') {
			optind++;
			return (-1);		/* "--" terminates options */
		}
		g_place = &argv[optind][1];
	}

	c = (unsigned char)*g_place++;
	oli = strchr(optstring, c);
	if (c == ':' || oli == NULL) {
		if (*g_place == '\0')
			optind++;
		if (opterr && *optstring != ':')
			fprintf(__stderrp, "%s: illegal option -- %c\n",
			    argv[0], c);
		optopt = c;
		return ('?');
	}

	if (oli[1] != ':') {			/* no argument */
		optarg = NULL;
		if (*g_place == '\0')
			optind++;
	} else {				/* option wants an argument */
		if (*g_place != '\0') {
			optarg = (char *)(unsigned long)g_place;
		} else if (++optind < argc) {
			optarg = argv[optind];
		} else {			/* missing argument */
			g_place = "";
			optopt = c;
			if (opterr && *optstring != ':')
				fprintf(__stderrp,
				    "%s: option requires an argument -- %c\n",
				    argv[0], c);
			return (*optstring == ':' ? ':' : '?');
		}
		g_place = "";
		optind++;
	}
	return (c);
}

/* ---- filesystem metadata: stat / readdir over the private backchannel --- */

/*
 * There used to be a path_abs() here, rewriting "." and "./x" into absolute
 * paths before the syscall, and its own comment admitted why: the kernel had
 * no working directory to consult, so "this is the only place that knows the
 * cwd is a fiction."  It is gone because the fiction is.  The kernel resolves
 * relative paths against the calling task's real working directory now, which
 * is the only place that CAN resolve them correctly -- "../x" needs to know
 * where you actually are, and this side never did.
 */

/*
 * The kernel reports filesystem metadata in these small neutral structs (the
 * style9-private class-0x2A calls fill them); this file then shapes them into
 * the macOS ABI the binary expects.  The layouts mirror kern/fs.h exactly -- a
 * private kernel<->libSystem wire format, never seen by an Apple binary, and
 * deliberately saying nothing about WHICH filesystem answered.  The asserts
 * are the same ones the kernel header carries: two hand-written copies of a
 * layout drift, and a drifted copyout would be silent.
 */
#define	FS_NAME_MAX	256

struct fs_dirent {
	uint64_t	fde_ino;
	uint64_t	fde_size;
	uint8_t		fde_is_dir;
	char		fde_name[FS_NAME_MAX];
};

struct fs_statbuf {
	uint64_t	fs_size;
	uint64_t	fs_ino;
	uint64_t	fs_alloced;
	uint64_t	fs_mtime_ns;
	uint64_t	fs_atime_ns;
	uint64_t	fs_ctime_ns;
	uint64_t	fs_btime_ns;
	uint32_t	fs_nlink;
	uint32_t	fs_uid;
	uint32_t	fs_gid;
	uint16_t	fs_mode;
	uint8_t		fs_is_dir;
};

_Static_assert(sizeof(struct fs_dirent) == 280, "must match kern/fs.h");
_Static_assert(sizeof(struct fs_statbuf) == 72, "must match kern/fs.h");

/* fs_stat backchannel: fills *sb; returns 0, or -1 (carry set) if absent. */
static long
s9_fs_stat(const char *path, struct fs_statbuf *sb)
{

	return (bsd_call(0x2A000002, (long)path, (long)sb, 0));
}

/* fs_readdir backchannel: fills *out; returns 1 (entry), 0 (end), -1 (error). */
static long
s9_fs_readdir(const char *path, uint32_t index, struct fs_dirent *out)
{

	return (bsd_call(0x2A000003, (long)path, (long)index, (long)out));
}

/*
 * uname backchannel: the kernel reports its (fabricated) identity card in this
 * neutral struct -- layout mirrors kern/darwin.h's struct darwin_uname exactly.
 * uname(3) below reshapes it into Apple's struct utsname.  Returns 0, or -1
 * (carry set) only if the pointer faults.
 */
#define	DARWIN_UNAME_FIELD	128

struct darwin_uname {
	char	un_sysname[DARWIN_UNAME_FIELD];
	char	un_nodename[DARWIN_UNAME_FIELD];
	char	un_release[DARWIN_UNAME_FIELD];
	char	un_version[DARWIN_UNAME_FIELD];
	char	un_machine[DARWIN_UNAME_FIELD];
};

static long
s9_uname(struct darwin_uname *out)
{
	return (bsd_call(0x2A000004, (long)out, 0, 0));
}

/*
 * stat$INODE64 / lstat$INODE64: a binary stat()s a path and reads st_mode (is
 * it a directory?), st_size, and st_ino (a path walker keys cycle detection on
 * st_ino).  We fill the macOS INODE64 struct stat layout here -- st_mode@4,
 * st_ino@8, st_size@96, ... -- so the ABI knowledge lives in libSystem while
 * the kernel reports only the neutral fs_statbuf.  The import names hold a
 * '$' no C identifier can spell, so ordinary functions are aliased onto them;
 * the read-only FS has no symlinks, so lstat is just stat.
 *
 * The permission bits, owner, link count and four timestamps used to be
 * invented here -- every file 0644, every directory 0755, every date the
 * epoch -- because the kernel reported only size, inode and a directory flag.
 * It now reports what the volume actually records, so this stops guessing;
 * `ls -l` prints the mode APFS stored and the day the file was written.
 * Where a filesystem genuinely has nothing to say (FAT has no owner) the
 * invention happens in the kernel, at the layer that knows which volume
 * answered, rather than here where it would apply to both.
 */
#define	STAT_BUF_SIZE	144		/* the $INODE64 struct stat */

int	stat_inode64(const char *path, void *buf) __asm__("_stat$INODE64");
int	lstat_inode64(const char *path, void *buf) __asm__("_lstat$INODE64");

/* Shape a filled fs_statbuf into Apple's struct stat.  Never fails. */
static void
stat_shape(const struct fs_statbuf *sb, void *buf)
{
	unsigned char	*p;
	int		 i;

	p = (unsigned char *)buf;
	for (i = 0; i < STAT_BUF_SIZE; i++)
		p[i] = 0;

	*(uint16_t *)(p + 4)   = sb->fs_mode;		/* st_mode    */
	*(uint16_t *)(p + 6)   = (uint16_t)sb->fs_nlink;
	*(uint64_t *)(p + 8)   = sb->fs_ino;
	*(uint32_t *)(p + 16)  = sb->fs_uid;
	*(uint32_t *)(p + 20)  = sb->fs_gid;
	/*
	 * The four timespecs.  The kernel reports one nanosecond count per
	 * time; splitting it into (seconds, nanoseconds) is this layer's job
	 * because the split is Apple's struct, not the filesystem's fact.
	 */
	*(int64_t *)(p + 32)   = (int64_t)(sb->fs_atime_ns / 1000000000ULL);
	*(int64_t *)(p + 40)   = (int64_t)(sb->fs_atime_ns % 1000000000ULL);
	*(int64_t *)(p + 48)   = (int64_t)(sb->fs_mtime_ns / 1000000000ULL);
	*(int64_t *)(p + 56)   = (int64_t)(sb->fs_mtime_ns % 1000000000ULL);
	*(int64_t *)(p + 64)   = (int64_t)(sb->fs_ctime_ns / 1000000000ULL);
	*(int64_t *)(p + 72)   = (int64_t)(sb->fs_ctime_ns % 1000000000ULL);
	*(int64_t *)(p + 80)   = (int64_t)(sb->fs_btime_ns / 1000000000ULL);
	*(int64_t *)(p + 88)   = (int64_t)(sb->fs_btime_ns % 1000000000ULL);
	*(int64_t *)(p + 96)   = (int64_t)sb->fs_size;
	/* st_blocks counts 512-byte units, always, whatever st_blksize says. */
	*(int64_t *)(p + 104)  = (int64_t)(sb->fs_alloced / 512u);
	*(uint32_t *)(p + 112) = 4096;			/* st_blksize */
}

int
stat_inode64(const char *path, void *buf)
{
	struct fs_statbuf	sb;

	if (s9_fs_stat(path, &sb) < 0)
		return (-1);				/* absent -> ENOENT */
	stat_shape(&sb, buf);
	return (0);
}

int
lstat_inode64(const char *path, void *buf)
{

	return (stat_inode64(path, buf));
}

/*
 * stat64/lstat64/fstat64: the pre-INODE64 symbol names.  Same struct
 * layout as the $INODE64 variants (both are the 144-byte 64-bit-inode
 * form), just the older exported spelling -- a binary built against a
 * lower deployment target (dash) binds these.  fstat64 classifies an OPEN
 * fd via the fs_fstat backchannel: the kernel says reg/chr/fifo in a
 * neutral struct and the S_IF* spelling happens here.
 */
struct s9_fdstat {			/* mirrors kern/darwin.h */
	uint32_t	fds_size;
	uint8_t		fds_kind;	/* 0 reg / 1 chr / 2 fifo */
};

int
stat64(const char *path, void *buf)
{

	return (stat_inode64(path, buf));
}

int
lstat64(const char *path, void *buf)
{

	return (stat_inode64(path, buf));
}

int
fstat64(int fd, void *buf)
{
	struct s9_fdstat	 ds;
	unsigned char		*p;
	int			 i;

	if (bsd_call(0x2A000005, fd, (long)&ds, 0) < 0)
		return (-1);

	p = (unsigned char *)buf;
	for (i = 0; i < STAT_BUF_SIZE; i++)
		p[i] = 0;
	switch (ds.fds_kind) {
	case 1:					/* console */
		*(uint16_t *)(p + 4) = 0x2190u;	/* S_IFCHR | 0620 */
		break;
	case 2:					/* pipe end */
		*(uint16_t *)(p + 4) = 0x1180u;	/* S_IFIFO | 0600 */
		break;
	default:				/* regular file */
		*(uint16_t *)(p + 4) = 0x81A4u;	/* S_IFREG | 0644 */
		*(int64_t  *)(p + 96) = (int64_t)(uint64_t)ds.fds_size;
		break;
	}
	*(uint16_t *)(p + 6)   = 1;			/* st_nlink   */
	*(uint32_t *)(p + 112) = 512;			/* st_blksize */
	return (0);
}

/*
 * fstat$INODE64: the same call under the name a binary built against a modern
 * deployment target imports.  Both spellings describe the identical 144-byte
 * struct -- INODE64 was about widening ino_t, which this layout already is.
 */
int	fstat_inode64(int fd, void *buf) __asm__("_fstat$INODE64");

int
fstat_inode64(int fd, void *buf)
{

	return (fstat64(fd, buf));
}

/*
 * faccessat: existence (and on this FS, any-permission) probe.  dash tests
 * X_OK on PATH candidates; present == executable on a volume where
 * everything readable is runnable-or-openable.  Only AT_FDCWD-relative
 * absolute paths exist here, so dirfd is moot.
 */
int
faccessat(int dirfd, const char *path, int mode, int flags)
{
	struct fs_statbuf	sb;

	(void)dirfd;
	(void)mode;
	(void)flags;
	if (s9_fs_stat(path, &sb) < 0)
		return (-1);			/* absent */
	return (0);
}

/* ---- directory streams (opendir / readdir / closedir) ------------------- */

/*
 * opendir/readdir/closedir over the fs_readdir backchannel.  An Apple binary
 * (tree(1)) imports the $INODE64 variants and treats DIR as opaque, so the
 * layout is ours: just the directory path and the next index.  The kernel side
 * is stateless -- we pass (path, index) and bump index -- and readdir returns a
 * pointer to a single embedded struct dirent it refills each call, exactly as
 * readdir(3) returns a pointer to internal storage.
 */
#define	DT_DIR	4
#define	DT_REG	8

struct dirent {				/* the macOS $INODE64 layout */
	uint64_t	d_ino;
	uint64_t	d_seekoff;
	uint16_t	d_reclen;
	uint16_t	d_namlen;
	uint8_t		d_type;
	char		d_name[1024];
};

typedef struct {
	char		dd_path[1024];
	uint32_t	dd_index;
	struct dirent	dd_de;
} DIR;

DIR	*opendir_inode64(const char *path) __asm__("_opendir$INODE64");
struct dirent *readdir_inode64(DIR *dp) __asm__("_readdir$INODE64");

DIR *
opendir_inode64(const char *path)
{
	struct fs_statbuf	sb;
	DIR			*dp;
	size_t			 i;

	if (s9_fs_stat(path, &sb) < 0 || sb.fs_is_dir == 0)
		return (NULL);			/* absent or not a directory */
	dp = (DIR *)malloc(sizeof(*dp));
	if (dp == NULL)
		return (NULL);
	for (i = 0; i + 1 < sizeof(dp->dd_path) && path[i] != '\0'; i++)
		dp->dd_path[i] = path[i];
	dp->dd_path[i] = '\0';
	dp->dd_index = 0;
	return (dp);
}

struct dirent *
readdir_inode64(DIR *dp)
{
	struct fs_dirent	kde;
	int			 i;

	if (dp == NULL)
		return (NULL);
	if (s9_fs_readdir(dp->dd_path, dp->dd_index, &kde) != 1)
		return (NULL);			/* end of directory or error */
	dp->dd_index++;

	dp->dd_de.d_ino = kde.fde_ino;
	dp->dd_de.d_seekoff = dp->dd_index;
	dp->dd_de.d_type = kde.fde_is_dir ? DT_DIR : DT_REG;
	for (i = 0; i + 1 < (int)sizeof(dp->dd_de.d_name) &&
	    kde.fde_name[i] != '\0'; i++)
		dp->dd_de.d_name[i] = kde.fde_name[i];
	dp->dd_de.d_name[i] = '\0';
	dp->dd_de.d_namlen = (uint16_t)i;
	dp->dd_de.d_reclen = (uint16_t)sizeof(struct dirent);
	return (&dp->dd_de);
}

/*
 * dirfd / fstatat: how a directory walker asks about an entry it just read.
 *
 * ls(1) does not stat "path/name" -- it opens the directory once and then
 * calls fstatat(dirfd(dirp), name, ...) per entry, which is the *at family's
 * whole point: the directory is named by an open descriptor rather than by a
 * path that could be replaced underneath the walk.
 *
 * This kernel has no *at syscalls and no descriptor for a directory: our
 * opendir is a path plus an index, and the DIR already remembers the path.
 * So a dirfd here is a token that finds the DIR again, and fstatat joins its
 * remembered path to the name.  That gives up exactly what *at was invented
 * to provide -- atomicity against a directory being moved mid-walk -- which
 * costs nothing on a read-only volume where no directory can move.  It is
 * worth naming the trade rather than letting the table look like an
 * implementation detail.
 */
#define	DIRFD_BASE	0x7D00		/* far above any real fd number */
#define	DIRFD_SLOTS	16

#define	AT_FDCWD	(-2)		/* macOS's, and it is not -100 */

static DIR	*dirfd_tab[DIRFD_SLOTS];

int
dirfd(DIR *dp)
{
	int	i;

	if (dp == NULL)
		return (-1);
	for (i = 0; i < DIRFD_SLOTS; i++) {
		if (dirfd_tab[i] == dp)
			return (DIRFD_BASE + i);
	}
	for (i = 0; i < DIRFD_SLOTS; i++) {
		if (dirfd_tab[i] == NULL) {
			dirfd_tab[i] = dp;
			return (DIRFD_BASE + i);
		}
	}
	g_errno = 24;					/* EMFILE */
	return (-1);
}

/*
 * Joining a directory descriptor's path to a relative name is this side's
 * job and stays here: a dirfd is a libSystem construct (dirfd_tab above), so
 * the kernel has never heard of it.  Distinct from the working directory,
 * which the kernel now owns -- AT_FDCWD below just hands the name straight
 * through and lets it resolve there.
 */
#define	AT_PATH_MAX	1024

int	fstatat_inode64(int fd, const char *name, void *buf, int flag)
	    __asm__("_fstatat$INODE64");

int
fstatat_inode64(int fd, const char *name, void *buf, int flag)
{
	char		 joined[AT_PATH_MAX];
	const char	*dir;
	size_t		 i;
	size_t		 j;

	(void)flag;			/* no symlinks: NOFOLLOW is moot */
	if (name == NULL)
		return (-1);
	/* An absolute name ignores the descriptor, by definition. */
	if (fd == AT_FDCWD || name[0] == '/')
		return (stat_inode64(name, buf));

	if (fd < DIRFD_BASE || fd >= DIRFD_BASE + DIRFD_SLOTS ||
	    dirfd_tab[fd - DIRFD_BASE] == NULL) {
		g_errno = 9;				/* EBADF */
		return (-1);
	}
	dir = dirfd_tab[fd - DIRFD_BASE]->dd_path;

	for (j = 0; dir[j] != '\0' && j + 1 < sizeof(joined); j++)
		joined[j] = dir[j];
	if (j > 0 && joined[j - 1] != '/' && j + 1 < sizeof(joined))
		joined[j++] = '/';
	for (i = 0; name[i] != '\0' && j + 1 < sizeof(joined); i++)
		joined[j++] = name[i];
	joined[j] = '\0';
	return (stat_inode64(joined, buf));
}

int
closedir(DIR *dp)
{
	int	i;

	if (dp == NULL)
		return (-1);
	for (i = 0; i < DIRFD_SLOTS; i++) {
		if (dirfd_tab[i] == dp)
			dirfd_tab[i] = NULL;
	}
	free(dp);
	return (0);
}

/* ---- more string / memory ----------------------------------------------- */

int
strcmp(const char *a, const char *b)
{

	while (*a != '\0' && *a == *b) {
		a++;
		b++;
	}
	return ((int)(unsigned char)*a - (int)(unsigned char)*b);
}

int
strncmp(const char *a, const char *b, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		if (a[i] != b[i] || a[i] == '\0')
			return ((int)(unsigned char)a[i] -
			    (int)(unsigned char)b[i]);
	}
	return (0);
}

char *
strncpy(char *dst, const char *src, size_t n)
{
	size_t	i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		dst[i] = src[i];
	for (; i < n; i++)
		dst[i] = '\0';
	return (dst);
}

static int
ascii_lower(int c)
{

	return ((c >= 'A' && c <= 'Z') ? c + 32 : c);
}

int
strcasecmp(const char *a, const char *b)
{

	while (*a != '\0' &&
	    ascii_lower((unsigned char)*a) == ascii_lower((unsigned char)*b)) {
		a++;
		b++;
	}
	return (ascii_lower((unsigned char)*a) - ascii_lower((unsigned char)*b));
}

/* C locale: collation is plain byte order. */
int
strcoll(const char *a, const char *b)
{

	return (strcmp(a, b));
}

char *
strstr(const char *hay, const char *needle)
{
	size_t	i;
	size_t	nl;

	if (*needle == '\0')
		return ((char *)(unsigned long)hay);
	nl = strlen(needle);
	for (; *hay != '\0'; hay++) {
		for (i = 0; i < nl && hay[i] == needle[i]; i++)
			;
		if (i == nl)
			return ((char *)(unsigned long)hay);
	}
	return (NULL);
}

char *
strtok(char *str, const char *delim)
{
	static char	*save;
	char		*tok;

	if (str != NULL)
		save = str;
	if (save == NULL)
		return (NULL);
	while (*save != '\0' && strchr(delim, (unsigned char)*save) != NULL)
		save++;
	if (*save == '\0') {
		save = NULL;
		return (NULL);
	}
	tok = save;
	while (*save != '\0' && strchr(delim, (unsigned char)*save) == NULL)
		save++;
	if (*save != '\0') {
		*save = '\0';
		save++;
	}
	return (tok);
}

unsigned long
strtoul(const char *s, char **end, int base)
{
	unsigned long	v;

	while (*s == ' ' || (*s >= '\t' && *s <= '\r'))
		s++;
	if (*s == '+' || *s == '-')
		s++;
	if ((base == 0 || base == 16) && s[0] == '0' &&
	    (s[1] == 'x' || s[1] == 'X')) {
		s += 2;
		base = 16;
	} else if (base == 0) {
		base = (s[0] == '0') ? 8 : 10;
	}
	v = 0;
	for (;;) {
		unsigned char	c;
		int		d;

		c = (unsigned char)*s;
		if (c >= '0' && c <= '9')
			d = c - '0';
		else if (c >= 'a' && c <= 'z')
			d = c - 'a' + 10;
		else if (c >= 'A' && c <= 'Z')
			d = c - 'A' + 10;
		else
			break;
		if (d >= base)
			break;
		v = v * (unsigned long)base + (unsigned long)d;
		s++;
	}
	if (end != NULL)
		*end = (char *)(unsigned long)s;
	return (v);
}

/*
 * strtol: the signed sibling of strtoul.  Consume an optional sign, convert the
 * magnitude with strtoul, then apply it.  gmp uses this to parse small decimal
 * fields; the LONG_MIN/MAX saturation a full libc does is unneeded here.
 */
long
strtol(const char *s, char **end, int base)
{
	const char	*p;
	unsigned long	 v;
	int		 neg;

	p = s;
	while (*p == ' ' || (*p >= '\t' && *p <= '\r'))
		p++;
	neg = 0;
	if (*p == '+' || *p == '-') {
		neg = (*p == '-');
		p++;
	}
	v = strtoul(p, end, base);
	return (neg ? -(long)v : (long)v);
}

/* __strcpy_chk: clang's _FORTIFY_SOURCE strcpy.  Bounded so it cannot overrun. */
char *
__strcpy_chk(char *dst, const char *src, size_t dstlen)
{
	size_t	i;

	for (i = 0; i + 1 < dstlen && src[i] != '\0'; i++)
		dst[i] = src[i];
	if (dstlen > 0)
		dst[i] = '\0';
	return (dst);
}

/* C locale: each byte maps to one wide character. */
size_t
mbstowcs(wchar_t *pwcs, const char *s, size_t n)
{
	size_t	i;

	if (pwcs == NULL)
		return (strlen(s));
	for (i = 0; i < n && s[i] != '\0'; i++)
		pwcs[i] = (wchar_t)(unsigned char)s[i];
	if (i < n)
		pwcs[i] = 0;
	return (i);
}

/* qsort: insertion sort -- the directories we host hold few entries. */
static void
qs_swap(unsigned char *a, unsigned char *b, size_t size)
{
	size_t	i;

	for (i = 0; i < size; i++) {
		unsigned char	t;

		t = a[i];
		a[i] = b[i];
		b[i] = t;
	}
}

void
qsort(void *base, size_t n, size_t size,
    int (*cmp)(const void *, const void *))
{
	unsigned char	*b;
	size_t		 i;
	size_t		 j;

	b = (unsigned char *)base;
	for (i = 1; i < n; i++) {
		for (j = i; j > 0 &&
		    cmp(b + (j - 1) * size, b + j * size) > 0; j--)
			qs_swap(b + (j - 1) * size, b + j * size, size);
	}
}

/* ---- more stdio --------------------------------------------------------- */

int
fputc(int c, FILE *fp)
{
	unsigned char	ch;

	if (fp == NULL)
		return (EOF);
	ch = (unsigned char)c;
	write(fp->fd, &ch, 1);
	return (c);
}

int
putc(int c, FILE *fp)
{

	return (fputc(c, fp));
}

int
fputs(const char *s, FILE *fp)
{

	if (fp == NULL)
		return (EOF);
	write(fp->fd, s, strlen(s));
	return (0);
}

char *
fgets(char *s, int size, FILE *fp)
{
	int	i;

	if (fp == NULL || size <= 0)
		return (NULL);
	for (i = 0; i < size - 1; ) {
		unsigned char	c;
		long		r;

		r = s_read(fp->fd, &c, 1);
		if (r <= 0) {
			if (r == 0) {
				fp->eof = 1;
				fp->aflags |= APPLE_SEOF;
			}
			break;
		}
		s[i++] = (char)c;
		if (c == '\n')
			break;
	}
	if (i == 0)
		return (NULL);
	s[i] = '\0';
	return (s);
}

/*
 * Read side of stdio.  Our streams are unbuffered, so getc/__srget reduce to a
 * single-byte read; a one-slot pushback backs ungetc.  factor invoked with
 * command-line operands never reads a stream, but gmp imports these, so they
 * must bind -- and behave if some other input path reaches them.
 */
int
fgetc(FILE *fp)
{
	unsigned char	ch;
	int		c;

	if (fp == NULL)
		return (EOF);
	if (fp->unget != EOF) {
		c = fp->unget;
		fp->unget = EOF;
		return (c);
	}
	if (s_read(fp->fd, &ch, 1) != 1) {
		fp->eof = 1;
		fp->aflags |= APPLE_SEOF;	/* the feof() inline reads this */
		return (EOF);
	}
	return ((int)ch);
}

int
getc(FILE *fp)
{

	return (fgetc(fp));
}

/*
 * __srget: the refill hook of Apple's inlined getc() macro, which has just
 * decremented _r below zero to get here.  Re-pin _r at 0 FIRST -- that is
 * what keeps the next inlined getc funneling back into this function
 * instead of dereferencing the fake buffer pointer -- then read.
 */
int
__srget(FILE *fp)
{

	if (fp == NULL)
		return (EOF);
	fp->rspace = 0;
	return (fgetc(fp));
}

int
ungetc(int c, FILE *fp)
{

	if (fp == NULL || c == EOF)
		return (EOF);
	fp->unget = (int)(unsigned char)c;
	fp->eof = 0;
	fp->aflags &= ~APPLE_SEOF;
	return (c);
}

/* No sticky error flag is tracked; report "no error". */
int
ferror(FILE *fp)
{

	(void)fp;
	return (0);
}

/*
 * fscanf: gmp links it for formatted input, but factor on the command line
 * never reaches a scan.  With no scan engine we report input failure (EOF) so a
 * caller sees "nothing matched" rather than undefined behaviour.
 */
int
fscanf(FILE *fp, const char *fmt, ...)
{

	(void)fp;
	(void)fmt;
	return (EOF);
}

/* ---- POSIX / locale stubs (present so the bind resolves) ---------------- */

/*
 * Minimal answers for a read-only, single-user, C-locale system: no passwd or
 * group database, no symlinks, no real-time clock, one charset.  Each returns
 * the neutral value that makes a CLI tool fall back to its numeric / ASCII /
 * epoch path -- enough for the bind to resolve and the tool to run.
 */
int	__mb_cur_max = 1;		/* C locale: single-byte encoding */

/*
 * isatty: classify an open fd via the fs_fstat backchannel.  The kernel
 * reports a console fd (implicit stdin/out/err, or an explicit CONSOLE
 * slot) as DARWIN_FDSTAT_CHR; a character device is a tty as far as a CLI
 * binary cares (interactive prompts, line-buffered output).  Files, pipes,
 * and bad fds are not ttys -- ENOTTY / EBADF.  An honest answer here is
 * what flips a no-argument dash into its interactive REPL.
 */
int
isatty(int fd)
{
	struct s9_fdstat	ds;

	if (bsd_call(0x2A000005, fd, (long)&ds, 0) < 0)
		return (0);			/* bad fd -> not a tty   */
	return (ds.fds_kind == 1);		/* DARWIN_FDSTAT_CHR -> tty */
}

char *
setlocale(int category, const char *locale)
{

	(void)category;
	(void)locale;
	return ((char *)"C");
}

/*
 * nl_langinfo(3): the locale's names for things.
 *
 * This returned "" for everything but the codeset until gls asked, and the
 * consequence was visible rather than theoretical: gnulib's strftime -- which
 * is what coreutils formats dates with -- gets the month abbreviation for %b
 * from ABMON_1 + tm_mon, so `ls -l` printed a blank where every month should
 * have been.  The C locale HAS these names; not returning them was the bug.
 *
 * The item numbers are Apple's <langinfo.h>, which is BSD's: CODESET 0, then
 * the date and time formats, AM/PM, seven day names, seven abbreviations,
 * twelve month names, twelve abbreviations.
 */
#define	NL_CODESET	0
#define	NL_D_T_FMT	1
#define	NL_D_FMT	2
#define	NL_T_FMT	3
#define	NL_T_FMT_AMPM	4
#define	NL_AM_STR	5
#define	NL_PM_STR	6
#define	NL_DAY_1	7		/* .. DAY_7    = 13 */
#define	NL_ABDAY_1	14		/* .. ABDAY_7  = 20 */
#define	NL_MON_1	21		/* .. MON_12   = 32 */
#define	NL_ABMON_1	33		/* .. ABMON_12 = 44 */

static const char *const nl_days[7] = {
	"Sunday", "Monday", "Tuesday", "Wednesday", "Thursday", "Friday",
	"Saturday"
};

static const char *const nl_abdays[7] = {
	"Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat"
};

static const char *const nl_months[12] = {
	"January", "February", "March", "April", "May", "June", "July",
	"August", "September", "October", "November", "December"
};

static const char *const nl_abmonths[12] = {
	"Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct",
	"Nov", "Dec"
};

char *
nl_langinfo(int item)
{

	if (item >= NL_ABMON_1 && item < NL_ABMON_1 + 12)
		return ((char *)nl_abmonths[item - NL_ABMON_1]);
	if (item >= NL_MON_1 && item < NL_MON_1 + 12)
		return ((char *)nl_months[item - NL_MON_1]);
	if (item >= NL_ABDAY_1 && item < NL_ABDAY_1 + 7)
		return ((char *)nl_abdays[item - NL_ABDAY_1]);
	if (item >= NL_DAY_1 && item < NL_DAY_1 + 7)
		return ((char *)nl_days[item - NL_DAY_1]);

	switch (item) {
	/* A non-UTF-8 codeset, so tools pick ASCII line-drawing over box glyphs. */
	case NL_CODESET:	return ((char *)"US-ASCII");
	case NL_D_T_FMT:	return ((char *)"%a %b %e %H:%M:%S %Y");
	case NL_D_FMT:		return ((char *)"%m/%d/%y");
	case NL_T_FMT:		return ((char *)"%H:%M:%S");
	case NL_T_FMT_AMPM:	return ((char *)"%I:%M:%S %p");
	case NL_AM_STR:		return ((char *)"AM");
	case NL_PM_STR:		return ((char *)"PM");
	default:		return ((char *)"");
	}
}

void *
getpwuid(unsigned int uid)
{

	(void)uid;
	return (NULL);			/* no passwd db -> numeric uid */
}

void *
getgrgid(unsigned int gid)
{

	(void)gid;
	return (NULL);			/* no group db -> numeric gid */
}

int
gethostname(char *name, size_t len)
{
	const char	*h = "style9";
	size_t		 i;

	if (name == NULL || len == 0)
		return (-1);
	for (i = 0; i + 1 < len && h[i] != '\0'; i++)
		name[i] = h[i];
	name[i] = '\0';
	return (0);
}

long
readlink(const char *path, char *buf, size_t bufsize)
{

	(void)path;
	(void)buf;
	(void)bufsize;
	return (-1);			/* no symlinks on the read-only FS */
}

char	*realpath_extsn(const char *path, char *resolved)
	    __asm__("_realpath$DARWIN_EXTSN");

char *
realpath_extsn(const char *path, char *resolved)
{
	char	*out;
	size_t	 i;

	/* Identity: our paths are already canonical (no symlinks, no ".."). */
	out = resolved != NULL ? resolved : (char *)malloc(1024);
	if (out == NULL)
		return (NULL);
	for (i = 0; i < 1023 && path[i] != '\0'; i++)
		out[i] = path[i];
	out[i] = '\0';
	return (out);
}

/*
 * Wall clock.  The kernel reads the CMOS RTC once at boot and reports the
 * anchor plus elapsed uptime, so these are cheap and monotonic.  UTC only --
 * no timezone database exists on this system, and gettimeofday's second
 * argument has been ignored by real systems for decades.
 */
struct s9_timeval {			/* mirrors kern/darwin.h exactly */
	long		tv_sec;
	int		tv_usec;
	int		tv_pad;
};

/* The carry-capturing trap wrapper; defined with the process-control block. */
static long	bsd_call_e(long nr, long a, long b, long c);

int
gettimeofday(struct s9_timeval *tv, void *tz)
{

	(void)tz;
	return ((int)bsd_call_e(0x2000074, (long)tv, 0, 0));
}

long
time(long *t)
{
	struct s9_timeval	tv;

	if (gettimeofday(&tv, (void *)0) != 0)
		tv.tv_sec = 0;		/* no clock: (time_t)-1 is the C answer,
					   but 0 is what our callers can print */
	if (t != NULL)
		*t = tv.tv_sec;
	return (tv.tv_sec);
}

/*
 * clock_gettime.  Every clock id gets the same answer here, and that is a
 * fact about this system rather than a shortcut: the kernel's wall time IS
 * uptime plus a boot-time anchor that nothing can change afterwards, so it
 * already has the property MONOTONIC exists to promise -- it cannot be
 * stepped, slewed, or set backwards.  The only difference from a textbook
 * MONOTONIC clock is which instant counts as zero, and a program measuring an
 * interval subtracts two readings and never notices.
 */
struct s9_timespec {
	long	tv_sec;
	long	tv_nsec;
};

int
clock_gettime(int clk, struct s9_timespec *ts)
{
	struct s9_timeval	tv;

	(void)clk;
	if (ts == (void *)0)
		return (-1);
	if (gettimeofday(&tv, (void *)0) != 0)
		return (-1);
	ts->tv_sec  = tv.tv_sec;
	ts->tv_nsec = (long)tv.tv_usec * 1000L;
	return (0);
}

void *
localtime(const long *t)
{
	static long	tmbuf[16];	/* a zeroed struct tm, amply sized */

	(void)t;
	return (tmbuf);
}

size_t
strftime(char *s, size_t max, const char *fmt, const void *tm)
{

	(void)fmt;
	(void)tm;
	if (s != NULL && max > 0)
		s[0] = '\0';
	return (0);
}

/* ---- guname rung: the machine-identity trick + the libc it pulls in ------ */

/*
 * guname (GNU coreutils' uname, a real Apple x86-64 bottle) asks the system
 * what it is and prints the answer.  The whole illusion rides on uname(3): we
 * fetch the kernel's fabricated identity card and reshape it into Apple's
 * struct utsname.  guname does not validate a single field -- it cannot tell it
 * is not on a Mac, because it never asks anything but uname().  The remaining
 * symbols below are the small, ordinary libc guname drags in (the bind needs
 * every name present); each is a complete implementation, not a stub, except
 * where a read-only single-user C-locale system has nothing real to return.
 */

#define	_SYS_NAMELEN	256

struct utsname {
	char	sysname[_SYS_NAMELEN];
	char	nodename[_SYS_NAMELEN];
	char	release[_SYS_NAMELEN];
	char	version[_SYS_NAMELEN];
	char	machine[_SYS_NAMELEN];
};

/* Copy one NUL-terminated identity field into a 256-byte utsname slot. */
static void
un_field(char *dst, const char *src)
{
	size_t	i;

	for (i = 0; i + 1 < _SYS_NAMELEN && i < DARWIN_UNAME_FIELD &&
	    src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

int
uname(struct utsname *u)
{
	struct darwin_uname	k;

	if (u == NULL)
		return (-1);
	if (s9_uname(&k) < 0)
		return (-1);
	un_field(u->sysname, k.un_sysname);
	un_field(u->nodename, k.un_nodename);
	un_field(u->release, k.un_release);
	un_field(u->version, k.un_version);
	un_field(u->machine, k.un_machine);
	return (0);
}

/* errno's accessor; the cell itself is declared at the top of the file. */
int *
__error(void)
{

	return (&g_errno);
}

/*
 * mmap(2) / munmap(2).
 *
 * The only six-argument syscall in this file, and the argument that makes it
 * six is the one the SYSCALL instruction cannot carry in %rcx -- so arg3
 * (flags) travels in %r10, and args 4 and 5 (fd, offset) in %r8 and %r9,
 * exactly as Apple's stubs load them.
 *
 * Failure is MAP_FAILED, not NULL: address zero is a legal mapping in
 * principle, so mmap has never been allowed to use it as the error value.
 */
static long
bsd_call6_e(long nr, long a, long b, long c, long d, long e, long f)
{
	register long	r10 __asm__("r10");
	register long	r8  __asm__("r8");
	register long	r9  __asm__("r9");
	long		ret;
	unsigned char	cf;

	r10 = d;
	r8  = e;
	r9  = f;
	__asm__ __volatile__(
	    "syscall\n\t"
	    "setc %1\n"
	    : "=a"(ret), "=r"(cf)
	    : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10), "r"(r8), "r"(r9)
	    : "rcx", "r11", "memory");
	if (cf) {
		g_errno = (int)ret;
		return (-1);
	}
	return (ret);
}

void *
mmap(void *addr, size_t len, int prot, int flags, int fd, long off)
{
	long	rv;

	rv = bsd_call6_e(0x20000C5, (long)addr, (long)len, prot, flags,
	    fd, off);
	if (rv == -1)
		return (MAP_FAILED);
	return ((void *)rv);
}

int
munmap(void *addr, size_t len)
{

	return ((int)bsd_call6_e(0x2000049, (long)addr, (long)len, 0, 0, 0, 0));
}

/*
 * ioctl: nothing here is a terminal in the sense a device ioctl means, so this
 * fails rather than pretending.  That is safe by inspection, not by hope --
 * gcat's only ioctl is FIONREAD (0x4004667f) on its input, and the code right
 * after the call accepts ENOTSUP, ENOTTY or EINVAL and takes the ordinary
 * read-loop path; any OTHER errno makes it abort with "cannot do ioctl on %s".
 * So the stub must fail with one of those three, and ENOTTY is the true one.
 */
int
ioctl(int fd, unsigned long request, ...)
{

	(void)fd;
	(void)request;
	g_errno = 25;					/* ENOTTY */
	return (-1);
}

/*
 * MB_CUR_MAX on modern macOS expands to (___mb_cur_max()) -- a CALL, not the
 * legacy `int __mb_cur_max` data symbol we also export above.  C/US-ASCII
 * locale: one byte per character.
 */
int
___mb_cur_max(void)
{

	return (1);
}

/* _exit(2): terminate now, skipping the atexit handlers exit(3) runs. */
void
_exit(int code)
{

	dsys(0x2000001, code, 0, 0);
	for (;;)
		;
}

void
abort(void)
{

	write(2, "abort\n", 6);
	_exit(134);				/* 128 + SIGABRT */
	for (;;)
		;
}

/*
 * __assert_rtn: Darwin's assert(3) failure handler.  A tripped assertion in
 * gfactor or gmp must terminate, not return, so print a diagnostic and abort.
 */
void
__assert_rtn(const char *func, const char *file, int line, const char *expr)
{

	(void)line;
	write(2, "Assertion failed: ", 18);
	if (expr != NULL)
		write(2, expr, strlen(expr));
	write(2, " (", 2);
	if (func != NULL)
		write(2, func, strlen(func));
	write(2, ", ", 2);
	if (file != NULL)
		write(2, file, strlen(file));
	write(2, ")\n", 2);
	abort();
}

/*
 * raise: we deliver no signals.  raise(SIGABRT) must still terminate -- it is
 * how abort(3) is specified to act -- so route it to abort; any other signal is
 * a no-op success (there is no handler to run).
 */
int
raise(int sig)
{

	if (sig == 6)				/* SIGABRT */
		abort();
	return (0);
}

int
atexit(void (*fn)(void))
{

	if (fn == NULL || atexit_n >= ATEXIT_MAX)
		return (-1);
	atexit_fns[atexit_n++] = fn;
	return (0);
}

void *
calloc(size_t n, size_t size)
{
	size_t	total;
	void	*p;

	total = n * size;
	if (n != 0 && total / n != size)
		return (NULL);			/* multiplication overflow */
	p = malloc(total);
	if (p != NULL)
		(void)memset(p, 0, total);
	return (p);
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const unsigned char	*pa;
	const unsigned char	*pb;
	size_t			 i;

	pa = (const unsigned char *)a;
	pb = (const unsigned char *)b;
	for (i = 0; i < n; i++) {
		if (pa[i] != pb[i])
			return ((int)pa[i] - (int)pb[i]);
	}
	return (0);
}

/*
 * memmove: copy n bytes, correct even when the regions overlap.  gmp's mpn
 * shifting and gfactor both pull it in; memcpy alone is not enough because the
 * source and destination can alias.
 */
void *
memmove(void *dst, const void *src, size_t n)
{
	unsigned char		*d;
	const unsigned char	*s;
	size_t			 i;

	d = (unsigned char *)dst;
	s = (const unsigned char *)src;
	if (d == s || n == 0)
		return (dst);
	if (d < s) {
		for (i = 0; i < n; i++)
			d[i] = s[i];
	} else {
		for (i = n; i > 0; i--)
			d[i - 1] = s[i - 1];
	}
	return (dst);
}

/*
 * __memmove_chk / __memset_chk: clang's _FORTIFY_SOURCE wrappers.  When the
 * compiler cannot size the destination it passes (size_t)-1, so the check is a
 * no-op; a genuine overflow (n > dstlen) aborts, as real Darwin does.
 */
void *
__memmove_chk(void *dst, const void *src, size_t n, size_t dstlen)
{

	if (n > dstlen) {
		write(2, "libSystem: __memmove_chk overflow\n", 34);
		abort();
	}
	return (memmove(dst, src, n));
}

void *
__memset_chk(void *dst, int c, size_t n, size_t dstlen)
{

	if (n > dstlen) {
		write(2, "libSystem: __memset_chk overflow\n", 33);
		abort();
	}
	return (memset(dst, c, n));
}

/* Length of the leading run of *s consisting only of bytes in `set`. */
size_t
strspn(const char *s, const char *set)
{
	size_t	i;

	for (i = 0; s[i] != '\0' &&
	    strchr(set, (unsigned char)s[i]) != NULL; i++)
		;
	return (i);
}

/* Length of the leading run of *s consisting of bytes NOT in `set`. */
size_t
strcspn(const char *s, const char *set)
{
	size_t	i;

	for (i = 0; s[i] != '\0' &&
	    strchr(set, (unsigned char)s[i]) == NULL; i++)
		;
	return (i);
}

/* C locale: one byte -> one wide character; conversion is stateless. */
size_t
mbrtowc(wchar_t *pwc, const char *s, size_t n, void *ps)
{

	(void)ps;
	if (s == NULL)
		return (0);			/* state reset */
	if (n == 0)
		return ((size_t)-2);		/* incomplete */
	if (pwc != NULL)
		*pwc = (wchar_t)(unsigned char)*s;
	return (*s == '\0' ? 0 : 1);
}

int
mbsinit(const void *ps)
{

	(void)ps;
	return (1);				/* always the initial state */
}

long
lseek(int fd, long off, int whence)
{

	return (s_lseek(fd, off, whence));
}

/*
 * Public read(2)/close(2)/open(2): until the process rung, every consumer
 * went through stdio and only the s_* internals existed.  pipefork reads
 * its pipe end raw, shell-shaped tools close fds they dup2'd away, and a
 * shell open(2)s its script file directly -- through the errno-recording
 * trap wrapper (defined with the process-control section below) so its
 * "cannot open x" diagnostics name the real reason.
 */
static long	bsd_call_e(long nr, long a, long b, long c);

long
read(int fd, void *buf, unsigned long n)
{

	return (s_read(fd, buf, n));
}

int
close(int fd)
{

	return (s_close(fd));
}

int
open(const char *path, int flags, ...)
{

	return ((int)bsd_call_e(0x2000005, (long)path, flags, 0));
}

/*
 * fcntl: F_DUPFD (cmd 0) has real semantics -- a shell parks its saved std
 * fds at 10+ with it around redirections -- so it travels to the kernel
 * (Darwin #92), which duplicates the slot at the lowest free fd >= arg.
 * The flag commands stay accepted-and-ignored: there is nothing to set on
 * this fd table.
 */
int
fcntl(int fd, int cmd, ...)
{
	__builtin_va_list	ap;
	long			arg;

	__builtin_va_start(ap, cmd);
	arg = __builtin_va_arg(ap, long);
	__builtin_va_end(ap);
	/*
	 * Every command travels to the kernel -- it knows which ones carry
	 * semantics (F_DUPFD and its CLOEXEC twin) and answers the flag
	 * commands itself.  Swallowing a command here would hand the caller
	 * a fake 0, and 0 is a valid fd: dash once "moved" its script to
	 * stdin that way.
	 */
	return ((int)bsd_call_e(0x200005C, fd, cmd, arg));
}

/* Our stdio is unbuffered -- writes hit the kernel immediately. */
int
fflush(FILE *fp)
{

	(void)fp;
	return (0);
}

int
fileno(FILE *fp)
{

	return (fp != NULL ? fp->fd : -1);
}

/* Single-threaded: the FILE locks are no-ops. */
void
flockfile(FILE *fp)
{

	(void)fp;
}

void
funlockfile(FILE *fp)
{

	(void)fp;
}

/* No buffered bytes to discard. */
int
fpurge(FILE *fp)
{

	(void)fp;
	return (0);
}

int
fseeko(FILE *fp, int64_t off, int whence)
{

	if (fp == NULL)
		return (-1);
	if (s_lseek(fp->fd, (long)off, whence) < 0)
		return (-1);			/* e.g. a non-seekable std stream */
	fp->eof = 0;
	fp->aflags &= ~APPLE_SEOF;
	return (0);
}

int64_t
ftello(FILE *fp)
{

	if (fp == NULL)
		return (-1);
	return ((int64_t)s_lseek(fp->fd, 0, 1));	/* SEEK_CUR */
}

int
putc_unlocked(int c, FILE *fp)
{

	return (fputc(c, fp));
}

int
vfprintf(FILE *fp, const char *fmt, __builtin_va_list ap)
{

	return (vfmt(fp != NULL ? fp->fd : 2, NULL, 0, fmt, ap));
}

/*
 * vsprintf: unbounded formatted write into `dst`.  We have no way to know the
 * buffer size, so the cap is SIZE_MAX -- gmp's callers size dst from the value
 * being printed, the same trust the real vsprintf extends.
 */
int
vsprintf(char *dst, const char *fmt, __builtin_va_list ap)
{

	return (vfmt(-1, dst, (size_t)-1, fmt, ap));
}

/*
 * set/getprogname(): the program's short name, the one every coreutils
 * diagnostic prefixes itself with.  Our dyld calls setprogname with argv[0]
 * before entering LC_MAIN, which is where a real system does it too -- Darwin
 * has no crt0 doing this either; libSystem's own init takes it off the handoff
 * stack.  Until then the name is a placeholder rather than a lie about which
 * program is speaking.
 */
static const char	*g_progname = "darwin";

void
setprogname(const char *name)
{
	const char	*p;

	if (name == NULL)
		return;
	/* BSD semantics: store the last path component, not the whole path. */
	for (p = name; *p != '\0'; p++)
		if (*p == '/')
			name = p + 1;
	g_progname = name;
}

const char *
getprogname(void)
{

	return (g_progname);
}

/*
 * XSI strerror_r.  There is a real errno table further down this file; this
 * used to answer "Unknown error" to everything, which turned a program's
 * perfectly good diagnostic into a shrug.
 */
char	*strerror(int errnum);

int
strerror_r(int errnum, char *buf, size_t buflen)
{
	const char	*m;
	size_t		 i;

	m = strerror(errnum);
	if (buf == NULL || buflen == 0)
		return (0);
	for (i = 0; i + 1 < buflen && m[i] != '\0'; i++)
		buf[i] = m[i];
	buf[i] = '\0';
	return (0);
}

/* ---- stack protector ---------------------------------------------------- */

/*
 * The canary: figlet's prologues load ___stack_chk_guard and its epilogues
 * compare.  Any consistent value works since both sides read this one global;
 * a real system randomises it at startup, which we have no entropy source for.
 */
void	*__stack_chk_guard = (void *)0x595e9fbd94fda766ULL;

void
__stack_chk_fail(void)
{

	write(2, "libSystem: stack smashing detected\n", 35);
	exit(134);				/* 128 + SIGABRT */
}

/*
 * ____chkstk_darwin: the stack-probe thunk clang emits ahead of a large or
 * variable-length frame.  Its contract is to probe (touch) the requested span
 * -- passed in %rax -- below %rsp, preserving every register including %rax (the
 * caller subtracts it from %rsp afterward) and never touching %rsp itself.  Our
 * ring-3 stack is fully pre-mapped (no demand-paged guard region to fault in),
 * so probing is unnecessary: a bare `ret` is a correct, register-clean thunk as
 * long as the frame fits the mapped stack -- which the launcher sizes for.
 */
__asm__(
	".text\n"
	".globl ____chkstk_darwin\n"
	"____chkstk_darwin:\n"
	"\tret\n"
);

/*
 * dyld_stub_binder lives in libdyld inside Apple's libSystem umbrella and is
 * the target of classic lazy-binding stubs.  We bind with chained fixups,
 * which need no stub binder, so this is vestigial -- but exporting it keeps
 * the link working regardless of fixup mode, and our dyld binds eagerly, so
 * it is never actually entered.
 */
__asm__(
	".globl dyld_stub_binder\n"
	".globl _dyld_stub_binder\n"
	"dyld_stub_binder:\n"
	"_dyld_stub_binder:\n"
	"\tud2\n"
);

/* ---- process control (fork / exec / wait / pipes) ------------------------ */

/*
 * Carry-capturing BSD syscalls that also set errno.  The process-control
 * wrappers below report failure through the errno protocol -- coreutils'
 * gnulib branches on ENOENT vs EACCES after a failed exec, on ECHILD
 * after wait -- unlike the early fd routines (bsd_call) that predate
 * g_errno.  The 4-argument form loads %r10, the SYSCALL slot for arg3
 * (wait4's rusage pointer).
 */
static long
bsd_call_e(long nr, long a, long b, long c)
{
	long		ret;
	unsigned char	cf;

	__asm__ __volatile__(
	    "syscall\n\t"
	    "setc %1\n"
	    : "=a"(ret), "=r"(cf)
	    : "a"(nr), "D"(a), "S"(b), "d"(c)
	    : "rcx", "r11", "memory");
	if (cf) {
		g_errno = (int)ret;
		return (-1);
	}
	return (ret);
}

static long
bsd_call4_e(long nr, long a, long b, long c, long d)
{
	register long	r10 __asm__("r10");
	long		ret;
	unsigned char	cf;

	r10 = d;
	__asm__ __volatile__(
	    "syscall\n\t"
	    "setc %1\n"
	    : "=a"(ret), "=r"(cf)
	    : "a"(nr), "D"(a), "S"(b), "d"(c), "r"(r10)
	    : "rcx", "r11", "memory");
	if (cf) {
		g_errno = (int)ret;
		return (-1);
	}
	return (ret);
}

/*
 * fork(2).  The kernel rebuilds the child's register file from almost
 * nothing: the child re-enters userspace at the instruction after this
 * `syscall` with only %rax (= 0), %rsp, and %rip guaranteed.  That is
 * exactly the C ABI's caller-save set gone -- so the wrapper parks the
 * six callee-saved registers on the stack first.  The address-space copy
 * duplicates that stack, and parent and child alike restore from their
 * own copy on the way out.  Carry set means no child: errno in %rax,
 * fold to -1 via fork_fail (kept out-of-line so the hot path is pop+ret).
 */
long
fork_fail(long err)
{

	g_errno = (int)err;
	return (-1);
}

__asm__(
	".text\n"
	".globl _fork\n"
	"_fork:\n"
	"\tpushq %rbx\n"
	"\tpushq %rbp\n"
	"\tpushq %r12\n"
	"\tpushq %r13\n"
	"\tpushq %r14\n"
	"\tpushq %r15\n"
	"\tmovl $0x2000002, %eax\n"
	"\tsyscall\n"
	"\tjnc 1f\n"
	"\tmovq %rax, %rdi\n"
	"\tcall _fork_fail\n"
	"1:\n"
	"\tpopq %r15\n"
	"\tpopq %r14\n"
	"\tpopq %r13\n"
	"\tpopq %r12\n"
	"\tpopq %rbp\n"
	"\tpopq %rbx\n"
	"\tret\n"
);

extern int	fork(void);

/*
 * vfork: with a full-copy fork underneath, fork semantics are a strict
 * superset of what a vfork caller may rely on (child and parent own
 * private stacks, so the child's "until it execs" window cannot scribble
 * on the parent).
 */
int
vfork(void)
{

	return (fork());
}

/* execve(2): only ever returns on failure, -1 with errno set. */
int
execve(const char *path, char *const argv[], char *const envp[])
{

	return ((int)bsd_call_e(0x200003B, (long)path, (long)argv,
	    (long)envp));
}

int
execv(const char *path, char *const argv[])
{

	return (execve(path, argv, NULL));
}

/*
 * execvp: PATH search collapses to a single try -- the kernel resolves
 * the final path component against its program registry, so a bare name
 * and any absolute spelling land on the same image.
 */
int
execvp(const char *file, char *const argv[])
{

	return (execve(file, argv, NULL));
}

int
wait4(int pid, int *status, int options, void *rusage)
{

	return ((int)bsd_call4_e(0x2000007, pid, (long)status, options,
	    (long)rusage));
}

int
waitpid(int pid, int *status, int options)
{

	return (wait4(pid, status, options, NULL));
}

int
wait(int *status)
{

	return (wait4(-1, status, 0, NULL));
}

/*
 * pipe(2): the kernel hands both ends back packed in %rax (read end in
 * the low half, write end in the high half) -- see kern/darwin.c for why
 * the native %rax/%rdx convention is not used.  This wrapper is the only
 * caller, so the packing is private ABI between it and the kernel.
 */
int
pipe(int fds[2])
{
	long	rv;

	rv = bsd_call_e(0x200002A, 0, 0, 0);
	if (rv < 0)
		return (-1);
	fds[0] = (int)(rv & 0x7FFFFFFF);
	fds[1] = (int)((unsigned long)rv >> 32);
	return (0);
}

int
dup(int fd)
{

	return ((int)bsd_call_e(0x2000029, fd, 0, 0));
}

int
dup2(int oldfd, int newfd)
{

	return ((int)bsd_call_e(0x200005A, oldfd, newfd, 0));
}

int
kill(int pid, int sig)
{

	return ((int)bsd_call_e(0x2000025, pid, sig, 0));
}

int
getppid(void)
{

	return ((int)dsys(0x2000027, 0, 0, 0));
}

/*
 * Signal management.  The kernel delivers signals now: sigaction/signal
 * record a ring-3 handler (or SIG_DFL/SIG_IGN) and hand the kernel the
 * address of the trampoline below (arg2, like Apple's sa_tramp) so it knows
 * where to enter ring 3 on a caught signal.  Apple's sigset_t is a 32-bit
 * mask; struct sigaction leads with the handler at offset 0.
 *
 * _sig_tramp is where the kernel resumes ring 3 on a caught signal.  It is
 * entered with rdi=signo, rsi=siginfo, rdx=ucontext, r10=handler and rsp
 * 16-aligned-plus-8 (as if called).  It calls the handler, then issues
 * sigreturn(ucontext) (SYS_sigreturn = 0x20000B8), which never returns.
 *
 * THE UCONTEXT IS CARRIED ON THE STACK, NOT IN A CALLEE-SAVED REGISTER, and
 * that is not a style choice.  A synchronous frame -- one built at a syscall's
 * exit -- saves only rip, rsp, rflags and rax, because everything else is
 * guaranteed by the calling convention: the interrupted point is a syscall
 * return, and callee-saved registers hold what they held before the call.
 * That guarantee is the trampoline's to keep.
 *
 * The earlier version kept the ucontext in rbx, having pushed the old value
 * first.  The pop never happened: sigreturn does not return, so the push was
 * a promise nothing could keep, and ring 3 resumed with the ucontext ADDRESS
 * where its own variable used to be.  The symptom was a local turning into a
 * plausible-looking stack pointer, and it took a test that let a signal
 * interrupt a blocking read to see it -- every earlier signal test either
 * kept nothing live across the call or came in on the asynchronous path,
 * which saves the whole machine state and restores it through IRETQ.
 *
 * A push and a pop, on the other hand, cost the same and are kept by the
 * ordinary rules: the value lives below the frame the kernel just wrote, the
 * handler cannot see it, and no register the interrupted code owns is touched.
 */
extern void	sig_tramp(void);

__asm__(
	".globl _sig_tramp\n"
	"_sig_tramp:\n"
	"\tpushq %rdx\n"		/* ucontext; also realigns for the call */
	"\tmovq %r10, %rax\n"		/* handler                             */
	"\tcall *%rax\n"		/* handler(signo, siginfo, ucontext)   */
	"\tpopq %rdi\n"			/* ucontext -> sigreturn arg0          */
	"\tmovq $0x20000B8, %rax\n"	/* SYS_sigreturn (184), BSD class      */
	"\tsyscall\n"
	"\tud2\n"			/* sigreturn does not return           */
);

int
sigaction(int sig, const void *act, void *oact)
{
	long	handler;

	handler = (act != NULL) ? (long)*(const unsigned long *)act : 0;
	if (oact != NULL)
		(void)memset(oact, 0, 16);
	return ((int)bsd_call_e(0x200002E, sig, handler, (long)&sig_tramp));
}

void *
signal(int sig, void *handler)
{

	/*
	 * signal(3) records through the same path as sigaction, handing the
	 * kernel the trampoline too.  We do not track the previous
	 * disposition, so report SIG_DFL (NULL).
	 */
	(void)bsd_call_e(0x200002E, sig, (long)handler, (long)&sig_tramp);
	return (NULL);
}

int
sigprocmask(int how, const void *set, void *oset)
{
	unsigned int	newmask;
	long		old;

	/*
	 * Apple's sigset_t is a 32-bit mask.  A NULL set means "query only":
	 * pass how == 0 so the kernel leaves the mask untouched and hands back
	 * the current one in %rax (SYS_sigprocmask = 0x2000030) for *oset.
	 */
	newmask = (set != NULL) ? *(const unsigned int *)set : 0u;
	if (set == NULL)
		how = 0;
	old = bsd_call_e(0x2000030, how, (long)newmask, 0);
	if (oset != NULL)
		*(unsigned int *)oset = (unsigned int)old;
	return (0);
}

int
sigemptyset(void *set)
{

	if (set != NULL)
		(void)memset(set, 0, 4);
	return (0);
}

int
sigaddset(void *set, int sig)
{

	(void)set;
	(void)sig;
	return (0);
}

int
sigismember(const void *set, int sig)
{

	(void)set;
	(void)sig;
	return (0);
}

int
setitimer(int which, const void *val, void *oval)
{

	(void)which;
	(void)val;
	(void)oval;
	return (0);
}

unsigned int
alarm(unsigned int secs)
{

	(void)secs;
	return (0);
}

/* ---- the genv / gtimeout gap ---------------------------------------------- */

/*
 * The process environment.  The kernel passes no envp, so this library IS
 * the environment's source of truth: a single PATH entry pointing at the
 * synthetic /bin (the program registry presented as a directory by
 * kern/darwin.c), which is where every runnable thing on this system
 * lives.  A shell imports it and its PATH search then stats and execs
 * straight out of /bin; env(1) prints it.
 *
 * Both Apple access routes land on the same storage: older binaries bind
 * the `environ` data symbol directly, newer ones call _NSGetEnviron()
 * (crt does not vend `environ` from a dylib, but our dyld binds data
 * symbols fine, so we can simply export it).
 */
static char	*environ_default[] = { (char *)"PATH=/bin", 0 };

char	**environ = environ_default;

char ***
_NSGetEnviron(void)
{

	return (&environ);
}

/*
 * The kernel has a working directory now, so these stop pretending.
 *
 * What they used to be is worth recording, because the shape of the lie is
 * instructive: chdir(2) returned success without doing anything, getcwd(3)
 * answered "/" whatever had happened, and a helper on this side of the
 * syscall rewrote relative paths against that imaginary root.  Nothing ever
 * reported an error, so a program that changed directory and then opened a
 * relative name simply got a different file than it asked for.
 */
int
chdir(const char *path)
{

	return ((int)bsd_call_e(0x2000000 | 12, (long)path, 0, 0));
}

int
unsetenv(const char *name)
{

	(void)name;
	return (0);
}

int
setenv(const char *name, const char *value, int overwrite)
{

	(void)name;
	(void)value;
	(void)overwrite;
	return (0);
}

/* Process groups and resource limits do not exist yet: report success. */
int
setpgid(int pid, int pgid)
{

	(void)pid;
	(void)pgid;
	return (0);
}

int
setrlimit(int which, const void *rlp)
{

	(void)which;
	(void)rlp;
	return (0);
}

/* Apple sigset_t is a 32-bit mask; these two complete the sigset family. */
int
sigfillset(void *set)
{

	if (set != NULL)
		*(unsigned int *)set = 0xFFFFFFFFu;
	return (0);
}

int
sigdelset(void *set, int sig)
{

	if (set != NULL && sig >= 1 && sig <= 32)
		*(unsigned int *)set &= ~(1u << (sig - 1));
	return (0);
}

/*
 * sigsuspend: POSIX blocks here until a signal fires; no signal will ever
 * fire, so return the mandated -1/EINTR immediately.  gtimeout's wait loop
 * is `while (waitpid(pid, .., WNOHANG) == 0) sigsuspend(..)` -- with an
 * immediate EINTR that degrades to polling, and the loop still terminates
 * the moment the child exits.
 */
int
sigsuspend(const void *mask)
{

	(void)mask;
	g_errno = 4;				/* EINTR */
	return (-1);
}

/* No locale machinery: failure is the documented, handled answer. */
void *
newlocale(int mask, const char *name, void *base)
{

	(void)mask;
	(void)name;
	(void)base;
	return (NULL);
}

/* Leftmost byte of *s that appears in `set`, or NULL. */
char *
strpbrk(const char *s, const char *set)
{
	size_t	i;

	for (; *s != '\0'; s++) {
		for (i = 0; set[i] != '\0'; i++) {
			if (*s == set[i])
				return ((char *)s);
		}
	}
	return (NULL);
}

/*
 * strtod: decimal + optional fraction + optional e-notation -- what
 * gtimeout's duration parser ("10", "1.5", "2e1") consumes.  No hex
 * floats, no INF/NAN spellings; SSE2 double arithmetic is the Penryn
 * baseline, so plain multiply-accumulate is fine.
 */
double
strtod(const char *s, char **endp)
{
	const char	*p;
	double		 frac;
	double		 val;
	int		 esign;
	int		 expn;
	int		 i;
	int		 neg;

	p = s;
	while (*p == ' ' || *p == '\t' || *p == '\n')
		p++;
	neg = 0;
	if (*p == '+' || *p == '-') {
		neg = (*p == '-');
		p++;
	}
	val = 0.0;
	while (*p >= '0' && *p <= '9') {
		val = val * 10.0 + (double)(*p - '0');
		p++;
	}
	if (*p == '.') {
		p++;
		frac = 0.1;
		while (*p >= '0' && *p <= '9') {
			val += (double)(*p - '0') * frac;
			frac *= 0.1;
			p++;
		}
	}
	if (*p == 'e' || *p == 'E') {
		p++;
		esign = 0;
		if (*p == '+' || *p == '-') {
			esign = (*p == '-');
			p++;
		}
		expn = 0;
		while (*p >= '0' && *p <= '9') {
			expn = expn * 10 + (*p - '0');
			p++;
		}
		for (i = 0; i < expn; i++)
			val = esign ? val / 10.0 : val * 10.0;
	}
	if (endp != NULL)
		*endp = (char *)p;
	return (neg ? -val : val);
}

double
strtod_l(const char *s, char **endp, void *loc)
{

	(void)loc;
	return (strtod(s, endp));
}

/* ---- the shell rung (dash) ----------------------------------------------- */

/*
 * Everything below exists because a real POSIX shell (Homebrew dash) binds
 * it.  A shell is the most demanding libc consumer yet: it longjmps out of
 * errors, saves fds around redirections, walks PATH with stat, asks who it
 * is, and parses with the wide-char and string family.  The answers stay
 * true to this system: single user (root), one process group, no ttys on
 * the serial console, a read-only volume.
 */

/*
 * setjmp/longjmp -- the real thing, in asm; dash's error handling (exraise)
 * longjmps across arbitrary call depth, so no stub survives contact.  Both
 * jumpers are OUR code (this library is the only setjmp provider in the
 * closure), so the jmp_buf layout is private: the six callee-saved
 * registers + rsp + rip = 64 bytes, comfortably inside Apple's 148-byte
 * jmp_buf.  C `setjmp` does not save the signal mask on Darwin (`sigsetjmp`
 * does); our masks are no-ops anyway, so the register file is the entire
 * context.  longjmp(env, 0) must deliver 1, per POSIX.
 */
__asm__(
	".text\n"
	".globl _setjmp\n"
	"_setjmp:\n"
	"\tmovq %rbx,  0(%rdi)\n"
	"\tmovq %rbp,  8(%rdi)\n"
	"\tmovq %r12, 16(%rdi)\n"
	"\tmovq %r13, 24(%rdi)\n"
	"\tmovq %r14, 32(%rdi)\n"
	"\tmovq %r15, 40(%rdi)\n"
	"\tleaq 8(%rsp), %rax\n"
	"\tmovq %rax, 48(%rdi)\n"
	"\tmovq (%rsp), %rax\n"
	"\tmovq %rax, 56(%rdi)\n"
	"\txorl %eax, %eax\n"
	"\tret\n"
	".globl _longjmp\n"
	"_longjmp:\n"
	"\tmovq  0(%rdi), %rbx\n"
	"\tmovq  8(%rdi), %rbp\n"
	"\tmovq 16(%rdi), %r12\n"
	"\tmovq 24(%rdi), %r13\n"
	"\tmovq 32(%rdi), %r14\n"
	"\tmovq 40(%rdi), %r15\n"
	"\tmovq 48(%rdi), %rsp\n"
	"\tmovl %esi, %eax\n"
	"\ttestl %eax, %eax\n"
	"\tjnz 1f\n"
	"\tmovl $1, %eax\n"
	"1:\tjmpq *56(%rdi)\n"
);

/*
 * Identity: one user, root, one group, one process group (its leader being
 * whoever asks).  dash compares uid==euid to decide privileged mode --
 * equal answers keep it in normal mode.
 */
int
getuid(void)
{

	return (0);
}

int
geteuid(void)
{

	return (0);
}

int
getgid(void)
{

	return (0);
}

int
getegid(void)
{

	return (0);
}

int
getpgrp(void)
{

	return (getpid());
}

/* No passwd database: tilde expansion of ~user finds nobody. */
void *
getpwnam(const char *name)
{

	(void)name;
	return (NULL);
}

/*
 * getcwd(3) over __getcwd(2), which is how Darwin's own libc does it: the
 * syscall fills a caller-supplied buffer or fails with ERANGE, and the
 * allocating form is a courtesy this side adds.
 */
#define	GETCWD_MAX	256		/* matches DARWIN_PATH_MAX */

char *
getcwd(char *buf, size_t size)
{
	char	tmp[GETCWD_MAX];
	size_t	n;

	if (buf == NULL) {
		/*
		 * The POSIX extension: allocate one that fits.  Asking the
		 * kernel into a local first means the allocation is sized to
		 * the answer rather than to the maximum, and that a failure
		 * costs no malloc at all.
		 */
		if (bsd_call_e(0x2000000 | 326, (long)tmp,
		    (long)sizeof(tmp), 0) < 0)
			return (NULL);
		for (n = 0; tmp[n] != '\0'; n++)
			continue;
		if (size != 0 && size < n + 1) {
			g_errno = 34;			/* ERANGE */
			return (NULL);
		}
		buf = (char *)malloc(n + 1);
		if (buf == NULL)
			return (NULL);
		for (n = 0; ; n++) {
			buf[n] = tmp[n];
			if (tmp[n] == '\0')
				break;
		}
		return (buf);
	}
	if (bsd_call_e(0x2000000 | 326, (long)buf, (long)size, 0) < 0)
		return (NULL);
	return (buf);
}

/* umask on a read-only volume: the historical default, never consulted. */
int
umask(int mask)
{

	(void)mask;
	return (022);
}

/*
 * unlink(2), which reaches the kernel now that the volume can be written.
 *
 * It used to answer EROFS from here without asking anybody, which was true of
 * every volume this system could mount at the time.  It is not true any more,
 * and a libc that keeps saying so is a libc that makes the kernel look
 * broken.
 */
int
unlink(const char *path)
{

	return ((int)bsd_call_e(0x200000A, (long)path, 0, 0));
}

int
mkstemp(char *tmpl)
{

	(void)tmpl;
	g_errno = 30;				/* EROFS */
	return (-1);
}

/*
 * Resource limits: everything unlimited (Darwin RLIM_INFINITY).  dash's
 * ulimit builtin reads these; struct rlimit is two 64-bit counts.
 */
int
getrlimit(int which, void *rlp)
{
	unsigned long long	*r;

	(void)which;
	r = (unsigned long long *)rlp;
	r[0] = 0x7FFFFFFFFFFFFFFFULL;		/* rlim_cur */
	r[1] = 0x7FFFFFFFFFFFFFFFULL;		/* rlim_max */
	return (0);
}

/*
 * sysconf: dash asks for the clock tick to scale its times builtin.
 * struct tms below answers in those (fictional) ticks.
 */
long
sysconf(int name)
{

	switch (name) {
	case 3:					/* _SC_CLK_TCK */
		return (100);
	case 5:					/* _SC_OPEN_MAX */
		return (32);
	case 29:				/* _SC_PAGESIZE */
		return (4096);
	default:
		return (-1);
	}
}

/* No CPU accounting yet: every process has consumed zero ticks. */
long
times(void *buf)
{
	unsigned long	*t;
	int		 i;

	t = (unsigned long *)buf;
	for (i = 0; i < 4; i++)
		t[i] = 0;
	return (0);
}

/*
 * killpg: with one process group per session, the group IS the process --
 * forward to kill(2).  Only dash's interactive job control sends group
 * signals, so this is bind-resolution insurance more than a hot path.
 */
int
killpg(int pgrp, int sig)
{

	return (kill(pgrp, sig));
}

/* The pre-sigprocmask mask call: masks are no-ops here (see sigprocmask). */
int
sigsetmask(int mask)
{

	(void)mask;
	return (0);
}

/* wait3 is wait4 with "any child" implied. */
int
wait3(int *status, int options, void *rusage)
{

	return (wait4(-1, status, options, rusage));
}

/*
 * Terminal control: the serial console is not a tty (isatty already says
 * 0), so the termios family reports ENOTTY consistently.  dash only walks
 * this path when deciding whether to start job control; a uniform "no
 * terminal" keeps it non-interactive.
 */
int
tcgetattr(int fd, void *termios_p)
{

	(void)fd;
	(void)termios_p;
	g_errno = 25;				/* ENOTTY */
	return (-1);
}

int
tcgetpgrp(int fd)
{

	(void)fd;
	g_errno = 25;				/* ENOTTY */
	return (-1);
}

int
tcsetpgrp(int fd, int pgrp)
{

	(void)fd;
	(void)pgrp;
	g_errno = 25;				/* ENOTTY */
	return (-1);
}

/*
 * fdopen (Apple exports it as fdopen$DARWIN_EXTSN under unix2003
 * versioning): wrap an existing fd in a fresh FILE.  Our FILE is just
 * {fd, eof, unget}, so the mode string only needs to exist.
 */
FILE	*fdopen_extsn(int fd, const char *mode) __asm__("_fdopen$DARWIN_EXTSN");

FILE *
fdopen_extsn(int fd, const char *mode)
{
	FILE	*fp;

	(void)mode;
	if (fd < 0)
		return (NULL);
	fp = (FILE *)malloc(sizeof(*fp));
	if (fp == NULL)
		return (NULL);
	fp->fd     = fd;
	fp->eof    = 0;
	fp->rspace = 0;
	fp->wspace = 0;
	fp->aflags = 0;
	fp->afile  = (short)fd;
	fp->unget  = EOF;
	return (fp);
}

/* ---- string/scan helpers a shell leans on -------------------------------- */

void *
bsearch(const void *key, const void *base, size_t nmemb, size_t size,
    int (*compar)(const void *, const void *))
{
	const unsigned char	*b;
	size_t			 lo;
	size_t			 hi;
	size_t			 mid;
	int			 c;

	b  = (const unsigned char *)base;
	lo = 0;
	hi = nmemb;
	while (lo < hi) {
		mid = lo + (hi - lo) / 2;
		c = compar(key, b + mid * size);
		if (c == 0)
			return ((void *)(b + mid * size));
		if (c < 0)
			hi = mid;
		else
			lo = mid + 1;
	}
	return (NULL);
}

char *
stpcpy(char *dst, const char *src)
{

	while ((*dst = *src) != '\0') {
		dst++;
		src++;
	}
	return (dst);
}

char *
stpncpy(char *dst, const char *src, size_t n)
{
	size_t	end;
	size_t	i;

	for (i = 0; i < n && src[i] != '\0'; i++)
		dst[i] = src[i];
	end = i;			/* dst+end: the NUL, or dst+n  */
	for (; i < n; i++)
		dst[i] = '\0';
	return (dst + end);
}

char *
strdup(const char *s)
{
	char	*d;
	size_t	 n;

	n = strlen(s) + 1;
	d = (char *)malloc(n);
	if (d == NULL)
		return (NULL);
	memcpy(d, s, n);
	return (d);
}

/* intmax_t == long on LP64: the strto*max family is strtol/strtoul. */
long long
strtoimax(const char *s, char **end, int base)
{

	return (strtol(s, end, base));
}

unsigned long long
strtoumax(const char *s, char **end, int base)
{

	return (strtoul(s, end, base));
}

/*
 * strerror/strsignal: name the errnos this system can actually produce
 * (kern/darwin.h's set); anything else formats numerically into a static
 * buffer, which is all the POSIX lifetime contract requires.
 */
static char	strerror_buf[32];

static void
strerror_fmt(const char *prefix, int n)
{
	size_t	i;
	int	 d;

	for (i = 0; prefix[i] != '\0'; i++)
		strerror_buf[i] = prefix[i];
	if (n < 0) {
		strerror_buf[i++] = '-';
		n = -n;
	}
	d = (n >= 100) ? 100 : (n >= 10) ? 10 : 1;
	for (; d > 0; d /= 10)
		strerror_buf[i++] = (char)('0' + (n / d) % 10);
	strerror_buf[i] = '\0';
}

char *
strerror(int errnum)
{

	switch (errnum) {
	case 1:	 return ((char *)"Operation not permitted");
	case 2:	 return ((char *)"No such file or directory");
	case 3:	 return ((char *)"No such process");
	case 4:	 return ((char *)"Interrupted system call");
	case 5:	 return ((char *)"Input/output error");
	case 8:	 return ((char *)"Exec format error");
	case 9:	 return ((char *)"Bad file descriptor");
	case 10: return ((char *)"No child processes");
	case 12: return ((char *)"Cannot allocate memory");
	case 13: return ((char *)"Permission denied");
	case 14: return ((char *)"Bad address");
	case 22: return ((char *)"Invalid argument");
	case 24: return ((char *)"Too many open files");
	case 25: return ((char *)"Inappropriate ioctl for device");
	case 29: return ((char *)"Illegal seek");
	case 30: return ((char *)"Read-only file system");
	case 32: return ((char *)"Broken pipe");
	case 34: return ((char *)"Result too large");
	case 78: return ((char *)"Function not implemented");
	default:
		strerror_fmt("Unknown error: ", errnum);
		return (strerror_buf);
	}
}

char *
strsignal(int sig)
{

	switch (sig) {
	case 1:	 return ((char *)"Hangup");
	case 2:	 return ((char *)"Interrupt");
	case 3:	 return ((char *)"Quit");
	case 6:	 return ((char *)"Abort trap");
	case 9:	 return ((char *)"Killed");
	case 13: return ((char *)"Broken pipe");
	case 15: return ((char *)"Terminated");
	default:
		strerror_fmt("Signal ", sig);
		return (strerror_buf);
	}
}

/*
 * Wide/multibyte: the C locale is single-byte (__mb_cur_max == 1), so
 * every conversion is a 1:1 byte<->wchar walk and no shift state exists.
 */
size_t
mbrlen(const char *s, size_t n, void *ps)
{

	(void)ps;
	if (s == NULL)
		return (0);
	if (n == 0)
		return ((size_t)-2);		/* incomplete (no bytes) */
	return (*s == '\0' ? 0 : 1);
}

size_t
mbsrtowcs(wchar_t *dst, const char **src, size_t len, void *ps)
{
	const char	*s;
	size_t		 i;

	(void)ps;
	s = *src;
	if (dst == NULL)
		return (strlen(s));
	for (i = 0; i < len; i++) {
		dst[i] = (wchar_t)(unsigned char)s[i];
		if (s[i] == '\0') {
			*src = NULL;
			return (i);
		}
	}
	*src = s + len;
	return (len);
}

wchar_t *
wcschr(const wchar_t *ws, wchar_t wc)
{

	for (;; ws++) {
		if (*ws == wc)
			return ((wchar_t *)ws);
		if (*ws == 0)
			return (NULL);
	}
}

/* No named character classes: 0 is wctype(3)'s documented "no such class". */
unsigned long
wctype(const char *property)
{

	(void)property;
	return (0);
}

/* ---- gls rung: the calendar, the mode string, and the rest of ls(1) ------ */

/*
 * gls (GNU coreutils 9.11's ls) is the ninth real Apple binary, and the first
 * one that asks the system what it REMEMBERS rather than what it is: a mode
 * word, an owner, a link count and a date, per file.  Every one of those was
 * being invented in this file until the kernel learned to carry them out of
 * the inode -- so most of what follows is presentation for facts that now
 * arrive from the disk, not substitutes for them.
 *
 * The exceptions are named where they occur: this volume has no ACLs, no
 * group database and no timezone, and saying so plainly beats approximating
 * any of the three.
 */

/*
 * struct tm, Apple's layout: nine ints, then the two BSD extensions.  The
 * date formatting ls(1) actually uses is gnulib's own strftime replacement,
 * compiled into the binary; it reads tm_gmtoff and tm_zone, so both are here
 * and both say UTC.
 */
struct tm {
	int	 tm_sec;
	int	 tm_min;
	int	 tm_hour;
	int	 tm_mday;
	int	 tm_mon;		/* 0-11 */
	int	 tm_year;		/* years since 1900 */
	int	 tm_wday;		/* 0 = Sunday */
	int	 tm_yday;		/* 0-365 */
	int	 tm_isdst;
	long	 tm_gmtoff;
	char	*tm_zone;
};

/*
 * The calendar, in both directions, by the same trick: shift the year to
 * begin in March so that February's length stops being a special case, count
 * whole 400-year eras, and let one linear formula carry the month lengths.
 * Leap years and century rules fall out of the era arithmetic instead of
 * being tested for.  (Howard Hinnant's days_from_civil / civil_from_days; the
 * kernel has the forward half in kern/fs_fat.c for the same reason.)
 */
static int64_t
days_from_civil(int32_t y, uint32_t m, uint32_t d)
{
	int64_t		era;
	uint32_t	yoe;
	uint32_t	doy;
	uint32_t	doe;

	y -= (int32_t)(m <= 2u);
	era = (int64_t)(y >= 0 ? y : y - 399) / 400;
	yoe = (uint32_t)((int64_t)y - era * 400);
	doy = (153u * (m > 2u ? m - 3u : m + 9u) + 2u) / 5u + d - 1u;
	doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
	return (era * 146097 + (int64_t)doe - 719468);
}

static void
civil_from_days(int64_t z, int *y_out, unsigned *m_out, unsigned *d_out)
{
	int64_t		era;
	int64_t		y;
	uint64_t	doe;		/* day of era   [0, 146096] */
	uint64_t	yoe;		/* year of era  [0, 399]    */
	uint64_t	doy;		/* day of year, March-based */
	uint64_t	mp;		/* month        [0, 11]     */

	z += 719468;			/* move the epoch to 0000-03-01 */
	era = (z >= 0 ? z : z - 146096) / 146097;
	doe = (uint64_t)(z - era * 146097);
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y   = (int64_t)yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp  = (5 * doy + 2) / 153;
	*d_out = (unsigned)(doy - (153 * mp + 2) / 5 + 1);
	*m_out = (unsigned)(mp < 10 ? mp + 3 : mp - 9);
	*y_out = (int)(y + (*m_out <= 2));
}

/*
 * gmtime_r: seconds since the epoch to a broken-down UTC time.
 *
 * localtime_r is the same function.  That is not laziness: this system reads
 * one clock, the CMOS RTC, and has no timezone database, no TZ handling and
 * nothing that could tell it what offset the machine sits at.  Choosing one
 * would mean printing every timestamp wrong by it.  UTC is what the hardware
 * said, and tm_zone says so.
 */
struct tm *
gmtime_r(const int64_t *t, struct tm *tm)
{
	int64_t		secs;
	int64_t		days;
	int64_t		rem;
	unsigned	mon;
	unsigned	day;
	int		year;

	if (t == NULL || tm == NULL)
		return (NULL);
	secs = *t;

	/*
	 * Floor division, not truncation: a pre-1970 timestamp divided the C
	 * way rounds toward zero and lands a day late with a negative
	 * remainder.  Nothing on this volume is that old, but a date routine
	 * that is right for only half the number line is a trap for whoever
	 * reaches for it next.
	 */
	days = secs / 86400;
	rem  = secs % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}

	civil_from_days(days, &year, &mon, &day);
	tm->tm_sec    = (int)(rem % 60);
	tm->tm_min    = (int)((rem / 60) % 60);
	tm->tm_hour   = (int)(rem / 3600);
	tm->tm_mday   = (int)day;
	tm->tm_mon    = (int)mon - 1;
	tm->tm_year   = year - 1900;
	/* 1970-01-01 was a Thursday (4); the +11 keeps the modulus positive. */
	tm->tm_wday   = (int)(((days % 7) + 11) % 7);
	tm->tm_yday   = (int)(days - days_from_civil(year, 1, 1));
	tm->tm_isdst  = 0;
	tm->tm_gmtoff = 0;
	tm->tm_zone   = (char *)"UTC";
	return (tm);
}

struct tm *
localtime_r(const int64_t *t, struct tm *tm)
{

	return (gmtime_r(t, tm));
}

/*
 * tzset(3): read the timezone from the environment.  There is no timezone to
 * read and no database to read it from, so this is a genuine no-op rather
 * than an unimplemented stub -- the state it would set is already correct.
 */
void
tzset(void)
{
}

/*
 * strmode(3): the "drwxr-xr-x " a long listing opens with, BSD's spelling,
 * including the trailing space.  That space is where macOS puts the '+' or
 * '@' marking an ACL or an extended attribute; this volume has neither, so
 * it stays blank rather than being dropped -- the column belongs there.
 */
void
strmode(int mode, char *p)
{

	switch (mode & 0170000) {
	case 0040000:	*p++ = 'd'; break;
	case 0100000:	*p++ = '-'; break;
	case 0120000:	*p++ = 'l'; break;
	case 0020000:	*p++ = 'c'; break;
	case 0060000:	*p++ = 'b'; break;
	case 0010000:	*p++ = 'p'; break;
	case 0140000:	*p++ = 's'; break;
	default:	*p++ = '?'; break;
	}

	*p++ = (mode & 0400) ? 'r' : '-';
	*p++ = (mode & 0200) ? 'w' : '-';
	if ((mode & 04000) != 0)
		*p++ = (mode & 0100) ? 's' : 'S';
	else
		*p++ = (mode & 0100) ? 'x' : '-';

	*p++ = (mode & 0040) ? 'r' : '-';
	*p++ = (mode & 0020) ? 'w' : '-';
	if ((mode & 02000) != 0)
		*p++ = (mode & 0010) ? 's' : 'S';
	else
		*p++ = (mode & 0010) ? 'x' : '-';

	*p++ = (mode & 0004) ? 'r' : '-';
	*p++ = (mode & 0002) ? 'w' : '-';
	if ((mode & 01000) != 0)
		*p++ = (mode & 0001) ? 't' : 'T';
	else
		*p++ = (mode & 0001) ? 'x' : '-';

	*p++ = ' ';
	*p = '\0';
}

/* ---- the small libc gls drags in with it -------------------------------- */

size_t
strnlen(const char *s, size_t maxlen)
{
	size_t	n;

	for (n = 0; n < maxlen && s[n] != '\0'; n++)
		continue;
	return (n);
}

/*
 * __memcpy_chk: what _FORTIFY_SOURCE compiles a memcpy into when the
 * destination's size is known.  A copy larger than that size is a detected
 * overflow, and the contract is to abort rather than to truncate: truncating
 * would hide the bug the check exists to find.
 */
void *
__memcpy_chk(void *dst, const void *src, size_t len, size_t dstlen)
{

	if (len > dstlen)
		abort();
	return (memcpy(dst, src, len));
}

/*
 * Wide characters in the C locale, where every byte is its own character.
 * wcwidth is how ls(1) aligns columns: one column for a printable character,
 * zero for the null, and -1 for a control character, whose effect on the
 * cursor a column counter cannot predict.
 */
int
wcwidth(wchar_t wc)
{

	if (wc == 0)
		return (0);
	if (wc < 32 || (wc >= 0x7F && wc < 0xA0))
		return (-1);
	return (1);
}

int
btowc(int c)
{

	return (c == -1 ? -1 : (int)(unsigned char)c);
}

wchar_t *
wmemchr(const wchar_t *s, wchar_t c, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		if (s[i] == c)
			return ((wchar_t *)&s[i]);
	}
	return (NULL);
}

wchar_t *
wmemcpy(wchar_t *dst, const wchar_t *src, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
	return (dst);
}

/*
 * The locale, in the two shapes a program asks for it.  setlocale(3) above
 * already answers "C"; these are the query paths.  localeconv's answer is the
 * C locale's by definition -- "." for the decimal point, empty strings for
 * everything monetary, and CHAR_MAX for every numeric field, which is how the
 * standard spells "this locale does not specify one".
 */
struct lconv {
	char	*decimal_point;
	char	*thousands_sep;
	char	*grouping;
	char	*int_curr_symbol;
	char	*currency_symbol;
	char	*mon_decimal_point;
	char	*mon_thousands_sep;
	char	*mon_grouping;
	char	*positive_sign;
	char	*negative_sign;
	char	 int_frac_digits;
	char	 frac_digits;
	char	 p_cs_precedes;
	char	 p_sep_by_space;
	char	 n_cs_precedes;
	char	 n_sep_by_space;
	char	 p_sign_posn;
	char	 n_sign_posn;
};

struct lconv *
localeconv(void)
{
	static struct lconv	lc;
	static char		empty[] = "";
	static char		dot[] = ".";

	lc.decimal_point     = dot;
	lc.thousands_sep     = empty;
	lc.grouping          = empty;
	lc.int_curr_symbol   = empty;
	lc.currency_symbol   = empty;
	lc.mon_decimal_point = empty;
	lc.mon_thousands_sep = empty;
	lc.mon_grouping      = empty;
	lc.positive_sign     = empty;
	lc.negative_sign     = empty;
	lc.int_frac_digits   = (char)127;	/* CHAR_MAX = unspecified */
	lc.frac_digits       = (char)127;
	lc.p_cs_precedes     = (char)127;
	lc.p_sep_by_space    = (char)127;
	lc.n_cs_precedes     = (char)127;
	lc.n_sep_by_space    = (char)127;
	lc.p_sign_posn       = (char)127;
	lc.n_sign_posn       = (char)127;
	return (&lc);
}

/*
 * uselocale(3): install a thread's locale and return the previous one.  There
 * is one locale and it is C, so the previous one is always the global locale
 * -- returned as a non-null token because NULL is uselocale's ERROR return,
 * and a caller that checks would otherwise see a failure that did not happen.
 */
void *
uselocale(void *loc)
{
	static int	global_locale;

	(void)loc;
	return (&global_locale);
}

/* MB_CUR_MAX for an explicitly named locale.  Whichever it is, it is C. */
int
___mb_cur_max_l(void *loc)
{

	(void)loc;
	return (1);
}

/*
 * The group database, which does not exist.  getgrgid above answers NULL to
 * the same question asked by number.  ls(1) falls back to printing the
 * numeric gid, which is the honest rendering of a system where group 0 has
 * no name to print.
 */
void *
getgrnam(const char *name)
{

	(void)name;
	return (NULL);
}

/*
 * POSIX.1e ACLs, which this filesystem does not have.  ls(1) calls
 * acl_get_file (or acl_get_link_np) on every entry to decide whether to print
 * the '+' that marks an extended ACL, and acl_get_entry to see whether what
 * came back holds anything.
 *
 * NULL alone is not the answer, and getting that wrong was visible: gnulib
 * reads a NULL return as an ERROR unless errno says the system does not do
 * ACLs, so `ls -l` printed a bare "gls: /usr" line -- an error report with an
 * empty message, because errno happened to be 0 -- before every single entry
 * it then listed correctly.  ENOTSUP is both the truthful answer and the one
 * gnulib's ACL_NOT_WELL_SUPPORTED() accepts as "no ACLs here, carry on".
 */
#define	DARWIN_ENOTSUP	45

void *
acl_get_file(const char *path, unsigned int type)
{

	(void)path;
	(void)type;
	g_errno = DARWIN_ENOTSUP;
	return (NULL);
}

void *
acl_get_link_np(const char *path, unsigned int type)
{

	(void)path;
	(void)type;
	g_errno = DARWIN_ENOTSUP;
	return (NULL);
}

void *
acl_get_fd_np(int fd, unsigned int type)
{

	(void)fd;
	(void)type;
	g_errno = DARWIN_ENOTSUP;
	return (NULL);
}

int
acl_get_entry(void *acl, int entry_id, void **entry_p)
{

	(void)acl;
	(void)entry_id;
	(void)entry_p;
	return (-1);				/* no entries, ever */
}

int
acl_free(void *obj)
{

	(void)obj;
	return (0);
}

/*
 * pthread mutexes.  Every process here has exactly one thread, so a lock is
 * uncontended by construction: these are not stubs standing in for missing
 * synchronisation, they are what correct synchronisation degenerates to when
 * there is nobody to race against.  The day this system grows threads inside
 * a Darwin process, these become real and the compiler will not remind us --
 * which is why it is written down here.
 */
int
pthread_mutex_lock(void *m)
{

	(void)m;
	return (0);
}

int
pthread_mutex_unlock(void *m)
{

	(void)m;
	return (0);
}
