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

static void
sb_putu(struct sbuf *sb, unsigned long long v, unsigned int base, int upper)
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
	while (n > 0)
		sb_putc(sb, tmp[--n]);
}

static void
sb_putd(struct sbuf *sb, long long v)
{

	if (v < 0) {
		sb_putc(sb, '-');
		v = -v;
	}
	sb_putu(sb, (unsigned long long)v, 10u, 0);
}

int
printf(const char *fmt, ...)
{
	struct sbuf	sb;
	char		buf[256];
	va_list		ap;
	const char	*s;
	int		longs;
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
		longs = 0;
		while (fmt[i] == 'l') {
			longs++;
			i++;
		}

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
				sb_putd(&sb, va_arg(ap, long long));
			else if (longs == 1)
				sb_putd(&sb, (long long)va_arg(ap, long));
			else
				sb_putd(&sb, (long long)va_arg(ap, int));
			break;
		case 'u':
			if (longs >= 2)
				sb_putu(&sb,
				    va_arg(ap, unsigned long long), 10u, 0);
			else if (longs == 1)
				sb_putu(&sb,
				    (unsigned long long)va_arg(ap, unsigned long),
				    10u, 0);
			else
				sb_putu(&sb,
				    (unsigned long long)va_arg(ap, unsigned int),
				    10u, 0);
			break;
		case 'x':
			if (longs >= 2)
				sb_putu(&sb,
				    va_arg(ap, unsigned long long), 16u, 0);
			else if (longs == 1)
				sb_putu(&sb,
				    (unsigned long long)va_arg(ap, unsigned long),
				    16u, 0);
			else
				sb_putu(&sb,
				    (unsigned long long)va_arg(ap, unsigned int),
				    16u, 0);
			break;
		case 'X':
			if (longs >= 2)
				sb_putu(&sb,
				    va_arg(ap, unsigned long long), 16u, 1);
			else if (longs == 1)
				sb_putu(&sb,
				    (unsigned long long)va_arg(ap, unsigned long),
				    16u, 1);
			else
				sb_putu(&sb,
				    (unsigned long long)va_arg(ap, unsigned int),
				    16u, 1);
			break;
		case 'p':
			sb_putc(&sb, '0');
			sb_putc(&sb, 'x');
			sb_putu(&sb,
			    (unsigned long long)(uintptr_t)va_arg(ap, void *),
			    16u, 0);
			break;
		default:
			/* unknown conversion -- emit literally */
			sb_putc(&sb, '%');
			sb_putc(&sb, fmt[i]);
			break;
		}
	}

done:
	va_end(ap);
	sb_flush(&sb);
	return (sb.sb_emitted);
}
