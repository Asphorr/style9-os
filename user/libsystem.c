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

void
exit(int code)
{
	dsys(0x2000001, code, 0, 0);				/* BSD exit  */
	for (;;)
		;
}

int
getpid(void)
{
	return ((int)dsys(0x2000014, 0, 0, 0));			/* BSD getpid */
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
 * arena starts zeroed).  free() is a no-op: figlet allocates monotonically and
 * never depends on reclamation.  4 MiB covers a font plus working buffers with
 * room to spare; an exhausted arena returns NULL exactly as a real malloc would.
 */
#define	ARENA_SIZE	(4u * 1024u * 1024u)

static unsigned char	arena[ARENA_SIZE];
static size_t		arena_off;

void *
malloc(size_t n)
{
	size_t	off;

	n = (n + 15u) & ~(size_t)15u;		/* 16-byte alignment */
	if (n == 0)
		n = 16;
	if (n > ARENA_SIZE || arena_off > ARENA_SIZE - n)
		return (NULL);
	off = arena_off;
	arena_off += n;
	return (&arena[off]);
}

void
free(void *p)
{

	(void)p;
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
 * figlet treats FILE as opaque (it only passes FILE* to the stdio calls), so
 * the layout is entirely ours.  `fd` routes fwrite/fprintf to write(2);
 * `mem`/`len`/`pos` would back a read-only in-memory file (e.g. an embedded
 * font) -- unused until a font is wired in, but the read path is already here.
 */
typedef struct __sFILE {
	int			 fd;
	int			 eof;
	const unsigned char	*mem;
	size_t			 len;
	size_t			 pos;
} FILE;

static FILE	__stdin_file  = { 0, 0, NULL, 0, 0 };
static FILE	__stdout_file = { 1, 0, NULL, 0, 0 };
static FILE	__stderr_file = { 2, 0, NULL, 0, 0 };

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

	return (EOF);				/* no stdin plumbed to ring 3 */
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
	size_t	want;
	size_t	avail;

	want = size * nmemb;
	if (fp == NULL || fp->mem == NULL || want == 0 || size == 0)
		return (0);
	avail = fp->len - fp->pos;
	if (want > avail)
		want = avail;
	(void)memcpy(ptr, fp->mem + fp->pos, want);
	fp->pos += want;
	if (fp->pos >= fp->len)
		fp->eof = 1;
	return (want / size);
}

FILE *
fopen(const char *path, const char *mode)
{

	(void)path;
	(void)mode;
	return (NULL);				/* no VFS in ring 3 yet */
}

int
fclose(FILE *fp)
{

	(void)fp;
	return (0);
}

int
fseek(FILE *fp, long off, int whence)
{

	if (fp == NULL || fp->mem == NULL)
		return (0);
	if (whence == 0)			/* SEEK_SET */
		fp->pos = (size_t)off;
	else if (whence == 1)			/* SEEK_CUR */
		fp->pos += (size_t)off;
	else					/* SEEK_END */
		fp->pos = fp->len + (size_t)off;
	fp->eof = 0;
	return (0);
}

long
ftell(FILE *fp)
{

	return (fp == NULL ? -1 : (long)fp->pos);
}

FILE *
tmpfile(void)
{

	return (NULL);				/* no VFS in ring 3 yet */
}

/* ---- formatted output (printf / fprintf) -------------------------------- */

/*
 * A tiny buffered emitter: format into `buf`, flushing to the fd when full, so
 * arbitrarily long output costs O(len) work and few write(2)s rather than one
 * syscall per byte.
 */
struct ob {
	int		fd;
	unsigned	n;
	char		buf[256];
};

static void
ob_flush(struct ob *o)
{

	if (o->n != 0) {
		write(o->fd, o->buf, o->n);
		o->n = 0;
	}
}

static void
ob_putc(struct ob *o, char c)
{

	if (o->n == sizeof(o->buf))
		ob_flush(o);
	o->buf[o->n++] = c;
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
vfmt(int fd, const char *fmt, __builtin_va_list ap)
{
	struct ob	o;
	const char	*p;

	o.fd = fd;
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
	ob_flush(&o);
	return (0);
}

int
printf(const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	__builtin_va_start(ap, fmt);
	r = vfmt(1, fmt, ap);
	__builtin_va_end(ap);
	return (r);
}

int
fprintf(FILE *fp, const char *fmt, ...)
{
	__builtin_va_list	ap;
	int			r;

	__builtin_va_start(ap, fmt);
	r = vfmt(fp != NULL ? fp->fd : 2, fmt, ap);
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

		if (*f == 'd' || *f == 'u' || *f == 'x') {
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

/* ---- stat (the $INODE64 variant) ---------------------------------------- */

/*
 * figlet stat()s candidate font/control files.  With no VFS, every path is
 * "absent": return -1.  The import is the inode-64 ABI variant `_stat$INODE64`,
 * a name no C identifier can spell, so alias an ordinary function onto it.
 */
int	stat_inode64(const char *path, void *buf) __asm__("_stat$INODE64");

int
stat_inode64(const char *path, void *buf)
{

	(void)path;
	(void)buf;
	return (-1);
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
