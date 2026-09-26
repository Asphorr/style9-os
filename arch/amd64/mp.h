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
 * An application processor comes out of reset in real mode at the address
 * the STARTUP IPI names, with no page tables, no stack and no idea which
 * CPU it is, and has to pass through three addressing modes before it can
 * call C.  Hence a trampoline in the first megabyte: the STARTUP IPI
 * carries a one-byte page number, so it cannot name anything above
 * 0xFF000.
 */

/*
 * Where the trampoline is assembled to run.  A constant, so every address
 * inside it (GDT base, far-jump targets) is a link-time constant and no
 * code is patched at run time; only the parameter block is written.
 *
 * pmm never hands out the first megabyte, and 0x8000 is conventional
 * memory the firmware is done with; mp_install_trampoline still checks
 * the memory map.
 */
#define	AP_TRAMP_PA		0x8000

/*
 * The parameter block, at a fixed offset in the same page so C and asm
 * can both name it.  Written by the starting CPU, read by the started
 * one; there is one block, so starts are serialised.
 */
#define	AP_PARAM_OFF		0xF00
#define	AP_PARAM_PA		(AP_TRAMP_PA + AP_PARAM_OFF)

#define	AP_P_CR3		0x00	/* page tables to start with     */
#define	AP_P_RSP		0x08	/* stack top, already 16-aligned */
#define	AP_P_CPU		0x10	/* struct cpu * for this one     */

/*
 * Selectors in the trampoline's own GDT.  code64 and data match the
 * kernel's (gdt.h), so loading the real GDT later changes nothing under
 * the running CS.  code32 serves only the stretch between protected and
 * long mode; in the kernel's table 0x18 is a ring-3 descriptor.
 */
#define	AP_SEL_CODE64		0x08
#define	AP_SEL_DATA		0x10
#define	AP_SEL_CODE32		0x18

#ifndef __ASSEMBLER__

#include <stdbool.h>
#include <stdint.h>

/*
 * Install the trampoline and start, one at a time, every processor the
 * MADT described, reporting each that arrives or does not.  Needs the
 * local APIC (INIT/STARTUP IPIs), pmm (a stack each) and the TSC (the
 * protocol's waits are in microseconds).  Returns how many checked in,
 * not counting the caller.
 */
unsigned int	mp_start_aps(void);

/*
 * Let the started, parked processors into the scheduler.  Separate from
 * starting them because they must not run anything until the per-CPU
 * state they inherit is settled: CR4.SMAP and the SYSCALL MSRs are set
 * near the end of kmain, and a CPU that missed either fails silently (see
 * cpu_state_init).  Returns how many are now in the scheduler.
 */
unsigned int	mp_release_aps(void);

/*
 * Ask another processor to look at the runqueue: the scheduler's nudge to
 * a CPU halted in idle, which would otherwise not notice new work until
 * its next tick.  Sends and returns; the IPI only sets need_resched there.
 */
struct cpu;
void		mp_resched(struct cpu *cp);

/*
 * Have every other processor report, straight at the UART, its ring and
 * RIP from the trapframe the IPI lands in -- the only way to see where a
 * CPU is that loops outside the scheduler.  No lock, no console.
 */
void		mp_where_all(void);

/* The same report for the calling processor, from the frame it is in. */
struct trapframe;
void		mp_where(struct trapframe *tf);

#endif /* !__ASSEMBLER__ */

#endif /* !_MACHINE_MP_H_ */
