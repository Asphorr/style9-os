/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_CLOCK_H_
#define	_SYS_CLOCK_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Time.
 *
 * Uptime is time since boot: PIT ticks, with the TSC for sub-tick
 * resolution.  Monotonic by construction.
 *
 * Wall time: the CMOS RTC (dev/rtc.c) is read once, in clock_init, as an
 * anchor, and every reading after is the anchor plus elapsed uptime.  That
 * keeps wall time as monotonic and cheap as uptime, and confines the slow,
 * race-prone chip access to boot.
 *
 * clock_init programs the PIT, calibrates the TSC and takes the anchor.
 * Nothing here works before it -- clock_uptime_ms divides by pit_hz() --
 * and intr_enable must come first, since calibration counts IRQ-driven
 * ticks.
 */

void		clock_init(void);
uint64_t	clock_ticks(void);
uint64_t	clock_hz(void);
uint64_t	clock_uptime_ms(void);
uint64_t	clock_uptime_us(void);
void		clock_busy_sleep_ms(uint64_t ms);

/*
 * Microseconds since 1970-01-01T00:00:00Z, or 0 when the machine has no
 * usable RTC.  Callers that must distinguish "epoch" from "unknown" should
 * ask clock_walltime_valid() rather than testing for zero.
 */
int64_t		clock_walltime_us(void);
bool		clock_walltime_valid(void);

#endif /* !_SYS_CLOCK_H_ */
