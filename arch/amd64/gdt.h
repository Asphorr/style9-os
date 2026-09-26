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
 *	0x00  null
 *	0x08  kernel code (L=1, DPL=0)	SYSCALL CS = STAR[47:32]
 *	0x10  kernel data (DPL=0)	SYSCALL SS = STAR[47:32] + 8
 *	0x18  user code32 (DPL=3)	SYSRET CS = STAR[63:48] (32-bit)
 *	0x20  user data (DPL=3)		SYSRET SS = STAR[63:48] + 8
 *	0x28  user code64 (L=1, DPL=3)	SYSRET CS = STAR[63:48] + 16 (REX.W)
 *	0x30  TSS (a 16-byte system descriptor: slots 6 and 7)
 *
 * STAR is therefore programmed with [47:32]=0x08 and [63:48]=0x18.
 *
 * The TSS exists so a #PF/#GP/IRQ taken in ring 3 lands on the thread's
 * kernel stack, which the scheduler installs as RSP0, rather than on the
 * user stack.
 */

#define	GDT_NULL		0x00
#define	GDT_KCODE		0x08
#define	GDT_KDATA		0x10
#define	GDT_UCODE32		0x18
#define	GDT_UDATA		0x20
#define	GDT_UCODE		0x28
#define	GDT_TSS			0x30

#define	GDT_RPL3		0x03	/* OR'd into user selectors at use */

/*
 * Build and load the calling CPU's own GDT and TSS.  One per CPU because
 * the TSS cannot be shared: it carries the stack a ring transition lands
 * on, and two CPUs on one TSS would fault onto the same stack.  The flat
 * descriptors are duplicated too; one table per CPU is simpler than
 * sharing them.
 *
 * The CPU's identity comes from the per-CPU block, so the GS base must
 * already be installed: cpu_bsp_init (kmain's first statement) on the BSP,
 * the first line of ap_entry on an AP.
 */
void	gdt_init_cpu(void);

/*
 * Install `rsp' in this CPU's TSS as the stack for the next ring 3 -> 0
 * transition.  The scheduler calls this on every switch to a thread with a
 * kstack, and the user launchers before their first iretq.
 */
void	tss_set_rsp0(uint64_t rsp);

#endif /* !_MACHINE_GDT_H_ */
