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

static void
pit_isr(struct trapframe *tf)
{
	unsigned int	q;

	(void)tf;
	__atomic_add_fetch(&pit_tick_count, 1, __ATOMIC_RELEASE);

	/*
	 * Track quantum usage for the currently-running thread.  When
	 * the quantum is exhausted, set need_resched -- intr_dispatch
	 * (or the next spin_unlock-to-zero) honours it.  We do NOT
	 * gate on preempt_is_enabled here: the gate lives at the
	 * actual schedule point, not at the flag-set point, so that
	 * a critical section ending later still has the resched
	 * pending.
	 */
	q = __atomic_add_fetch(&preempt_quantum_used, 1, __ATOMIC_RELAXED);
	if (q >= PREEMPT_QUANTUM_TICKS)
		__atomic_store_n(&preempt_need_resched, 1,
		    __ATOMIC_RELAXED);

	/*
	 * Deadline-driven wakes (sched_check_timeouts) do NOT belong
	 * here -- spin_trylock inside that routine drops preempt_count
	 * via spin_unlock, and a preempt_count transition to zero from
	 * inside the ISR would yield BEFORE pic_eoi runs, leaving the
	 * 8259 holding the IRQ and starving future PIT ticks.  The
	 * check runs from intr_dispatch's tail instead, after pic_eoi.
	 */
}
