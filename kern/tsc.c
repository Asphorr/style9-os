/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "kprintf.h"
#include "panic.h"
#include "pit.h"
#include "tsc.h"

/* (c) const after tsc_calibrate. */
static uint64_t	tsc_freq_hz;

/*
 * The end-of-calibration instant, latched from both clocks at once, so a
 * caller can read time between PIT ticks: the anchor's tick count plus
 * the TSC delta since it.  (c) as above.
 */
static uint64_t	tsc_anchor_tsc;
static uint64_t	tsc_anchor_pit;

/*
 * Spin until the PIT tick counter has advanced by `samples' ticks,
 * sampling the TSC at both ends: `samples' * 10 ms at PIT_DEFAULT_HZ.
 */
void
tsc_calibrate(void)
{
	uint64_t	t0, t1, p0, p1, samples;
	uint64_t	delta_tsc, delta_pit;

	/*
	 * 25 ticks at 100 Hz == 250 ms: shorter, and an SMI on one tick
	 * shows as noticeable error; longer only slows boot.
	 */
	samples = 25;

	/* Wait for a tick boundary so the first sample is clean. */
	p0 = pit_ticks();
	while (pit_ticks() == p0)
		__asm__ __volatile__ ("pause");

	p0 = pit_ticks();
	t0 = tsc_read();

	while (pit_ticks() - p0 < samples)
		__asm__ __volatile__ ("pause");

	p1 = pit_ticks();
	t1 = tsc_read();

	delta_tsc = t1 - t0;
	delta_pit = p1 - p0;
	if (delta_pit == 0)
		panic("tsc_calibrate: PIT did not advance");

	tsc_freq_hz = (delta_tsc * pit_hz()) / delta_pit;
	tsc_anchor_tsc = t1;
	tsc_anchor_pit = p1;

	kprintf("tsc: %llu Hz (~%llu MHz) over %llu PIT ticks "
	    "(%llu cycles)\n",
	    (unsigned long long)tsc_freq_hz,
	    (unsigned long long)(tsc_freq_hz / 1000000ULL),
	    (unsigned long long)delta_pit,
	    (unsigned long long)delta_tsc);
}

uint64_t
tsc_hz(void)
{

	return (tsc_freq_hz);
}

uint64_t
tsc_anchor_cycles(void)
{

	return (tsc_anchor_tsc);
}

uint64_t
tsc_anchor_ticks(void)
{

	return (tsc_anchor_pit);
}

/*
 * tsc_to_ns / tsc_to_us: cycles * 10^N / hz, multiplying first only when
 * the product fits in 64 bits, dividing first otherwise.  0 before
 * calibration.
 */
uint64_t
tsc_to_ns(uint64_t cycles)
{

	if (tsc_freq_hz == 0)
		return (0);
	if (cycles < ((uint64_t)1 << 34))		/* cycles * 1e9 fits */
		return ((cycles * 1000000000ULL) / tsc_freq_hz);
	return ((cycles / tsc_freq_hz) * 1000000000ULL);
}

uint64_t
tsc_to_us(uint64_t cycles)
{

	if (tsc_freq_hz == 0)
		return (0);
	if (cycles < ((uint64_t)1 << 44))		/* cycles * 1e6 fits */
		return ((cycles * 1000000ULL) / tsc_freq_hz);
	return ((cycles / tsc_freq_hz) * 1000000ULL);
}
