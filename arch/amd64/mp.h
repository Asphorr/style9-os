/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_MP_H_
#define	_MACHINE_MP_H_

/*
 * Starting the other processors.
 *
 * An application processor comes out of reset in REAL MODE, at a physical
 * address the startup message names, with no page tables, no stack, no idea
 * which processor it is and a segment base of zero.  Everything this kernel
 * takes for granted has to be built for it in that order, by code that runs
 * in three different addressing modes before it can call a C function.  Hence
 * a trampoline: a page of position-known code in the first megabyte, because
 * the startup message carries a PAGE NUMBER in one byte and cannot name an
 * address above 0xFF000.
 */

/*
 * Where the trampoline is assembled to run.  A compile-time constant rather
 * than an allocation, and that is what keeps the assembly honest: every
 * address inside it -- the temporary GDT's base, the far-jump targets -- is a
 * link-time constant, so there is no runtime patching of code at all.  Only
 * the parameter block below is written, and it is data.
 *
 * 0x8000 is inside the conventional memory the firmware leaves alone once it
 * has finished booting, and pmm never offers it: the whole first megabyte is
 * marked used at startup as firmware playground.  The install checks the
 * memory map anyway, because "never" is a property of today's pmm.
 */
#define	AP_TRAMP_PA		0x8000

/*
 * The parameter block, at a fixed offset in the same page so that both C and
 * assembly can name it without either one knowing how long the other's code
 * is.  Written by the processor doing the starting, read by the one being
 * started, one processor at a time -- the block is shared, so the starts are
 * serialised, which they would be anyway: each AP is waited for before the
 * next is asked.
 */
#define	AP_PARAM_OFF		0xF00
#define	AP_PARAM_PA		(AP_TRAMP_PA + AP_PARAM_OFF)

#define	AP_P_CR3		0x00	/* page tables to start with     */
#define	AP_P_RSP		0x08	/* stack top, already 16-aligned */
#define	AP_P_CPU		0x10	/* struct cpu * for this one     */

/*
 * Selectors in the trampoline's own GDT.  The first two deliberately match the
 * kernel's (gdt.h): code64 at 0x08 and data at 0x10, so that the code segment
 * an AP is running with when it reaches C is already the selector the real GDT
 * uses for the same thing, and loading that GDT changes nothing under it.  The
 * 32-bit code segment exists only for the handful of instructions between
 * protected mode and long mode and has no counterpart in the kernel's table --
 * where 0x18 is a ring-3 descriptor that ring 0 could not jump to.
 */
#define	AP_SEL_CODE64		0x08
#define	AP_SEL_DATA		0x10
#define	AP_SEL_CODE32		0x18

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>

/*
 * Install the trampoline and start every processor the MADT described.  Each
 * one is asked, then waited for, then the next -- and each is reported, both
 * the ones that arrived and the ones that did not.  Needs the local APIC (the
 * startup sequence is two interrupts), the physical allocator (a stack per
 * processor) and the TSC (the sequence has real-time waits in it that are
 * measured in microseconds, not in loop iterations).
 *
 * Returns the number of processors that checked in, not counting the one
 * calling.
 */
unsigned int	mp_start_aps(void);

#endif /* !__ASSEMBLER__ */

#endif /* !_MACHINE_MP_H_ */
