/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "clock.h"
#include "kprintf.h"
#include "pit.h"
#include "rtc.h"
#include "tsc.h"

/*
 * Wall-clock anchor.  (i) = written once by clock_init before any reader
 * exists, read-only afterwards.
 */
static int64_t	wall_epoch_us;		/* (i) epoch time at the anchor    */
static uint64_t	wall_uptime_us;		/* (i) uptime at the same instant  */
static bool	wall_valid;		/* (i) */

void
clock_init(void)
{
	struct rtc_time	t;

	pit_init(PIT_DEFAULT_HZ);
	tsc_calibrate();
	kprintf("clock: %u Hz tick, TSC anchor calibrated\n", pit_hz());

	/*
	 * Anchor wall time.  Read the uptime immediately after the chip so the
	 * two refer to as nearly the same instant as we can manage; the RTC
	 * read itself can spin for up to a second waiting out an update, and
	 * anchoring to a stale uptime would bake that delay into every future
	 * reading.
	 */
	if (!rtc_read(&t)) {
		kprintf("clock: no usable RTC -- wall time unavailable\n");
		return;
	}
	wall_epoch_us  = rtc_to_epoch(&t) * 1000000LL;
	wall_uptime_us = clock_uptime_us();
	wall_valid     = true;
	kprintf("clock: RTC %u-%02u-%02u %02u:%02u:%02u UTC (epoch %lld)\n",
	    (unsigned)t.rt_year, (unsigned)t.rt_month, (unsigned)t.rt_day,
	    (unsigned)t.rt_hour, (unsigned)t.rt_min, (unsigned)t.rt_sec,
	    (long long)(wall_epoch_us / 1000000LL));
}

int64_t
clock_walltime_us(void)
{

	if (!wall_valid)
		return (0);
	return (wall_epoch_us + (int64_t)(clock_uptime_us() - wall_uptime_us));
}

bool
clock_walltime_valid(void)
{

	return (wall_valid);
}

uint64_t
clock_ticks(void)
{

	return (pit_ticks());
}

uint64_t
clock_hz(void)
{

	return ((uint64_t)pit_hz());
}

uint64_t
clock_uptime_ms(void)
{

	/*
	 * pit_hz() is at most ~1.2M so ticks * 1000 cannot overflow
	 * for any realistic uptime.  pit_ticks is monotonic.
	 */
	return ((pit_ticks() * 1000ULL) / pit_hz());
}

uint64_t
clock_uptime_us(void)
{
	uint64_t	base_us;

	/*
	 * Sub-tick resolution, which the name has always promised and this
	 * function used not to deliver: it returned the tick count scaled to
	 * microseconds, so every reading was a multiple of 10 ms and a caller
	 * asking for microseconds got three zeroes and no warning.
	 *
	 * The PIT tick count is the base -- coarse, but it cannot drift.  The
	 * TSC delta since the calibration anchor fills in what happened since
	 * the last tick.  Both halves come from the same latched instant, so
	 * they compose without a seam, and the result stays monotonic because
	 * neither anchor ever moves and the TSC only counts up.
	 */
	if (tsc_hz() == 0)			/* before calibration */
		return ((pit_ticks() * 1000000ULL) / pit_hz());

	base_us = (tsc_anchor_ticks() * 1000000ULL) / pit_hz();
	return (base_us + tsc_to_us(tsc_read() - tsc_anchor_cycles()));
}

void
clock_busy_sleep_ms(uint64_t ms)
{
	uint64_t	target_ticks, start;

	if (ms == 0)
		return;

	start = pit_ticks();
	target_ticks = (ms * pit_hz() + 999ULL) / 1000ULL;	/* ceil */

	while (pit_ticks() - start < target_ticks)
		__asm__ __volatile__ ("pause");
}
