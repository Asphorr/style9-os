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
 * x86 port I/O primitives.
 *
 * outb / inb are unprivileged in ring 0; the assembler instructions are
 * one byte each but gcc cannot synthesise them, so the inline helpers
 * below are the canonical project-wide entry points.
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

#endif /* !_MACHINE_IO_H_ */
