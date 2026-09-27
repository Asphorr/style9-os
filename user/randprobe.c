/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * randprobe -- the kernel's random numbers as a Darwin program meets them:
 * /dev/urandom, /dev/random, getentropy(2) and arc4random(3).  A
 * self-authored Darwin-ABI probe like lockprobe, bound by our dyld against
 * our libSystem.  Statistics cannot prove a generator good, only catch one
 * that is broken; the bounds here are that loose.
 *
 *	1. 64 KiB from /dev/urandom: every byte value turns up, and the
 *	   chi-square statistic over the 256 is under 400 (its mean is 255,
 *	   its deviation about 23); a second read differs from the first.
 *	2. /dev/random is the same device; a write to it is taken whole;
 *	   lseek on it answers 0; neither it nor /dev/null is a terminal.
 *	3. getentropy gives up to 256 bytes and refuses 257 with EINVAL.
 *	4. A parent and its child draw different bytes after the fork.
 *	5. arc4random_uniform(10) stays under 10 and reaches all ten.
 *
 * Freestanding: prototypes declared here, entry at _entry (ld -e).
 */

typedef __UINT8_TYPE__	uint8_t;
typedef __UINT32_TYPE__	uint32_t;
typedef __SIZE_TYPE__	size_t;

#define	NULL		((void *)0)

#define	O_RDONLY	0x0000
#define	O_RDWR		0x0002

#define	EINVAL		22

#define	BULK		65536

extern int	*__error(void);
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern long	 write(int fd, const void *buf, unsigned long n);
extern long	 lseek(int fd, long off, int whence);
extern int	 close(int fd);
extern int	 isatty(int fd);
extern int	 getentropy(void *buf, size_t n);
extern uint32_t	 arc4random_uniform(uint32_t bound);
extern int	 fork(void);
extern int	 pipe(int fds[2]);
extern int	 waitpid(int pid, int *status, int options);
extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);

static int		fails;
static uint8_t		bulk[BULK];
static uint8_t		again[BULK];

static void
fail(const char *what)
{

	printf("randprobe: FAIL %s\n", what);
	fails++;
}

static void
pass(const char *what)
{

	printf("randprobe: PASS %s\n", what);
}

static int
same(const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return (0);
	return (1);
}

/* read(2) until `n` bytes or a failure; how many arrived. */
static long
fill(int fd, uint8_t *buf, size_t n)
{
	size_t	done;
	long	got;

	for (done = 0; done < n; done += (size_t)got) {
		got = read(fd, buf + done, n - done);
		if (got <= 0)
			break;
	}
	return ((long)done);
}

int
entry(void)
{
	unsigned long	count[256];
	unsigned long	chi;
	unsigned long	d;
	uint8_t		mine[32];
	uint8_t		theirs[32];
	uint8_t		big[257];
	uint32_t	v;
	int		seen[10];
	int		pfd[2];
	int		fd;
	int		fd2;
	int		i;
	int		pid;
	int		status;
	int		missing;
	int		bad;

	printf("randprobe: /dev/urandom, /dev/random, getentropy, "
	    "arc4random\n");

	/* 1. The bulk read. */
	fd = open("/dev/urandom", O_RDONLY);
	if (fd < 0 || fill(fd, bulk, BULK) != BULK ||
	    fill(fd, again, BULK) != BULK) {
		fail("/dev/urandom could not be opened and read");
	} else {
		for (i = 0; i < 256; i++)
			count[i] = 0;
		for (i = 0; i < BULK; i++)
			count[bulk[i]]++;
		chi = 0;
		missing = 0;
		for (i = 0; i < 256; i++) {
			d = count[i] > BULK / 256 ? count[i] - BULK / 256 :
			    BULK / 256 - count[i];
			chi += d * d;
			if (count[i] == 0)
				missing++;
		}
		chi /= BULK / 256;
		printf("randprobe: 64 KiB of /dev/urandom -- chi-square %lu "
		    "over 255 degrees of freedom\n", chi);
		if (missing != 0 || chi >= 400)
			fail("the bytes of /dev/urandom are not spread evenly");
		else if (same(bulk, again, BULK))
			fail("two reads of /dev/urandom gave the same bytes");
		else
			pass("64 KiB of /dev/urandom spread evenly over the "
			    "byte values, and a second read differs");
	}
	if (fd >= 0)
		(void)close(fd);

	/* 2. /dev/random, and what kind of thing it is. */
	fd  = open("/dev/random", O_RDWR);
	fd2 = open("/dev/null", O_RDWR);
	if (fd < 0 || fd2 < 0)
		fail("/dev/random or /dev/null could not be opened");
	else if (fill(fd, mine, sizeof(mine)) != (long)sizeof(mine) ||
	    same(mine, bulk, sizeof(mine)))
		fail("/dev/random could not be read");
	else if (write(fd, "style9", 6) != 6)
		fail("a write to /dev/random was not taken whole");
	else if (lseek(fd, 100, 0) != 0)
		fail("lseek on /dev/random did not answer 0");
	else if (isatty(fd) || isatty(fd2))
		fail("/dev/random or /dev/null claims to be a terminal");
	else
		pass("/dev/random reads, takes a write whole, seeks to 0, "
		    "and is no terminal, nor is /dev/null");
	if (fd >= 0)
		(void)close(fd);
	if (fd2 >= 0)
		(void)close(fd2);

	/* 3. getentropy's limit. */
	if (getentropy(big, 256) != 0)
		fail("getentropy refused 256 bytes");
	else if (getentropy(big, 257) != -1 || *__error() != EINVAL)
		fail("getentropy did not refuse 257 bytes with EINVAL");
	else
		pass("getentropy gives 256 bytes and refuses 257 with EINVAL");

	/* 4. Across a fork. */
	if (pipe(pfd) != 0) {
		fail("pipe");
	} else {
		pid = fork();
		if (pid == 0) {
			(void)close(pfd[0]);
			(void)getentropy(theirs, sizeof(theirs));
			(void)write(pfd[1], theirs, sizeof(theirs));
			exit(0);
		}
		(void)close(pfd[1]);
		(void)getentropy(mine, sizeof(mine));
		if (pid < 0 || fill(pfd[0], theirs, sizeof(theirs)) !=
		    (long)sizeof(theirs) || waitpid(pid, &status, 0) != pid)
			fail("the child's bytes never arrived");
		else if (same(mine, theirs, sizeof(mine)))
			fail("a parent and its child drew the same bytes");
		else
			pass("a parent and its child draw different bytes "
			    "after the fork");
		(void)close(pfd[0]);
	}

	/* 5. arc4random_uniform. */
	for (i = 0; i < 10; i++)
		seen[i] = 0;
	bad = 0;
	for (i = 0; i < 1000; i++) {
		v = arc4random_uniform(10);
		if (v >= 10)
			bad++;
		else
			seen[v] = 1;
	}
	missing = 0;
	for (i = 0; i < 10; i++)
		if (!seen[i])
			missing++;
	if (bad != 0 || missing != 0)
		fail("arc4random_uniform(10) left its range or missed a value");
	else
		pass("arc4random_uniform(10) stays under 10 and reaches all "
		    "ten in 1000 draws");

	if (fails != 0) {
		printf("randprobe: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("randprobe: done -- every check passed\n");
	exit(0);
	return (0);
}
