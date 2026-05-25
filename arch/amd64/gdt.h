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
 * Layout is dictated by the SYSCALL / SYSRET MSR conventions:
 *
 *	idx 0  0x00  null
 *	idx 1  0x08  kernel code64   (L=1, DPL=0)	SYSCALL CS  = STAR[47:32]+0
 *	idx 2  0x10  kernel data     (DPL=0)		SYSCALL SS  = STAR[47:32]+8
 *	idx 3  0x18  user code32     (DPL=3 compat)	SYSRET  CS  = STAR[63:48]+0 (32-bit)
 *	idx 4  0x20  user data       (DPL=3)		SYSRET  SS  = STAR[63:48]+8
 *	idx 5  0x28  user code64     (L=1, DPL=3)	SYSRET  CS  = STAR[63:48]+16 (REX.W)
 *	idx 6  0x30  TSS low half (16-byte sys desc occupies idx 6 and 7)
 *
 * STAR is therefore programmed with [47:32]=0x08 and [63:48]=0x18.
 *
 * The TSS exists so a #PF/#GP/IRQ delivered while we are in ring 3
 * lands on the per-task RSP0 the scheduler installs there, rather
 * than continuing on the user stack.
 */

#define	GDT_NULL		0x00
#define	GDT_KCODE		0x08
#define	GDT_KDATA		0x10
#define	GDT_UCODE32		0x18
#define	GDT_UDATA		0x20
#define	GDT_UCODE		0x28
#define	GDT_TSS			0x30

#define	GDT_RPL3		0x03	/* OR'd into user selectors at use */

void	gdt_init(void);

/*
 * Install `rsp` in the TSS as the ring-0 stack to use on the next
 * ring-3 -> ring-0 transition.  The scheduler calls this on every
 * context switch into a user-mode thread so exceptions land on that
 * thread's kernel stack rather than a stale one.
 */
void	tss_set_rsp0(uint64_t rsp);

#endif /* !_MACHINE_GDT_H_ */
