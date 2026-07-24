/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_RTC_H_
#define	_SYS_RTC_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * MC146818 CMOS real-time clock -- the only source of WALL time on this
 * machine.  Everything else the kernel calls "time" is uptime: the PIT counts
 * ticks since boot and the TSC interpolates between them, which answers "how
 * long since we started" and cannot answer "what year is it".  A filesystem
 * that stores timestamps (APFS does, to the nanosecond) and a program that
 * prints dates both need the second question answered, and this chip is where
 * the answer comes from.
 *
 * It is read exactly once, at boot: the RTC is slow to read (two port accesses
 * per field, and a field may be mid-update), while the TSC is a register.  So
 * boot anchors wall time to the RTC and every reading after that is the anchor
 * plus elapsed uptime -- accurate, monotonic, and cheap.  See kern/clock.c.
 */

/* Broken-down UTC, as the chip reports it after decoding. */
struct rtc_time {
	uint16_t	rt_year;	/* full year, e.g. 2026 */
	uint8_t		rt_month;	/* 1-12 */
	uint8_t		rt_day;		/* 1-31 */
	uint8_t		rt_hour;	/* 0-23 */
	uint8_t		rt_min;		/* 0-59 */
	uint8_t		rt_sec;		/* 0-59 */
};

/*
 * Read the chip.  Returns false if it reports something impossible (an
 * unset or dead RTC), in which case *out is untouched and the caller should
 * treat wall time as unavailable rather than trust a garbage year.
 */
bool	rtc_read(struct rtc_time *out);

/*
 * Seconds since 1970-01-01T00:00:00Z for a broken-down UTC time.  Pure
 * arithmetic, no chip access; exposed because the filesystem layer converts
 * timestamps the other way and both want the same civil-calendar rules.
 */
int64_t	rtc_to_epoch(const struct rtc_time *t);

/*
 * The inverse: broken-down UTC from seconds since the epoch.  Anything that
 * PRINTS a time needs this -- the kernel's date command now, file timestamps
 * later, since a filesystem stores epoch counts and a listing shows dates.
 */
void	rtc_from_epoch(int64_t secs, struct rtc_time *out);

#endif /* !_SYS_RTC_H_ */
