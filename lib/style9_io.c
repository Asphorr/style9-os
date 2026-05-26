/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

#include <stdarg.h>

/*
 * I/O routines.
 *
 * Every output path ultimately funnels through write(), which is the
 * SYS_PRINT syscall.  putchar/puts/printf are thin wrappers; printf
 * formats into a 256-byte stack buffer one segment at a time so the
 * kernel only sees `write` calls (no need for a streaming putchar
 * fast path until profiling says otherwise).
 *
 * Supported printf conversions:
 *	%s	NUL-terminated string ("(null)" on NULL)
 *	%c	one character
 *	%d / %i	signed decimal
 *	%u	unsigned decimal
 *	%x	unsigned hex, lowercase
 *	%X	unsigned hex, uppercase
 *	%p	pointer (== %#x of (uintptr_t))
 *	%%	literal '%'
 *
 * Width/precision flags are not honoured beyond a leading '0' on hex
 * to zero-pad to the natural width of the cast.  Long modifier `l` is
 * accepted (%ld / %lu / %lx) and read as long; `ll` is also accepted
 * for long long.  No floats -- we have no FPU saved on syscall yet.
 */

ssize_t
write(const char *buf, size_t len)
{

	return ((ssize_t)syscall2(SYS_PRINT, (long)buf, (long)len));
}

void
putchar(char c)
{
	char	tmp;

	tmp = c;
	(void)write(&tmp, 1);
}

void
puts(const char *s)
{

	(void)write(s, strlen(s));
}

/* ---- printf internals ---------------------------------------------- */

struct sbuf {
	char		*sb_buf;
	size_t		 sb_len;
	size_t		 sb_cap;
	int		 sb_emitted;
};

static void
sb_flush(struct sbuf *sb)
{

	if (sb->sb_len == 0)
		return;
	(void)write(sb->sb_buf, sb->sb_len);
	sb->sb_emitted += (int)sb->sb_len;
	sb->sb_len = 0;
}

static void
sb_putc(struct sbuf *sb, char c)
{

	if (sb->sb_len == sb->sb_cap)
		sb_flush(sb);
	sb->sb_buf[sb->sb_len++] = c;
}

static void
sb_puts(struct sbuf *sb, const char *s)
{
	size_t	i;

	for (i = 0; s[i] != '\0'; i++)
		sb_putc(sb, s[i]);
}

/*
 * Numeric emit with optional field width.  Build the digit string into
 * a 24-byte stack buffer (max length of any 64-bit value in any base
 * >= 2), then emit (width - digits) pad chars followed by the digits.
 * width <= digit count is a no-op pad-wise; the digits still emit.
 */
static void
sb_putu(struct sbuf *sb, unsigned long long v, unsigned int base, int upper,
    unsigned width, char pad)
{
	char	tmp[24];
	size_t	n;
	int	digit;

	n = 0;
	if (v == 0) {
		tmp[n++] = '0';
	} else {
		while (v != 0) {
			digit = (int)(v % base);
			tmp[n++] = (char)(digit < 10
			    ? '0' + digit
			    : (upper ? 'A' : 'a') + (digit - 10));
			v /= base;
		}
	}
	while (n < (size_t)width) {
		sb_putc(sb, pad);
		width--;
	}
	while (n > 0)
		sb_putc(sb, tmp[--n]);
}

/*
 * Signed counterpart: emit '-' before the field for negatives and let
 * sb_putu handle the rest, with the width reduced by one so the sign
 * is counted as part of the field.  Matches glibc behaviour for
 * %05d/-42 == "-0042"; for unsigned %5d we left-pad with spaces.
 */
static void
sb_putd(struct sbuf *sb, long long v, unsigned width, char pad)
{
	unsigned long long	uv;

	if (v < 0) {
		sb_putc(sb, '-');
		if (width > 0)
			width--;
		uv = (unsigned long long)(-v);
	} else {
		uv = (unsigned long long)v;
	}
	sb_putu(sb, uv, 10u, 0, width, pad);
}

int
printf(const char *fmt, ...)
{
	struct sbuf	sb;
	char		buf[256];
	va_list		ap;
	const char	*s;
	int		longs;
	unsigned	width;
	int		zero_pad;
	size_t		i;

	sb.sb_buf     = buf;
	sb.sb_len     = 0;
	sb.sb_cap     = sizeof(buf);
	sb.sb_emitted = 0;

	va_start(ap, fmt);

	for (i = 0; fmt[i] != '\0'; i++) {
		if (fmt[i] != '%') {
			sb_putc(&sb, fmt[i]);
			continue;
		}

		i++;

		/*
		 * Optional flag '0' means pad numeric conversions with '0'
		 * instead of spaces.  We don't honour '-', '+', ' ', '#' yet;
		 * skip them silently so a format that uses them still does
		 * the right thing for the conversion letter at least.
		 */
		zero_pad = 0;
		while (fmt[i] == '0' || fmt[i] == '-' ||
		    fmt[i] == '+' || fmt[i] == ' ' || fmt[i] == '#') {
			if (fmt[i] == '0')
				zero_pad = 1;
			i++;
		}

		/* Optional decimal width. */
		width = 0;
		while (fmt[i] >= '0' && fmt[i] <= '9') {
			width = width * 10u + (unsigned)(fmt[i] - '0');
			i++;
		}

		longs = 0;
		while (fmt[i] == 'l') {
			longs++;
			i++;
		}

		{
			char pad = zero_pad ? '0' : ' ';

			switch (fmt[i]) {
			case '\0':
				/* trailing '%' with no conversion -- emit it raw */
				sb_putc(&sb, '%');
				goto done;
			case '%':
				sb_putc(&sb, '%');
				break;
			case 'c':
				sb_putc(&sb, (char)va_arg(ap, int));
				break;
			case 's':
				s = va_arg(ap, const char *);
				sb_puts(&sb, s != NULL ? s : "(null)");
				break;
			case 'd':
			case 'i':
				if (longs >= 2)
					sb_putd(&sb, va_arg(ap, long long),
					    width, pad);
				else if (longs == 1)
					sb_putd(&sb, (long long)va_arg(ap, long),
					    width, pad);
				else
					sb_putd(&sb, (long long)va_arg(ap, int),
					    width, pad);
				break;
			case 'u':
				if (longs >= 2)
					sb_putu(&sb,
					    va_arg(ap, unsigned long long),
					    10u, 0, width, pad);
				else if (longs == 1)
					sb_putu(&sb,
					    (unsigned long long)va_arg(ap, unsigned long),
					    10u, 0, width, pad);
				else
					sb_putu(&sb,
					    (unsigned long long)va_arg(ap, unsigned int),
					    10u, 0, width, pad);
				break;
			case 'x':
				if (longs >= 2)
					sb_putu(&sb,
					    va_arg(ap, unsigned long long),
					    16u, 0, width, pad);
				else if (longs == 1)
					sb_putu(&sb,
					    (unsigned long long)va_arg(ap, unsigned long),
					    16u, 0, width, pad);
				else
					sb_putu(&sb,
					    (unsigned long long)va_arg(ap, unsigned int),
					    16u, 0, width, pad);
				break;
			case 'X':
				if (longs >= 2)
					sb_putu(&sb,
					    va_arg(ap, unsigned long long),
					    16u, 1, width, pad);
				else if (longs == 1)
					sb_putu(&sb,
					    (unsigned long long)va_arg(ap, unsigned long),
					    16u, 1, width, pad);
				else
					sb_putu(&sb,
					    (unsigned long long)va_arg(ap, unsigned int),
					    16u, 1, width, pad);
				break;
			case 'p':
				sb_putc(&sb, '0');
				sb_putc(&sb, 'x');
				sb_putu(&sb,
				    (unsigned long long)(uintptr_t)va_arg(ap, void *),
				    16u, 0, 0u, ' ');
				break;
			default:
				/* unknown conversion -- emit literally */
				sb_putc(&sb, '%');
				sb_putc(&sb, fmt[i]);
				break;
			}
		}

	}

done:
	va_end(ap);
	sb_flush(&sb);
	return (sb.sb_emitted);
}
