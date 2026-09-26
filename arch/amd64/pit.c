/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "cpu.h"
#include "intr.h"
#include "io.h"
#include "kprintf.h"
#include "mp.h"
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
 * True until the APIC timer takes the slice debit over
 * (pit_release_preempt); forever without a local APIC.  Written once,
 * before that timer is unmasked.
 */
static bool			pit_debits_slice = true;

static void	pit_isr(struct trapframe *tf);

/* Census period in ticks, zero for never; set by `cpu census N'. */
uint64_t	pit_census_ticks;

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
	 * Census of the CPUs, written straight at the UART: no lock, no
	 * formatting, no tty, so it still works when a wedge has taken the
	 * console away.  Off unless asked for.
	 */
	if (pit_census_ticks != 0 &&
	    (pit_tick_count % pit_census_ticks) == 0) {
		cpu_census_uart();
		mp_where(tf);
		mp_where_all();
	}

	/*
	 * Debit the slice of whatever runs on this (the boot) CPU and ask
	 * for a reschedule when it is spent; intr_dispatch or the next
	 * spin_unlock to zero honours it.  Not gated on preempt_is_enabled:
	 * the gate is at the schedule point, so a critical section ending
	 * later still owes the reschedule.
	 *
	 * Only until the APIC timer takes the job over; both debiting would
	 * silently halve every quantum.
	 */
	if (pit_debits_slice && preempt_quantum_tick())
		preempt_resched_request();

	/*
	 * No sched_check_timeouts here: its spin_unlock can drop the preempt
	 * count to zero and yield before pic_eoi, leaving the 8259 holding
	 * the IRQ.  intr_dispatch runs it after the EOI.
	 */
}
