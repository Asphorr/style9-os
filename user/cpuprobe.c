/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * cpuprobe -- CPU time as getrusage(2) and wait4(2) report it.  A
 * self-authored Darwin-ABI probe like lockprobe, bound by our dyld against
 * our libSystem.  Spans are measured on the wall clock; the kernel's
 * figures must account for them:
 *
 *	1. A spin in ring 3 is user time: at least half the span, and
 *	   system time under a quarter of it.
 *	2. Reading a file over and over is system time: more than user time.
 *	3. Sleeping is neither.
 *	4. wait4 reports what a child used, its own waited-for child's
 *	   included, and the parent waiting meanwhile used almost none.
 *	5. RUSAGE_CHILDREN then holds exactly what wait4 reported.
 *	6. times(3), clock(3) and clock_gettime's process CPU clock fall
 *	   between two getrusage readings taken around them.
 *
 * Freestanding: prototypes declared here, entry at _entry (ld -e).
 */

#define	NULL		((void *)0)

#define	O_RDWR		0x0002
#define	O_CREAT		0x0200
#define	O_TRUNC		0x0400

#define	RUSAGE_SELF	0
#define	RUSAGE_CHILDREN	(-1)

#define	CLK_TCK		100
#define	CLOCK_PROCESS_CPUTIME_ID 12

#define	PATH		"/etc/cpuprobe.dat"

struct timeval {
	long		tv_sec;
	int		tv_usec;
	int		tv_pad;
};

struct timespec {
	long		tv_sec;
	long		tv_nsec;
};

struct rusage {
	struct timeval	ru_utime;
	struct timeval	ru_stime;
	long		ru_counters[14];
};

struct tms {
	unsigned long	tms_utime;
	unsigned long	tms_stime;
	unsigned long	tms_cutime;
	unsigned long	tms_cstime;
};

extern int		 gettimeofday(struct timeval *tv, void *tz);
extern int		 getrusage(int who, struct rusage *ru);
extern int		 wait4(int pid, int *status, int options,
			    struct rusage *ru);
extern long		 times(struct tms *t);
extern unsigned long	 clock(void);
extern int		 clock_gettime(int clk, struct timespec *ts);
extern int		 open(const char *path, int flags, ...);
extern long		 write(int fd, const void *buf, unsigned long n);
extern long		 pread(int fd, void *buf, unsigned long n, long off);
extern int		 close(int fd);
extern int		 unlink(const char *path);
extern int		 fork(void);
extern int		 select(int n, void *r, void *w, void *e,
			    struct timeval *tv);
extern int		 printf(const char *fmt, ...);
extern void		 exit(int code);

static const char	*who = "cpuprobe";
static int		 fails;
static volatile unsigned long sink;

static void
fail(const char *what)
{

	printf("%s: FAIL %s\n", who, what);
	fails++;
}

static void
pass(const char *what)
{

	printf("%s: PASS %s\n", who, what);
}

static long
tv_us(const struct timeval *tv)
{

	return (tv->tv_sec * 1000000L + tv->tv_usec);
}

static long
now_us(void)
{
	struct timeval	tv;

	if (gettimeofday(&tv, NULL) != 0)
		return (0);
	return (tv_us(&tv));
}

/* User and system time so far, in microseconds; -1 each if refused. */
static void
used(int whom, long *u, long *s)
{
	struct rusage	ru;

	if (getrusage(whom, &ru) != 0) {
		*u = -1;
		*s = -1;
		return;
	}
	*u = tv_us(&ru.ru_utime);
	*s = tv_us(&ru.ru_stime);
}

/* Spin in ring 3 for `ms` of wall time, looking at the clock seldom. */
static void
spin(long ms)
{
	unsigned long	i;
	long		end;

	end = now_us() + ms * 1000L;
	do {
		for (i = 0; i < 20000; i++)
			sink += i;
	} while (now_us() < end);
}

static void
nap(long ms)
{
	struct timeval	tv;

	tv.tv_sec  = ms / 1000;
	tv.tv_usec = (int)(ms % 1000) * 1000;
	tv.tv_pad  = 0;
	(void)select(0, NULL, NULL, NULL, &tv);
}

/* Step 4's child: spin, have a child spin, wait for it, exit. */
static void
child(void)
{
	int	pid;
	int	status;

	who = "cpuprobe[child]";
	spin(150);
	pid = fork();
	if (pid == 0) {
		spin(150);
		exit(0);
	}
	if (pid < 0 || wait4(pid, &status, 0, NULL) != pid)
		exit(1);
	exit(0);
}

int
entry(void)
{
	struct rusage	ru;
	struct timespec	ts;
	struct tms	tm;
	char		buf[4096];
	long		u0, s0, u1, s1, cu, cs, w0, w1;
	unsigned long	ck;
	int		fd;
	int		i;
	int		pid;
	int		status;

	printf("cpuprobe: CPU time from getrusage, wait4 and times\n");

	/* 1. A spin is user time. */
	used(RUSAGE_SELF, &u0, &s0);
	w0 = now_us();
	spin(200);
	w1 = now_us();
	used(RUSAGE_SELF, &u1, &s1);
	printf("cpuprobe: spin %ld us of wall time: user +%ld us, system "
	    "+%ld us\n", w1 - w0, u1 - u0, s1 - s0);
	if (u0 < 0 || u1 < 0)
		fail("getrusage(RUSAGE_SELF) was refused");
	else if ((u1 - u0) * 2 < w1 - w0 || (s1 - s0) * 4 >= w1 - w0)
		fail("a spin in ring 3 was not counted as user time");
	else
		pass("a 200 ms spin in ring 3 is user time");

	/* 2. Reading a file is system time. */
	fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
	for (i = 0; i < (int)sizeof(buf); i++)
		buf[i] = (char)i;
	if (fd < 0 || write(fd, buf, sizeof(buf)) != (long)sizeof(buf)) {
		fail("could not make the file to read");
	} else {
		used(RUSAGE_SELF, &u0, &s0);
		w0 = now_us();
		do {
			for (i = 0; i < 16; i++)
				(void)pread(fd, buf, sizeof(buf), 0);
		} while (now_us() - w0 < 200000L);
		w1 = now_us();
		used(RUSAGE_SELF, &u1, &s1);
		printf("cpuprobe: pread for %ld us: user +%ld us, system "
		    "+%ld us\n", w1 - w0, u1 - u0, s1 - s0);
		if (s1 - s0 <= u1 - u0)
			fail("reading a file was not counted as system time");
		else
			pass("reading a file over and over is system time");
	}
	if (fd >= 0)
		(void)close(fd);
	(void)unlink(PATH);

	/* 3. Sleeping is neither. */
	used(RUSAGE_SELF, &u0, &s0);
	w0 = now_us();
	nap(300);
	w1 = now_us();
	used(RUSAGE_SELF, &u1, &s1);
	printf("cpuprobe: slept %ld us: user +%ld us, system +%ld us\n",
	    w1 - w0, u1 - u0, s1 - s0);
	if ((u1 - u0 + s1 - s0) * 5 >= w1 - w0)
		fail("a sleep was counted as CPU time");
	else
		pass("sleeping 300 ms is neither user nor system time");

	/* 4. What a child used, its own child's included. */
	used(RUSAGE_SELF, &u0, &s0);
	pid = fork();
	if (pid == 0)
		child();
	if (pid < 0 || wait4(pid, &status, 0, &ru) != pid || status != 0) {
		fail("the child could not be run and waited for");
		ru.ru_utime.tv_sec = -1;
	}
	used(RUSAGE_SELF, &u1, &s1);
	printf("cpuprobe: wait4 says the child used user %ld us, system "
	    "%ld us; the parent +%ld us meanwhile\n", tv_us(&ru.ru_utime),
	    tv_us(&ru.ru_stime), u1 - u0 + s1 - s0);
	if (tv_us(&ru.ru_utime) < 150000L)
		fail("wait4 did not report the child's and grandchild's "
		    "spins");
	else if (u1 - u0 + s1 - s0 >= 100000L)
		fail("the waiting parent was charged for its child");
	else
		pass("wait4 reports a child's time with its own child's, and "
		    "waiting costs the parent none");

	/* 5. The same figures in RUSAGE_CHILDREN. */
	used(RUSAGE_CHILDREN, &cu, &cs);
	if (cu != tv_us(&ru.ru_utime) || cs != tv_us(&ru.ru_stime))
		fail("RUSAGE_CHILDREN differs from what wait4 reported");
	else
		pass("RUSAGE_CHILDREN holds exactly what wait4 reported");

	/* 6. times, clock and clock_gettime between two readings. */
	used(RUSAGE_SELF, &u0, &s0);
	w0 = times(&tm);
	ck = clock();
	i  = clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts);
	used(RUSAGE_SELF, &u1, &s1);
	if (w0 == -1 || i != 0)
		fail("times or clock_gettime was refused");
	else if (tm.tms_utime < (unsigned long)u0 / (1000000 / CLK_TCK) ||
	    tm.tms_utime > (unsigned long)u1 / (1000000 / CLK_TCK) ||
	    tm.tms_stime < (unsigned long)s0 / (1000000 / CLK_TCK) ||
	    tm.tms_stime > (unsigned long)s1 / (1000000 / CLK_TCK) ||
	    tm.tms_cutime != (unsigned long)cu / (1000000 / CLK_TCK) ||
	    tm.tms_cstime != (unsigned long)cs / (1000000 / CLK_TCK))
		fail("times(3) disagrees with getrusage");
	else if (ck < (unsigned long)(u0 + s0) ||
	    ck > (unsigned long)(u1 + s1))
		fail("clock(3) disagrees with getrusage");
	else if (ts.tv_sec * 1000000L + ts.tv_nsec / 1000 < u0 + s0 ||
	    ts.tv_sec * 1000000L + ts.tv_nsec / 1000 > u1 + s1)
		fail("clock_gettime's process CPU clock disagrees with "
		    "getrusage");
	else
		pass("times, clock and clock_gettime agree with getrusage");

	if (fails != 0) {
		printf("cpuprobe: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("cpuprobe: done -- every check passed\n");
	exit(0);
	return (0);
}
