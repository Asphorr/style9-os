/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_CLOCK_H_
#define	_SYS_CLOCK_H_

#include <stdint.h>

/*
 * Wall-clock layer.  Built on top of the PIT for tick-driven time and
 * the TSC for sub-tick resolution.  No timekeeping callouts yet --
 * just enough to report uptime and busy-wait for a duration.
 *
 * clock_init programmes the PIT and calibrates the TSC; nothing else
 * here works until it has run, and intr_enable must have been called
 * first so the calibration loop can observe IRQ-driven tick bumps.
 */

void		clock_init(void);
uint64_t	clock_ticks(void);
uint64_t	clock_hz(void);
uint64_t	clock_uptime_ms(void);
uint64_t	clock_uptime_us(void);
void		clock_busy_sleep_ms(uint64_t ms);

#endif /* !_SYS_CLOCK_H_ */
