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
	return (cf ? -1 : ret);
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

/* ---- malloc: a bump allocator over a static zero-fill arena ------------- */

/*
 * No VM syscalls are wired to ring 3 yet, so malloc hands out bytes from a
 * fixed BSS arena (the Mach-O loader zero-fills the segment's bss tail, so the
 * arena starts zeroed).  Each block carries a 16-byte header holding its
 * usable size, so realloc() can copy the old contents forward; free() is a
 * no-op, as the programs we host allocate near-monotonically and never depend
 * on reclamation.  The arena is 16-aligned and every size is rounded to 16, so
 * returned pointers meet the alignment the SSE string paths assume.  4 MiB
 * covers a font plus working buffers with room to spare; an exhausted arena
 * returns NULL exactly as a real malloc would.
 */
#define	ARENA_SIZE	(4u * 1024u * 1024u)

static unsigned char	arena[ARENA_SIZE] __attribute__((aligned(16)));
static size_t		arena_off;

void *
malloc(size_t n)
{
	size_t	need;
	size_t	off;

	n = (n + 15u) & ~(size_t)15u;		/* 16-byte alignment */
	if (n == 0)
		n = 16;
	need = n + 16u;				/* + a 16-byte size header */
	if (need < n || need > ARENA_SIZE || arena_off > ARENA_SIZE - need)
		return (NULL);
	off = arena_off;
	arena_off += need;
	*(size_t *)(void *)&arena[off] = n;	/* usable size, for realloc */
	return (&arena[off + 16]);
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

char *
getenv(const char *name)
{

	(void)name;
	return (NULL);				/* no environment in ring 3 */
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
 * figlet treats FILE as opaque -- it only passes FILE* to the stdio calls --
 * so the layout is entirely ours: just the underlying fd.  The std streams use
 * fds 0/1/2; fopen() gets a real fd from the kernel's read-only filesystem
 * (kern/fs_fat.c, reached via the BSD open/read/lseek/close calls).
 */
typedef struct __sFILE {
	int	fd;
	int	eof;
} FILE;

static FILE	__stdin_file  = { 0, 0 };
static FILE	__stdout_file = { 1, 0 };
static FILE	__stderr_file = { 2, 0 };

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
			if (r == 0)
				fp->eof = 1;
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
	fp->fd  = fd;
	fp->eof = 0;
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
 */
int	 opterr = 1;
int	 optind = 1;
int	 optopt = 0;
char	*optarg = NULL;

static const char	*g_place = "";

int
getopt(int argc, char *const argv[], const char *optstring)
{
	const char	*oli;
	int		 c;

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
 * The kernel reports filesystem metadata in these small neutral structs (the
 * style9-private class-0x2A calls fill them); this file then shapes them into
 * the macOS ABI the binary expects.  The layouts mirror kern/fs_fat.h exactly
 * -- a private kernel<->libSystem wire format, never seen by an Apple binary.
 */
#define	FS_FAT_NAME_MAX	64

struct fs_fat_dirent {
	uint32_t	fde_ino;
	uint32_t	fde_size;
	uint8_t		fde_is_dir;
	char		fde_name[FS_FAT_NAME_MAX];
};

struct fs_fat_statbuf {
	uint32_t	fs_size;
	uint32_t	fs_ino;
	uint8_t		fs_is_dir;
};

/* fs_stat backchannel: fills *sb; returns 0, or -1 (carry set) if absent. */
static long
s9_fs_stat(const char *path, struct fs_fat_statbuf *sb)
{
	return (bsd_call(0x2A000002, (long)path, (long)sb, 0));
}

/* fs_readdir backchannel: fills *out; returns 1 (entry), 0 (end), -1 (error). */
static long
s9_fs_readdir(const char *path, uint32_t index, struct fs_fat_dirent *out)
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
 * the kernel reports only the neutral fs_fat_statbuf.  The import names hold a
 * '$' no C identifier can spell, so ordinary functions are aliased onto them;
 * the read-only FS has no symlinks, so lstat is just stat.
 */
int	stat_inode64(const char *path, void *buf) __asm__("_stat$INODE64");
int	lstat_inode64(const char *path, void *buf) __asm__("_lstat$INODE64");

int
stat_inode64(const char *path, void *buf)
{
	struct fs_fat_statbuf	sb;
	unsigned char		*p;
	int			 i;

	if (s9_fs_stat(path, &sb) < 0)
		return (-1);				/* absent -> ENOENT */

	p = (unsigned char *)buf;
	for (i = 0; i < 144; i++)
		p[i] = 0;
	/* st_mode: S_IFDIR|0755 for a directory, else S_IFREG|0644. */
	*(uint16_t *)(p + 4)   = sb.fs_is_dir ? 0x41EDu : 0x81A4u;
	*(uint16_t *)(p + 6)   = 1;			 /* st_nlink   */
	*(uint64_t *)(p + 8)   = sb.fs_ino;		 /* st_ino     */
	*(int64_t  *)(p + 96)  = (int64_t)(uint64_t)sb.fs_size;	/* st_size   */
	*(int64_t  *)(p + 104) =
	    (int64_t)(((uint64_t)sb.fs_size + 511) / 512);	/* st_blocks */
	*(uint32_t *)(p + 112) = 512;			 /* st_blksize */
	return (0);
}

int
lstat_inode64(const char *path, void *buf)
{

	return (stat_inode64(path, buf));
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
	struct fs_fat_statbuf	sb;
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
	struct fs_fat_dirent	kde;
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

int
closedir(DIR *dp)
{

	if (dp == NULL)
		return (-1);
	free(dp);				/* arena free is a no-op */
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
			if (r == 0)
				fp->eof = 1;
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

/* ---- POSIX / locale stubs (present so the bind resolves) ---------------- */

/*
 * Minimal answers for a read-only, single-user, C-locale system: no passwd or
 * group database, no symlinks, no real-time clock, one charset.  Each returns
 * the neutral value that makes a CLI tool fall back to its numeric / ASCII /
 * epoch path -- enough for the bind to resolve and the tool to run.
 */
int	__mb_cur_max = 1;		/* C locale: single-byte encoding */

int
isatty(int fd)
{

	(void)fd;
	return (0);			/* not a tty -> no colour, default width */
}

char *
setlocale(int category, const char *locale)
{

	(void)category;
	(void)locale;
	return ((char *)"C");
}

char *
nl_langinfo(int item)
{

	/* CODESET (item 0): a non-UTF-8 name, so tools pick ASCII line-drawing. */
	return ((char *)(item == 0 ? "US-ASCII" : ""));
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

/* time: no RTC wired to ring 3 -> a fixed epoch. */
long
time(long *t)
{

	if (t != NULL)
		*t = 0;
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

/*
 * errno location.  Apple's <errno.h> defines errno as (*__error()); a handful
 * of guname's gnulib paths read it.  One shared cell is enough -- we set it
 * nowhere, so it stays 0 (success), which is all the success path needs.
 */
static int	g_errno;

int *
__error(void)
{

	return (&g_errno);
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

/* No mutable fd flags on the read-only FS: pretend every request succeeds. */
int
fcntl(int fd, int cmd, ...)
{

	(void)fd;
	(void)cmd;
	return (0);
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
 * getprogname(): the program's short name.  We run no libc startup to capture
 * argv[0] (our dyld jumps straight to LC_MAIN), so we return a fixed name.
 * guname reads this only for diagnostics, never on the path that prints the
 * uname line, so the value is cosmetic for the demo.
 */
const char *
getprogname(void)
{

	return ("darwin");
}

/* XSI strerror_r: we have no errno table, so report a single generic message. */
int
strerror_r(int errnum, char *buf, size_t buflen)
{
	const char	*m = "Unknown error";
	size_t		 i;

	(void)errnum;
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
