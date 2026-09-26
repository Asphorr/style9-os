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
	 * Anchor wall time.  Read the uptime right after the chip: the RTC
	 * read can spin up to a second waiting out an update, and an uptime
	 * taken before it would bake that delay into every later reading.
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

/*
 * Uptime is 0 until clock_init has set the PIT rate.  An interrupt taken
 * earlier -- IRQ1 is live from kbd_init, before the clock -- reaches
 * sched_check_timeouts, which asks for the time; dividing by the unset
 * rate would be a #DE in the middle of bring-up.
 */
uint64_t
clock_uptime_ms(void)
{
	unsigned int	hz;

	/*
	 * pit_hz() is at most ~1.2M so ticks * 1000 cannot overflow
	 * for any realistic uptime.  pit_ticks is monotonic.
	 */
	hz = pit_hz();
	if (hz == 0)
		return (0);
	return ((pit_ticks() * 1000ULL) / hz);
}

uint64_t
clock_uptime_us(void)
{
	uint64_t	base_us;
	unsigned int	hz;

	/*
	 * Sub-tick resolution.  The PIT tick count at the calibration anchor
	 * is the base; the TSC delta since that anchor adds the rest.  Both
	 * halves come from the same latched instant, so they compose without
	 * a seam, and the result is monotonic: neither anchor moves and the
	 * TSC only counts up.
	 */
	hz = pit_hz();
	if (hz == 0)				/* before clock_init */
		return (0);
	if (tsc_hz() == 0)			/* before calibration */
		return ((pit_ticks() * 1000000ULL) / hz);

	base_us = (tsc_anchor_ticks() * 1000000ULL) / hz;
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
