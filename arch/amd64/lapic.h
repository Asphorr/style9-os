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
 * The local APIC: each CPU's own interrupt controller.
 *
 * The PIT and the 8259 are one chip each for the whole machine, so the
 * PIT can debit only one CPU's slice.  Every CPU needs a timer of its own
 * and a way to be interrupted by another CPU (reschedule, TLB shootdown,
 * startup); both live here.
 *
 * Enabling it can cut off every legacy interrupt.  The 8259's output
 * reaches the CPU through the APIC's LINT0 pin, only if that LVT entry is
 * unmasked ExtINT, and every LVT entry is masked out of reset.  Whatever
 * "virtual wire" setup firmware left is not relied on: lapic_init programs
 * the legacy pins together with the enable.
 *
 * xAPIC (MMIO) only.  A machine that hands over an APIC already in x2APIC
 * mode would fault on these reads, so lapic_init checks and refuses.
 */

/*
 * Vectors above the 8259's 32..47 window.  Spurious is 0xFF because older
 * APICs hardwire the low four bits of the spurious vector to one.
 */
#define	LAPIC_VEC_TIMER		0xF0
#define	LAPIC_VEC_SPURIOUS	0xFF

/*
 * Find this CPU's APIC, map its registers uncacheable, software-enable it
 * and program the legacy pins: LINT1 = NMI everywhere, LINT0 = ExtINT on
 * the boot CPU (so the 8259 keeps reaching it) and masked on APs.  Records
 * the hardware APIC id in the per-CPU block.  Needs pmap; call with
 * interrupts off, since it rewires the interrupt path.
 *
 * Returns false and leaves the APIC alone if there is none or it is one
 * this code will not drive.  The kernel then boots on the 8259 alone and
 * cannot start a second CPU.
 */
bool		lapic_init(void);

/*
 * Measure the APIC timer's counting rate against the TSC (not the PIT,
 * whose interrupts arrive in bursts), then run it briefly and count what
 * is delivered.  Needs the TSC calibrated and interrupts on.  Leaves the
 * timer masked and spends nobody's slice; lapic_timer_start starts it.
 */
void		lapic_timer_probe(void);

/*
 * Hand the slice debit from the PIT to this CPU's timer, run periodic at
 * the PIT's rate so PREEMPT_QUANTUM_TICKS keeps its meaning.  After
 * lapic_timer_probe, which supplies the counting rate.
 *
 * Returns false, leaving preemption with the PIT, if there is no APIC,
 * no measured rate, or no PIT rate to match.
 */
bool		lapic_timer_start(void);

/*
 * The same timer on an AP, at the rate the boot CPU measured; only the
 * arming half, as the PIT never debited an AP's slice.  Returns false if
 * the boot CPU never made the hand-over, in which case no AP is
 * preempted at all.
 */
bool		lapic_timer_start_ap(void);

/* Whether the APIC timers, not the PIT, debit the slice. */
bool		lapic_timer_preempting(void);

/*
 * Print, one line per CPU, what each timer has delivered since the
 * hand-over: its rate over TSC time and the slice that rate implies --
 * PREEMPT_QUANTUM_TICKS is 20 ms only while ticks arrive at the rate
 * asked.  The PIT's count over the same span is printed beside them.
 */
void		lapic_timer_report(void);

/*
 * End-of-interrupt, written by intr_dispatch after the handler of an
 * APIC-delivered vector.  Never for the spurious vector: it has no
 * in-service bit, and an EOI would retire somebody else's interrupt.
 */
void		lapic_eoi(void);

bool		lapic_present(void);
uint32_t	lapic_id(void);

/*
 * Physical address of the register page per IA32_APIC_BASE, or zero if
 * never mapped; the ACPI probe checks the MADT against it.
 */
uint64_t	lapic_base_pa(void);

/*
 * The two messages that start a processor, by APIC id.  INIT puts it in a
 * known state; STARTUP gives the start address as a page number, which is
 * why the trampoline lives in the first megabyte.
 *
 * Both return false only if the APIC would not take the message; whether
 * the far CPU acted on it is known only when it says so (cp_online).
 */
bool		lapic_ipi_init(uint32_t apic_id);
bool		lapic_ipi_startup(uint32_t apic_id, uint64_t tramp_pa);

/*
 * Raise `vec' on the processor with this APIC id, as an ordinary interrupt
 * through its IDT.  Returns false only if the APIC would not take the
 * message; a caller that needs to know it was acted on needs its own
 * acknowledgement.
 *
 * The sender must not wait for a reply while holding anything the receiver
 * could want, unless the wait services incoming requests: every spinlock
 * turns interrupts off, and a CPU with interrupts off cannot answer.
 * pmap_tlb_poll is that service; kern/spinlock.c calls it while spinning.
 */
bool		lapic_ipi_vector(uint32_t apic_id, uint8_t vec);

/*
 * APIC timer counting rate in ticks per second at the probe's divisor,
 * as measured on the boot CPU.  Zero until lapic_timer_probe has run.
 */
uint32_t	lapic_timer_hz(void);

#endif /* !_MACHINE_LAPIC_H_ */
