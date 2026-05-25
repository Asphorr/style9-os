/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "dbgcon.h"
#include "io.h"

#define	DBGCON_PORT	0xE9

void
dbgcon_putc(char ch)
{

	outb(DBGCON_PORT, (uint8_t)ch);
}

void
dbgcon_write(const char *s, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		outb(DBGCON_PORT, (uint8_t)s[i]);
}
