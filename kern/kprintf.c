/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "tty.h"

static int	emit_str(const char *, int width, bool left_align);
static int	emit_uint(uint64_t, unsigned int base, bool upper,
		    int width, bool zeropad, bool left_align);
static int	emit_int(int64_t, int width, bool zeropad, bool left_align);

int
kprintf(const char *fmt, ...)
{
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = kvprintf(fmt, ap);
	va_end(ap);

	return (n);
}

int
kvprintf(const char *fmt, va_list ap)
{
	char	ch;
	int	written;
	int	width;
	bool	zeropad;
	bool	left_align;
	int	length;		/* 0 = int, 1 = long, 2 = long long */

	written = 0;

	while ((ch = *fmt++) != '\0') {
		if (ch != '%') {
			tty_putc(ch);
			written++;
			continue;
		}

		zeropad = false;
		left_align = false;
		width = 0;
		length = 0;

		/* Flags: '-' (left align) and '0' (zero pad), any order. */
		for (;;) {
			if (*fmt == '-') {
				left_align = true;
				fmt++;
			} else if (*fmt == '0') {
				zeropad = true;
				fmt++;
			} else {
				break;
			}
		}
		while (*fmt >= '0' && *fmt <= '9') {
			width = width * 10 + (*fmt - '0');
			fmt++;
		}
		if (*fmt == 'l') {
			length = 1;
			fmt++;
			if (*fmt == 'l') {
				length = 2;
				fmt++;
			}
		}
		if (*fmt == 'z') {
			length = 1;	/* size_t on LP64 is 8 bytes */
			fmt++;
		}

		switch (*fmt++) {
		case 'c': {
			char	cbuf[2];

			cbuf[0] = (char)va_arg(ap, int);
			cbuf[1] = '\0';
			written += emit_str(cbuf, width, left_align);
			break;
		}
		case 's':
			written += emit_str(va_arg(ap, const char *),
			    width, left_align);
			break;
		case 'd':
		case 'i':
			if (length >= 1)
				written += emit_int(va_arg(ap, long),
				    width, zeropad, left_align);
			else
				written += emit_int(va_arg(ap, int),
				    width, zeropad, left_align);
			break;
		case 'u':
			if (length >= 1)
				written += emit_uint(va_arg(ap, unsigned long),
				    10, false, width, zeropad, left_align);
			else
				written += emit_uint(
				    (uint64_t)va_arg(ap, unsigned int),
				    10, false, width, zeropad, left_align);
			break;
		case 'x':
			if (length >= 1)
				written += emit_uint(va_arg(ap, unsigned long),
				    16, false, width, zeropad, left_align);
			else
				written += emit_uint(
				    (uint64_t)va_arg(ap, unsigned int),
				    16, false, width, zeropad, left_align);
			break;
		case 'X':
			if (length >= 1)
				written += emit_uint(va_arg(ap, unsigned long),
				    16, true, width, zeropad, left_align);
			else
				written += emit_uint(
				    (uint64_t)va_arg(ap, unsigned int),
				    16, true, width, zeropad, left_align);
			break;
		case 'p':
			written += emit_str("0x", 0, false);
			written += emit_uint(
			    (uint64_t)(uintptr_t)va_arg(ap, void *),
			    16, false, 16, true, false);
			break;
		case '%':
			tty_putc('%');
			written++;
			break;
		default:
			tty_putc('%');
			tty_putc(*(fmt - 1));
			written += 2;
			break;
		}
	}

	return (written);
}

static int
emit_str(const char *s, int width, bool left_align)
{
	const char	*p;
	int		 len, n;

	if (s == NULL)
		s = "(null)";

	len = 0;
	for (p = s; *p != '\0'; p++)
		len++;

	n = 0;
	if (!left_align) {
		while (width > len) {
			tty_putc(' ');
			n++;
			width--;
		}
	}
	while (*s != '\0') {
		tty_putc(*s++);
		n++;
	}
	if (left_align) {
		while (width > len) {
			tty_putc(' ');
			n++;
			width--;
		}
	}
	return (n);
}

static int
emit_uint(uint64_t v, unsigned int base, bool upper, int width, bool zeropad,
    bool left_align)
{
	static const char	digits_lo[] = "0123456789abcdef";
	static const char	digits_hi[] = "0123456789ABCDEF";
	char			buf[32];
	const char		*digits;
	int			 i, n;
	char			 pad;

	digits = upper ? digits_hi : digits_lo;
	i = 0;

	if (v == 0)
		buf[i++] = '0';
	while (v != 0) {
		buf[i++] = digits[v % base];
		v /= base;
	}

	/* '0' flag is ignored when left-aligning (POSIX). */
	pad = (zeropad && !left_align) ? '0' : ' ';

	n = 0;
	if (!left_align) {
		while (i < width) {
			tty_putc(pad);
			n++;
			width--;
		}
	}

	while (i > 0) {
		tty_putc(buf[--i]);
		n++;
		width--;
	}

	if (left_align) {
		while (width > 0) {
			tty_putc(' ');
			n++;
			width--;
		}
	}
	return (n);
}

static int
emit_int(int64_t v, int width, bool zeropad, bool left_align)
{
	uint64_t	u;
	int		n;

	n = 0;
	if (v < 0) {
		tty_putc('-');
		n++;
		if (width > 0)
			width--;
		u = (uint64_t)(-(v + 1)) + 1;	/* INT64_MIN-safe */
	} else {
		u = (uint64_t)v;
	}

	n += emit_uint(u, 10, false, width, zeropad, left_align);
	return (n);
}
