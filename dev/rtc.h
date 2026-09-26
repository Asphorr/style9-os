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
 * MC146818 CMOS real-time clock, the only source of wall time; everything
 * else the kernel calls time is uptime.
 *
 * It is read once, at boot, since it is slow to read (two port accesses
 * per field, which may be mid-update).  Wall time after that is the boot
 * anchor plus elapsed uptime: monotonic and cheap.  See kern/clock.c.
 */

/* Broken-down UTC, as the chip reports it after decoding. */
struct rtc_time {
	uint16_t	rt_year;	/* full year, e.g. 2026 */
	uint8_t		rt_month;	/* 1-12 */
	uint8_t		rt_day;		/* 1-31 */
	uint8_t		rt_hour;	/* 0-23 */
	uint8_t		rt_min;		/* 0-59 */
	uint8_t		rt_sec;		/* 0-60 (leap second) */
};

/*
 * Read the chip.  Returns false, leaving *out untouched, if it reports
 * something impossible (an unset or dead RTC); treat wall time as
 * unavailable then.
 */
bool	rtc_read(struct rtc_time *out);

/*
 * Seconds since 1970-01-01T00:00:00Z for a broken-down UTC time.  Pure
 * arithmetic, no chip access.
 */
int64_t	rtc_to_epoch(const struct rtc_time *t);

/* The inverse: broken-down UTC from seconds since the epoch. */
void	rtc_from_epoch(int64_t secs, struct rtc_time *out);

#endif /* !_SYS_RTC_H_ */
