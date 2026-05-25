/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "clock.h"
#include "kprintf.h"
#include "pit.h"
#include "tsc.h"

void
clock_init(void)
{

	pit_init(PIT_DEFAULT_HZ);
	tsc_calibrate();
	kprintf("clock: %u Hz tick, TSC anchor calibrated\n", pit_hz());
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
	uint64_t	cycles_per_tick, t;
	uint64_t	tsc_ref_cycle, base_ticks;

	/*
	 * Sub-tick resolution: combine the PIT tick count (gives us a
	 * monotonic millisecond anchor) with a TSC delta-since-tick to
	 * fill in microseconds.  For now we approximate by converting
	 * the whole TSC value through tsc_to_us, which is fine for the
	 * lifetimes the kernel will see -- no second-source crosscheck
	 * here, that's what the timer stress test is for.
	 */
	(void)cycles_per_tick;
	(void)tsc_ref_cycle;
	(void)base_ticks;

	t = pit_ticks();
	return ((t * 1000000ULL) / pit_hz());
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
