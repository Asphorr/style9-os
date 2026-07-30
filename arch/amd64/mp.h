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

/*
 * Let the started processors into the scheduler.
 *
 * SEPARATE FROM STARTING THEM, and the gap is deliberate.  A processor has to
 * be started EARLY: the trampoline needs a page of conventional memory that
 * later boot-time allocation would be entitled to take, and the messages that
 * start it need the APIC, which comes up long before the rest of the machine.
 * But it must not RUN anything until the per-CPU state it will inherit is
 * settled -- CR4.SMAP is turned on near the end of boot and the SYSCALL
 * registers with it, and a processor that missed either fails silently and
 * somewhere else (see cpu_state_init).
 *
 * So they arrive, park answering invalidations, and are released here.
 * Returns how many are now in the scheduler.
 */
unsigned int	mp_release_aps(void);

/*
 * Ask another processor to look at the runqueue.  Used by the scheduler when
 * it queues a thread and finds a CPU sitting in idle's hlt -- that CPU would
 * otherwise not notice until its own timer woke it a whole tick later, while
 * the queue grew in front of a busy one.
 *
 * Sends and returns.  Whether the far processor took the hint is its business
 * and costs nothing if it did not: the message only sets a flag it was going
 * to look at anyway.
 */
struct cpu;
void		mp_resched(struct cpu *cp);

/*
 * Ask every other processor where it is, and have each one answer for itself
 * at the UART: ring and instruction pointer, out of the trapframe the
 * question lands in.  Nothing else can answer it -- a program counter is not
 * readable from another CPU, and every other instrument here reports the
 * scheduler's opinion of a processor rather than the processor.
 *
 * For a machine that is busy and getting nothing done.  Takes no lock and
 * touches no console, since that is the state it exists for.
 */
void		mp_where_all(void);

/* The same answer for the CALLING processor, out of the frame it is in. */
struct trapframe;
void		mp_where(struct trapframe *tf);

#endif /* !__ASSEMBLER__ */

#endif /* !_MACHINE_MP_H_ */
