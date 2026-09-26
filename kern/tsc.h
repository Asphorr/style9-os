/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_TSC_H_
#define	_SYS_TSC_H_

#include <stdint.h>

/*
 * Time Stamp Counter: a free-running 64-bit cycle counter, cheap to read
 * (rdtsc) but of implementation-defined frequency.  tsc_calibrate measures
 * it against the PIT once at boot and treats the result as constant.
 *
 * Calibration must run after pit_init and after sti: it counts
 * IRQ-driven PIT ticks, 25 of them (250 ms at 100 Hz).
 */

void		tsc_calibrate(void);
uint64_t	tsc_hz(void);

/*
 * The TSC value and the PIT tick count latched together at the end of
 * calibration: the reference for time finer than the tick (anchor ticks
 * plus TSC delta since).  Both read 0 before tsc_calibrate has run.
 */
uint64_t	tsc_anchor_cycles(void);
uint64_t	tsc_anchor_ticks(void);
uint64_t	tsc_to_ns(uint64_t cycles);
uint64_t	tsc_to_us(uint64_t cycles);

static inline uint64_t
tsc_read(void)
{
	uint32_t	lo, hi;

	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return (((uint64_t)hi << 32) | lo);
}

#endif /* !_SYS_TSC_H_ */
