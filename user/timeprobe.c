/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * timeprobe -- a self-authored Darwin-ABI probe for the wall clock, in the
 * same role dirlist played for readdir and pipefork for fork/exec: prove the
 * syscall from ring 3 with a small binary built by the real toolchain, before
 * a genuine Apple binary (gdate, or ls -l's timestamps) depends on it.
 *
 * It checks three things a clock has to get right, and they are not the same
 * check:
 *
 *	1. the value is PLAUSIBLE -- a date in this century, not 1970 and not
 *	   year 2600.  A broken BCD decode or a bad epoch conversion lands
 *	   somewhere absurd, and absurd is easy to see.
 *
 *	2. it ADVANCES -- two readings across some work must differ, or the
 *	   clock is a constant wearing a timestamp's clothes.
 *
 *	3. it NEVER GOES BACKWARDS -- sampled repeatedly, no reading may be
 *	   less than the one before it.  This is the property everything that
 *	   measures an interval quietly relies on.
 *
 * It also decodes the epoch into a date itself, so the printed line can be
 * eyeballed against the host that launched QEMU.
 */

typedef __UINT32_TYPE__	uint32_t;
typedef __UINT64_TYPE__	uint64_t;
typedef __INT64_TYPE__	int64_t;

#define	NULL	((void *)0)

struct timeval {
	long	tv_sec;
	int	tv_usec;
	int	tv_pad;
};

struct timespec {
	long	tv_sec;
	long	tv_nsec;
};

extern int	gettimeofday(struct timeval *tv, void *tz);
extern int	clock_gettime(int clk, struct timespec *ts);
extern long	time(long *t);
extern int	printf(const char *fmt, ...);
extern void	exit(int code);

/* civil_from_days, so the probe can name the date it is looking at. */
static void
civil(int64_t secs, int *y, int *mo, int *d, int *h, int *mi, int *s)
{
	int64_t	days, rem, era, doe, yoe, yy, doy, mp, dd, mm;

	days = secs / 86400;
	rem  = secs % 86400;
	if (rem < 0) {
		rem += 86400;
		days -= 1;
	}
	days += 719468;
	era = (days >= 0 ? days : days - 146096) / 146097;
	doe = days - era * 146097;
	yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	yy  = yoe + era * 400;
	doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	mp  = (5 * doy + 2) / 153;
	dd  = doy - (153 * mp + 2) / 5 + 1;
	mm  = mp + (mp < 10 ? 3 : -9);
	yy += (mm <= 2);

	*y  = (int)yy;
	*mo = (int)mm;
	*d  = (int)dd;
	*h  = (int)(rem / 3600);
	*mi = (int)((rem / 60) % 60);
	*s  = (int)(rem % 60);
}

/* Enough arithmetic to take real time, without needing a sleep syscall. */
static uint64_t
spin(unsigned long n)
{
	volatile uint64_t	acc = 0;
	unsigned long		i;

	for (i = 0; i < n; i++)
		acc += i;
	return (acc);
}

int
entry(void)
{
	struct timeval	tv, tv2;
	struct timespec	ts;
	long		t;
	int64_t		prev, now;
	int		y, mo, d, h, mi, s;
	int		i;
	int		fail = 0;

	if (gettimeofday(&tv, NULL) != 0) {
		printf("timeprobe: gettimeofday failed -- no wall clock\n");
		exit(1);
	}
	civil((int64_t)tv.tv_sec, &y, &mo, &d, &h, &mi, &s);
	printf("timeprobe: %d-%02d-%02dT%02d:%02d:%02dZ (epoch %lld.%06d)\n",
	    y, mo, d, h, mi, s, (long long)tv.tv_sec, tv.tv_usec);

	if (y < 2020 || y > 2100) {
		printf("timeprobe: FAIL year %d is not plausible\n", y);
		fail = 1;
	}

	/* time(3) and clock_gettime must agree with it to the second. */
	t = time(NULL);
	if (clock_gettime(0, &ts) != 0) {
		printf("timeprobe: FAIL clock_gettime failed\n");
		fail = 1;
	} else if (ts.tv_sec < tv.tv_sec || ts.tv_sec > tv.tv_sec + 2 ||
	    t < tv.tv_sec || t > tv.tv_sec + 2) {
		printf("timeprobe: FAIL time=%lld clock_gettime=%lld disagree "
		    "with gettimeofday=%lld\n", (long long)t,
		    (long long)ts.tv_sec, (long long)tv.tv_sec);
		fail = 1;
	}

	/* Advances. */
	(void)spin(20000000UL);
	if (gettimeofday(&tv2, NULL) != 0) {
		printf("timeprobe: FAIL second gettimeofday failed\n");
		exit(1);
	}
	prev = (int64_t)tv.tv_sec * 1000000 + tv.tv_usec;
	now  = (int64_t)tv2.tv_sec * 1000000 + tv2.tv_usec;
	if (now <= prev) {
		printf("timeprobe: FAIL clock did not advance (%lld -> %lld)\n",
		    (long long)prev, (long long)now);
		fail = 1;
	} else
		printf("timeprobe: advanced %lld us across a busy loop\n",
		    (long long)(now - prev));

	/* Never backwards. */
	prev = now;
	for (i = 0; i < 200; i++) {
		(void)spin(20000UL);
		if (gettimeofday(&tv2, NULL) != 0)
			continue;
		now = (int64_t)tv2.tv_sec * 1000000 + tv2.tv_usec;
		if (now < prev) {
			printf("timeprobe: FAIL went backwards at sample %d "
			    "(%lld -> %lld)\n", i, (long long)prev,
			    (long long)now);
			fail = 1;
			break;
		}
		prev = now;
	}

	printf("timeprobe: %s\n", fail ? "FAILED" : "all checks passed");
	exit(fail);
	return (0);
}
