/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stdint.h>

#include "io.h"
#include "rtc.h"

/*
 * MC146818 CMOS RTC.  See rtc.h for why this exists and why it is read once.
 *
 * Two hazards make a naive read wrong, and neither is rare:
 *
 *	1. The chip updates its registers once a second, and a read that lands
 *	   mid-update can see a half-carried time -- 01:59:60, or worse, 01:00
 *	   when it is 02:00.  Status register A's top bit (UIP) says an update
 *	   is in progress, but checking it once is not enough: the update can
 *	   start right after the check.  The reliable move is to read the whole
 *	   time twice with UIP clear and accept it only when both agree.
 *
 *	2. The register format is not fixed.  Status register B says whether
 *	   values are BCD or binary and whether hours are 12- or 24-hour, and
 *	   real machines ship both ways.  In 12-hour mode the top bit of the
 *	   hour register means PM -- and it survives BCD decoding, so it has to
 *	   be stripped before decoding and applied after.
 */

#define	CMOS_ADDR	0x70
#define	CMOS_DATA	0x71

#define	CMOS_SEC	0x00
#define	CMOS_MIN	0x02
#define	CMOS_HOUR	0x04
#define	CMOS_DAY	0x07
#define	CMOS_MONTH	0x08
#define	CMOS_YEAR	0x09
#define	CMOS_STATUS_A	0x0A
#define	CMOS_STATUS_B	0x0B
#define	CMOS_CENTURY	0x32		/* ACPI's usual index; QEMU has it */

#define	STATUS_A_UIP	0x80
#define	STATUS_B_24H	0x02
#define	STATUS_B_BINARY	0x04
#define	HOUR_PM		0x80

/*
 * Bit 7 of the address port is the NMI-disable line on most chipsets, so a
 * read leaves it as it found it: the top bit of whatever we select is 0 here,
 * which is the "NMI enabled" state every other part of the system assumes.
 */
static uint8_t
cmos_read(uint8_t reg)
{

	outb(CMOS_ADDR, reg);
	return (inb(CMOS_DATA));
}

static uint8_t
from_bcd(uint8_t v)
{

	return ((uint8_t)((v & 0x0F) + ((v >> 4) * 10)));
}

/* One coherent snapshot of the six time registers, UIP-clear. */
static void
read_raw(struct rtc_time *t, uint8_t *century)
{

	while ((cmos_read(CMOS_STATUS_A) & STATUS_A_UIP) != 0)
		continue;
	t->rt_sec   = cmos_read(CMOS_SEC);
	t->rt_min   = cmos_read(CMOS_MIN);
	t->rt_hour  = cmos_read(CMOS_HOUR);
	t->rt_day   = cmos_read(CMOS_DAY);
	t->rt_month = cmos_read(CMOS_MONTH);
	t->rt_year  = cmos_read(CMOS_YEAR);
	*century    = cmos_read(CMOS_CENTURY);
}

static bool
same_raw(const struct rtc_time *a, const struct rtc_time *b, uint8_t ca,
    uint8_t cb)
{

	return (a->rt_sec == b->rt_sec && a->rt_min == b->rt_min &&
	    a->rt_hour == b->rt_hour && a->rt_day == b->rt_day &&
	    a->rt_month == b->rt_month && a->rt_year == b->rt_year &&
	    ca == cb);
}

bool
rtc_read(struct rtc_time *out)
{
	struct rtc_time	 a;
	struct rtc_time	 b;
	uint8_t		 ca;
	uint8_t		 cb;
	uint8_t		 status_b;
	uint8_t		 pm;
	int		 tries;

	/*
	 * Read until two consecutive snapshots agree.  Bounded: a chip that
	 * never settles is broken, and hanging the boot on it would be worse
	 * than booting without wall time.
	 */
	read_raw(&a, &ca);
	for (tries = 0; tries < 16; tries++) {
		read_raw(&b, &cb);
		if (same_raw(&a, &b, ca, cb))
			break;
		a  = b;
		ca = cb;
	}
	if (tries == 16)
		return (false);

	status_b = cmos_read(CMOS_STATUS_B);

	/* Strip PM before decoding: it shares a byte with the hour digits. */
	pm = 0;
	if ((status_b & STATUS_B_24H) == 0 && (b.rt_hour & HOUR_PM) != 0) {
		pm = 1;
		b.rt_hour &= (uint8_t)~HOUR_PM;
	}

	if ((status_b & STATUS_B_BINARY) == 0) {
		b.rt_sec   = from_bcd((uint8_t)b.rt_sec);
		b.rt_min   = from_bcd((uint8_t)b.rt_min);
		b.rt_hour  = from_bcd((uint8_t)b.rt_hour);
		b.rt_day   = from_bcd((uint8_t)b.rt_day);
		b.rt_month = from_bcd((uint8_t)b.rt_month);
		b.rt_year  = from_bcd((uint8_t)b.rt_year);
		cb         = from_bcd(cb);
	}

	if (pm != 0 && b.rt_hour < 12)
		b.rt_hour = (uint8_t)(b.rt_hour + 12);
	else if ((status_b & STATUS_B_24H) == 0 && pm == 0 && b.rt_hour == 12)
		b.rt_hour = 0;			/* 12 AM is hour 0 */

	/*
	 * The century register is optional and reads as 0 (or 0xFF) where it
	 * is absent.  Fall back to the usual two-digit convention rather than
	 * reporting year 26.
	 */
	if (cb >= 19 && cb <= 25)
		b.rt_year = (uint16_t)(cb * 100 + b.rt_year);
	else
		b.rt_year = (uint16_t)((b.rt_year < 70 ? 2000 : 1900) +
		    b.rt_year);

	if (b.rt_month < 1 || b.rt_month > 12 || b.rt_day < 1 ||
	    b.rt_day > 31 || b.rt_hour > 23 || b.rt_min > 59 ||
	    b.rt_sec > 60 || b.rt_year < 1970 || b.rt_year > 2200)
		return (false);

	*out = b;
	return (true);
}

int64_t
rtc_to_epoch(const struct rtc_time *t)
{
	int64_t	y;
	int64_t	era;
	int64_t	yoe;
	int64_t	doy;
	int64_t	doe;
	int64_t	days;
	int64_t	m;

	/*
	 * Howard Hinnant's days_from_civil.  The trick is to shift the year so
	 * it starts in March: then the leap day is the LAST day of the year
	 * instead of being wedged into the middle, and the month-length series
	 * becomes a single linear formula with no table and no special cases.
	 * Eras are 400 years, the span over which the Gregorian rule repeats.
	 */
	y = (int64_t)t->rt_year;
	m = (int64_t)t->rt_month;
	y -= (m <= 2);
	era = (y >= 0 ? y : y - 399) / 400;
	yoe = y - era * 400;				/* [0, 399] */
	doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + (int64_t)t->rt_day - 1;
	doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;	/* [0, 146096] */
	days = era * 146097 + doe - 719468;		/* shift epoch to 1970 */

	return (days * 86400 + (int64_t)t->rt_hour * 3600 +
	    (int64_t)t->rt_min * 60 + (int64_t)t->rt_sec);
}

void
rtc_from_epoch(int64_t secs, struct rtc_time *out)
{
	int64_t	days;
	int64_t	rem;
	int64_t	era;
	int64_t	doe;
	int64_t	yoe;
	int64_t	y;
	int64_t	doy;
	int64_t	mp;
	int64_t	d;
	int64_t	m;

	days = secs / 86400;
	rem  = secs % 86400;
	if (rem < 0) {				/* C rounds toward zero */
		rem += 86400;
		days -= 1;
	}

	/* civil_from_days -- the exact inverse of the shifted-year form above. */
	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	doe = days - era * 146097;			/* [0, 146096] */
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	y   = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);	/* [0, 365] */
	mp  = (5 * doy + 2) / 153;			/* [0, 11], March=0 */
	d   = doy - (153 * mp + 2) / 5 + 1;
	m   = mp + (mp < 10 ? 3 : -9);
	y  += (m <= 2);

	out->rt_year  = (uint16_t)y;
	out->rt_month = (uint8_t)m;
	out->rt_day   = (uint8_t)d;
	out->rt_hour  = (uint8_t)(rem / 3600);
	out->rt_min   = (uint8_t)((rem / 60) % 60);
	out->rt_sec   = (uint8_t)(rem % 60);
}
