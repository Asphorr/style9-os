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
 * comes out of reset MASKED, and this kernel software-enables the APIC
 * itself -- so it is this kernel's business to say what LINT0 carries.
 * Whether the firmware had already set "virtual wire mode" up is not known
 * here and deliberately not relied on: lapic_init programs the two legacy
 * pins in the same breath as the enable, which makes the question moot.  The
 * failure mode if that is wrong is loud rather than subtle -- the boot stops
 * dead at the first thing that waits for an interrupt -- which is what made
 * it safe to write and try.
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
 * Measure the APIC timer's counting rate against the TSC, then prove the
 * timer actually delivers by running it briefly and counting what arrives.
 * Needs the TSC calibrated and interrupts ON, so it runs well after
 * lapic_init.  Leaves the timer masked and spends nobody's slice while it
 * runs; starting it for real is the separate call below.
 *
 * ⚠ The ruler here used to be the PIT and that was WRONG, in a way only a
 * measurement caught: ten delivered PIT interrupts can arrive in ninety
 * milliseconds on this host, and a window that short comes out ten percent
 * fast, which lands directly in the timer's reload count and shortens every
 * slice by the same ten percent.  A counter the CPU reads cannot be hurried;
 * an interrupt that has to be delivered can.
 */
void		lapic_timer_probe(void);

/*
 * Hand preemption over from the PIT to this CPU's own timer: relieve the PIT
 * of the slice debit, then run this timer periodic at the rate the PIT was
 * keeping, so that PREEMPT_QUANTUM_TICKS still means the milliseconds it was
 * measured to mean.  Must run after lapic_timer_probe, which is what supplies
 * the counting rate.
 *
 * Returns false and leaves preemption with the PIT if there is no APIC, no
 * measured rate, or no PIT rate to match -- a kernel where this fails is a
 * kernel that preempts exactly as it did before.
 */
bool		lapic_timer_start(void);

/*
 * Whether the slice is being debited by this CPU's timer rather than by the
 * one PIT the machine has.
 */
bool		lapic_timer_preempting(void);

/*
 * Print what the timer has actually delivered since it took over: its rate
 * over TSC time, and the slice length that rate implies.  The slice is the
 * number that matters -- PREEMPT_QUANTUM_TICKS means 20 ms only while the
 * ticks arrive at the rate they were asked for.  The PIT's count over the same
 * span is printed beside it, where its delivery deficit shows.
 */
void		lapic_timer_report(void);

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
 * Physical address of the register page, as the MSR gave it, or zero if the
 * APIC was never mapped.  Exists so the ACPI probe can compare firmware's
 * description of this chip against the chip's own answer.
 */
uint64_t	lapic_base_pa(void);

/*
 * The two messages that start a processor, sent to one destination named by
 * its APIC id.  INIT puts it in a known state; STARTUP tells it where to begin
 * executing, as a page number -- which is why the trampoline has to live in
 * the first megabyte.
 *
 * Both return false if the message could not be handed to the APIC, and
 * NEITHER says anything about whether the far processor did something with it.
 * That is not knowable from here: the only evidence a start worked is the
 * started processor saying so, which is what cp_online is for.
 */
bool		lapic_ipi_init(uint32_t apic_id);
bool		lapic_ipi_startup(uint32_t apic_id, uint64_t tramp_pa);

/*
 * Counting rate of this CPU's APIC timer, in ticks per second, at the
 * divisor lapic_timer_probe measured with.  Zero until it has run.
 */
uint32_t	lapic_timer_hz(void);

#endif /* !_MACHINE_LAPIC_H_ */
