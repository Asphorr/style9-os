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
 * Time.  Two different questions live here and they have different answers.
 *
 * UPTIME is how long since boot: the PIT counts ticks and the TSC interpolates
 * between them.  It is monotonic by construction and needs no hardware beyond
 * what boot already programmed.
 *
 * WALL time is what the calendar says, and no amount of counting since boot
 * can produce it -- it has to be read from a chip that was running before we
 * were.  The CMOS RTC (dev/rtc.c) is read ONCE, during clock_init, and the
 * result becomes an anchor; every reading after that is the anchor plus
 * elapsed uptime.  That keeps wall time as monotonic and as cheap as uptime,
 * and confines the slow, race-prone chip access to a single point at boot.
 *
 * clock_init programmes the PIT, calibrates the TSC, and takes that anchor;
 * nothing here works until it has run, and intr_enable must have been called
 * first so the calibration loop can observe IRQ-driven tick bumps.
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
