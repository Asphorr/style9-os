/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "intr.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "pit.h"
#include "sched.h"

/*
 * PIT command-register layout (port 0x43):
 *	bit 7:6  channel	00 = ch0
 *	bit 5:4  access mode	11 = lobyte/hibyte
 *	bit 3:1  operating mode	010 = rate generator
 *	bit 0	 bcd/binary	0 = binary
 *
 * Composed: 0x34.  Channel 0 data port is 0x40.
 */
#define	PIT_PORT_CH0		0x40
#define	PIT_PORT_CMD		0x43

#define	PIT_CMD_RATEGEN_BIN_CH0	0x34

/*
 * Lock key:
 *	(a) atomic; only ever bumped from IRQ context, read from anywhere
 *	(c) const after pit_init
 */
static volatile uint64_t	pit_tick_count;		/* (a) */
static unsigned int		pit_actual_hz;		/* (c) */

/*
 * True until a per-CPU timer takes the job over (pit_release_preempt), and
 * true forever on a machine that has no local APIC.  Written once, before
 * that timer is unmasked, so the ISR reading it cannot race the writer.
 */
static bool			pit_debits_slice = true;

static void	pit_isr(struct trapframe *tf);

void
pit_init(unsigned int hz)
{
	uint32_t	divisor;

	if (hz == 0 || hz > PIT_INPUT_HZ)
		panic("pit_init: invalid hz %u", hz);

	divisor = PIT_INPUT_HZ / hz;
	if (divisor == 0 || divisor > 0xFFFF)
		panic("pit_init: divisor %u out of range for hz=%u",
		    divisor, hz);

	/* Recover the exact rate after integer division. */
	pit_actual_hz = PIT_INPUT_HZ / divisor;

	outb(PIT_PORT_CMD, PIT_CMD_RATEGEN_BIN_CH0);
	outb(PIT_PORT_CH0, (uint8_t)(divisor & 0xFF));
	outb(PIT_PORT_CH0, (uint8_t)((divisor >> 8) & 0xFF));

	irq_install(0, pit_isr);
	pic_unmask(0);

	kprintf("pit: %u Hz requested, %u Hz programmed "
	    "(divisor=%u)\n", hz, pit_actual_hz, divisor);
}

uint64_t
pit_ticks(void)
{

	return (__atomic_load_n(&pit_tick_count, __ATOMIC_ACQUIRE));
}

unsigned int
pit_hz(void)
{

	return (pit_actual_hz);
}

void
pit_release_preempt(void)
{

	pit_debits_slice = false;
}

static void
pit_isr(struct trapframe *tf)
{

	(void)tf;
	__atomic_add_fetch(&pit_tick_count, 1, __ATOMIC_RELEASE);

	/*
	 * Track quantum usage for whatever is running on THIS CPU -- the
	 * tick is charged to the CPU that took it, which is the CPU whose
	 * slice is being spent.  When the quantum is exhausted, ask for a
	 * reschedule; intr_dispatch (or the next spin_unlock-to-zero)
	 * honours it.  We do NOT
	 * gate on preempt_is_enabled here: the gate lives at the
	 * actual schedule point, not at the flag-set point, so that
	 * a critical section ending later still has the resched
	 * pending.
	 *
	 * ONLY while this is still the machine's only timer.  There is one
	 * PIT and it interrupts one CPU, so the slice it can debit is that
	 * one CPU's; a local APIC timer takes the job per-processor and this
	 * gate is how it is handed over.  Both debiting would halve every
	 * quantum, which no test asserts and no message reports.
	 */
	if (pit_debits_slice && preempt_quantum_tick())
		preempt_resched_request();

	/*
	 * Deadline-driven wakes (sched_check_timeouts) do NOT belong
	 * here -- spin_trylock inside that routine drops preempt_count
	 * via spin_unlock, and a preempt_count transition to zero from
	 * inside the ISR would yield BEFORE pic_eoi runs, leaving the
	 * 8259 holding the IRQ and starving future PIT ticks.  The
	 * check runs from intr_dispatch's tail instead, after pic_eoi.
	 */
}
