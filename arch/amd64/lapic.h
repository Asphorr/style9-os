/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_LAPIC_H_
#define	_MACHINE_LAPIC_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * The local APIC: the interrupt controller each CPU has of its own.
 *
 * WHY IT IS NEEDED HERE and not merely nice to have.  The 8259 and the PIT
 * are ONE chip each for the whole machine: the PIT can tick one interrupt
 * line, so it can debit one CPU's slice, and the per-CPU quantum the
 * scheduler now keeps would be a quantum only the boot CPU ever spends.
 * Every processor needs a timer of its own, and needs a way to be told
 * something by another processor -- a reschedule, a TLB invalidation, the
 * startup sequence itself.  Both live in this chip.
 *
 * ⚠ ENABLING IT CAN KILL EVERY LEGACY INTERRUPT, and that is the whole
 * difficulty of this step.  On a PC the 8259's output does not reach the CPU
 * directly; it arrives at the local APIC's LINT0 pin, and passes through
 * only if that pin's LVT entry says ExtINT and is unmasked.  Every LVT entry
 * comes out of reset MASKED.  A firmware boot has the BIOS set "virtual wire
 * mode" up before the kernel ever looks, but we are loaded by QEMU with no
 * BIOS in the path, so nobody has done it -- software-enabling the APIC
 * without programming LINT0 would silently disconnect the timer, the
 * keyboard and the disk all at once.  lapic_init therefore programs the two
 * legacy pins in the same breath as the enable, and the failure mode if it
 * gets that wrong is loud: the boot stops dead at the first thing that waits
 * for an interrupt.
 *
 * Registers are memory-mapped (xAPIC).  x2APIC would reach the same
 * registers through MSRs and is deliberately not used: MMIO works on
 * everything, and the day a machine hands us an APIC already in x2APIC mode
 * these reads would fault, so lapic_init checks for that and says so rather
 * than finding out by exception.
 */

/*
 * Vectors above the 8259's 32..47 window.  0xFF for spurious because the
 * low four bits of the spurious vector are hardwired to one on some older
 * implementations, so a vector ending in 0xF is the only portable choice.
 */
#define	LAPIC_VEC_TIMER		0xF0
#define	LAPIC_VEC_SPURIOUS	0xFF

/*
 * Find the APIC, map its register page uncacheable, software-enable it, and
 * connect the two legacy pins (LINT0 = ExtINT, LINT1 = NMI) so the 8259 and
 * the NMI line keep reaching this CPU.  Records this CPU's hardware APIC id
 * in its per-CPU block.  Needs pmap up; safe to call with interrupts off,
 * and must be, since it is the interrupt path being rewired.
 *
 * Returns false and leaves the APIC alone if the machine has none, or has
 * one this code will not drive.  A kernel with no APIC still boots -- the
 * 8259 is untouched -- it just cannot ever start a second processor.
 */
bool		lapic_init(void);

/*
 * Measure the APIC timer's counting rate against the PIT, then prove the
 * timer actually delivers by running it briefly and counting what arrives.
 * Needs the PIT ticking and interrupts ON, so it runs well after
 * lapic_init.  Leaves the timer masked: which clock drives preemption is a
 * separate decision, taken in a later rung and measured there.
 */
void		lapic_timer_probe(void);

/*
 * End-of-interrupt.  Written by intr_dispatch after any handler for a
 * vector the APIC delivered.  NOT written for the spurious vector, which by
 * architecture has no in-service bit to clear -- acknowledging it would
 * retire somebody else's interrupt.
 */
void		lapic_eoi(void);

bool		lapic_present(void);
uint32_t	lapic_id(void);

/*
 * Counting rate of this CPU's APIC timer, in ticks per second, at the
 * divisor lapic_timer_probe measured with.  Zero until it has run.
 */
uint32_t	lapic_timer_hz(void);

#endif /* !_MACHINE_LAPIC_H_ */
