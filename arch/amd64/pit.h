/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_PIT_H_
#define	_MACHINE_PIT_H_

#include <stdint.h>

/*
 * Intel 8253/8254 Programmable Interval Timer.
 *
 * Channel 0 drives IRQ0 on the master 8259 (vector 32 after pic_init's
 * remap).  pit_init programs it as a rate generator at the requested hz
 * and installs a handler that bumps a monotonic tick count (pit_ticks).
 *
 *	PIT_INPUT_HZ	the 1.193182 MHz input clock; the divisor is an
 *			integer, so pit_hz() returns the rate achieved.
 *	PIT_DEFAULT_HZ	100 Hz (10 ms tick), the rate clock_init uses.
 */

#define	PIT_INPUT_HZ		1193182U
#define	PIT_DEFAULT_HZ		100U

void		pit_init(unsigned int hz);
uint64_t	pit_ticks(void);
unsigned int	pit_hz(void);

/*
 * Stop debiting the running thread's slice; keep ticking.  Called once, by
 * lapic_timer_start, when the per-CPU timers take preemption over; the
 * tick count, timeouts and busy-sleeps stay on PIT ticks.  One-way: two
 * timers debiting one quantum would silently halve it.
 */
void		pit_release_preempt(void);

/*
 * Period, in PIT ticks, of the per-CPU census written straight at the
 * UART (cpu_census_uart); zero, the default, for never.
 */
extern uint64_t	pit_census_ticks;

#endif /* !_MACHINE_PIT_H_ */
