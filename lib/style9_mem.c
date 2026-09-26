/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include "style9.h"

/* ---- mem* helpers --------------------------------------------------- */

void *
memcpy(void *dst, const void *src, size_t n)
{
	uint8_t		*d;
	const uint8_t	*s;
	size_t		 i;

	d = (uint8_t *)dst;
	s = (const uint8_t *)src;
	for (i = 0; i < n; i++)
		d[i] = s[i];
	return (dst);
}

void *
memset(void *dst, int c, size_t n)
{
	uint8_t	*d;
	size_t	 i;

	d = (uint8_t *)dst;
	for (i = 0; i < n; i++)
		d[i] = (uint8_t)c;
	return (dst);
}

int
memcmp(const void *a, const void *b, size_t n)
{
	const uint8_t	*pa;
	const uint8_t	*pb;
	size_t		 i;

	pa = (const uint8_t *)a;
	pb = (const uint8_t *)b;
	for (i = 0; i < n; i++) {
		if (pa[i] != pb[i])
			return ((int)pa[i] - (int)pb[i]);
	}
	return (0);
}

/* ---- bump allocator -------------------------------------------------
 *
 * One .bss arena per program (4 KiB unless -DSTYLE9_HEAP_BYTES=N), served
 * linearly; free() is a no-op.  Enough for small programs whose memory
 * goes away with them at exit.
 */

#ifndef	STYLE9_HEAP_BYTES
#define	STYLE9_HEAP_BYTES	4096u
#endif

static uint8_t	heap_arena[STYLE9_HEAP_BYTES];
static size_t	heap_cursor;

void *
malloc(size_t n)
{
	size_t	aligned;
	void	*p;

	/*
	 * 16-byte steps, the SysV maximum alignment.  The arena itself is
	 * 16-aligned: SysV requires that of any global array of 16 bytes
	 * or more.
	 */
	aligned = (n + 15u) & ~(size_t)15u;
	if (heap_cursor + aligned > sizeof(heap_arena))
		return (NULL);

	p = &heap_arena[heap_cursor];
	heap_cursor += aligned;
	return (p);
}

void
free(void *p)
{

	(void)p;	/* bump allocator never reclaims */
}
