/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_GDT_H_
#define	_MACHINE_GDT_H_

#include <stdint.h>

/*
 * x86_64 Global Descriptor Table.
 *
 * In long mode the CPU largely ignores segment base/limit for code and
 * data, but the descriptors still have to be present with sane DPL/L
 * bits.  We keep the same layout as the boot stub for stability:
 *	idx 0  null
 *	idx 1  kernel code64	(L=1, DPL=0)
 *	idx 2  kernel data	(DPL=0)
 *	idx 3  user code64	(L=1, DPL=3)	reserved for later
 *	idx 4  user data	(DPL=3)		reserved for later
 *
 * TSS (16-byte descriptor in long mode) is added once a scheduler
 * needs RSP0 / IST stacks.
 */

#define	GDT_NULL		0x00
#define	GDT_KCODE		0x08
#define	GDT_KDATA		0x10
#define	GDT_UCODE		0x18
#define	GDT_UDATA		0x20

void	gdt_init(void);

#endif /* !_MACHINE_GDT_H_ */
