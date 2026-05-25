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
 * Calibration loop: spin until the PIT tick counter has advanced by
 * `samples` ticks, sampling TSC at both ends.  At PIT_DEFAULT_HZ this
 * is `samples * 10 ms` of wall clock.  We use ACQUIRE-ordered tick
 * reads to make sure we observe the IRQ handler's bump before we
 * commit to leaving the loop.
 */
void
tsc_calibrate(void)
{
	uint64_t	t0, t1, p0, p1, samples;
	uint64_t	delta_tsc, delta_pit;

	/*
	 * 25 ticks at 100Hz == 250 ms; smaller than that and an SMI on
	 * a single tick produces noticeable error, larger drags out
	 * boot pointlessly.
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

/*
 * tsc_to_ns / tsc_to_us: scale cycles into wall time.  Compute as
 * cycles * 10^N / hz with 128-bit-safe ordering (multiply first only
 * when the multiplication won't overflow uint64; otherwise divide
 * first to keep within u64).  At 10 GHz a 64-bit cycle count covers
 * ~58 years, so this is forgiving in practice.
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
