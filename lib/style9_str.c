/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

size_t
strlen(const char *s)
{
	size_t	n;

	n = 0;
	while (s[n] != '\0')
		n++;
	return (n);
}

int
strcmp(const char *a, const char *b)
{
	size_t	i;

	for (i = 0; ; i++) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];
		if (ca != cb)
			return ((int)ca - (int)cb);
		if (ca == 0)
			return (0);
	}
}

int
strncmp(const char *a, const char *b, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++) {
		unsigned char ca = (unsigned char)a[i];
		unsigned char cb = (unsigned char)b[i];
		if (ca != cb)
			return ((int)ca - (int)cb);
		if (ca == 0)
			return (0);
	}
	return (0);
}

char *
strcpy(char *dst, const char *src)
{
	size_t	i;

	for (i = 0; src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
	return (dst);
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
