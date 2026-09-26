/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_IO_H_
#define	_MACHINE_IO_H_

#include <stdint.h>

/*
 * x86 port I/O primitives.  The compiler never emits in/out itself, so
 * these inlines are the project-wide entry points.
 */

static inline void
outb(uint16_t port, uint8_t val)
{

	__asm__ __volatile__ ("outb %0, %1"
	    :
	    : "a"(val), "Nd"(port));
}

static inline uint8_t
inb(uint16_t port)
{
	uint8_t	val;

	__asm__ __volatile__ ("inb %1, %0"
	    : "=a"(val)
	    : "Nd"(port));
	return (val);
}

static inline void
io_wait(void)
{

	/* Write to an unused port; gives a ~1us settle to the legacy bus. */
	outb(0x80, 0);
}

/*
 * 16-bit port I/O, e.g. the ATA PIO data port.  The block helpers use
 * `rep insw' / `rep outsw', so a whole sector (256 words) moves in one
 * instruction.
 */
static inline void
outw(uint16_t port, uint16_t val)
{

	__asm__ __volatile__ ("outw %0, %1"
	    :
	    : "a"(val), "Nd"(port));
}

static inline uint16_t
inw(uint16_t port)
{
	uint16_t	val;

	__asm__ __volatile__ ("inw %1, %0"
	    : "=a"(val)
	    : "Nd"(port));
	return (val);
}

static inline void
insw(uint16_t port, void *buf, uint32_t count)
{

	__asm__ __volatile__ ("rep insw"
	    : "+D"(buf), "+c"(count)
	    : "d"(port)
	    : "memory");
}

static inline void
outsw(uint16_t port, const void *buf, uint32_t count)
{

	__asm__ __volatile__ ("rep outsw"
	    : "+S"(buf), "+c"(count)
	    : "d"(port));
}

#endif /* !_MACHINE_IO_H_ */
