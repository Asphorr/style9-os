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
 * Intel 8253 / 8254 Programmable Interval Timer.
 *
 * Channel 0 is wired to IRQ0 on the master 8259 (vector 32 after the
 * project's pic_init remap).  We programme it in rate-generator mode
 * with the divisor that produces the requested hz, then install a
 * handler that simply increments a monotonic tick counter.  Read with
 * pit_ticks().
 *
 *	PIT_INPUT_HZ	the canonical 1.193182 MHz input clock; divisor
 *			is integer so the actual rate is the nearest
 *			achievable (returned by pit_hz()).
 *	PIT_DEFAULT_HZ	100 Hz (10 ms tick) -- the rate clock_init uses
 *			when no override is supplied.
 */

#define	PIT_INPUT_HZ		1193182U
#define	PIT_DEFAULT_HZ		100U

void		pit_init(unsigned int hz);
uint64_t	pit_ticks(void);
unsigned int	pit_hz(void);

#endif /* !_MACHINE_PIT_H_ */
