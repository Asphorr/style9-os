/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * ttyprobe -- a self-authored Darwin-ABI probe for terminal control:
 * turning echo off, asking the window size, reading one keystroke without
 * Return.  As with dirlist, pipefork and filewrite, a program we wrote
 * proves the syscalls from ring 3 before one we did not (coreutils' stty)
 * depends on them.
 *
 *	1. A fresh terminal is canonical with echo on, the state a session
 *	   must start in.
 *	2. The window size is the screen's, not a made-up 80x24.
 *	3. A file and a pipe both answer ENOTTY.  isatty(3) rests on this,
 *	   and gls picks columns by it: a pipe answering would put columns
 *	   in a file.
 *	4. An unknown ioctl answers ENOTTY, and TIOCSWINSZ EINVAL, instead
 *	   of succeeding quietly.
 *	5. A setting reads back: raw mode is set by one call and confirmed
 *	   by another, so a tcsetattr that libSystem only pretends to do
 *	   fails.
 *	6. With VMIN=0 a read returns 0; canonical mode would block until
 *	   Return.
 *	7. A byte arrives with no newline behind it.  The driver feeds one
 *	   character and no line ending, which a canonical line buffer
 *	   would hold forever.
 *	8. The restore takes, so the next shell has its terminal back.
 *
 * Readiness (for make) is checked here too, since this probe already
 * makes pipes:
 *
 *	9. select(2) with a zero timeout sees a pipe's read end go from not
 *	   ready to ready when a byte is written; the write end is ready
 *	   throughout.
 *	10. A 40 ms select on an empty pipe returns 0 after 40 ms: the wait
 *	    parked and the clock ended it, a path make's own pselect use
 *	    does not exercise.
 *	11. poll(2) reports POLLIN and POLLHUP together on a read end whose
 *	    writer closed with a byte still in the pipe.
 *	12. A closed descriptor is EBADF to select and POLLNVAL to poll.
 *
 * Freestanding: no SDK headers, prototypes declared as <termios.h> would alias
 * them, entry at _entry (ld -e), relinked low like dyldhello.
 */

typedef __UINT8_TYPE__	uint8_t;
typedef __UINT16_TYPE__	uint16_t;
typedef __UINT32_TYPE__	uint32_t;
typedef __UINT64_TYPE__	uint64_t;
typedef __SIZE_TYPE__	size_t;

#define	NULL		((void *)0)

#define	O_RDONLY	0x0000
#define	ENOTTY		25
#define	EINVAL		22

/* c_lflag */
#define	ECHO		0x00000008UL
#define	ISIG		0x00000080UL
#define	ICANON		0x00000100UL

/* c_cc subscripts */
#define	VMIN		16
#define	VTIME		17

#define	TCSANOW		0
#define	TCSAFLUSH	2

#define	TIOCGWINSZ	0x40087468UL
#define	TIOCSWINSZ	0x80087467UL
#define	TIOCJUNK	0x4004745AUL	/* _IOR('t', 90, int) -- nothing */

/*
 * Apple's struct termios, also the kernel's and stty's.  Written out rather
 * than shared through a header: a probe taking the layout from the thing
 * it probes would agree with it by construction.
 */
struct termios {
	uint64_t	c_iflag;
	uint64_t	c_oflag;
	uint64_t	c_cflag;
	uint64_t	c_lflag;
	uint8_t		c_cc[20];
	uint8_t		c_pad[4];
	uint64_t	c_ispeed;
	uint64_t	c_ospeed;
};

struct winsize {
	uint16_t	ws_row;
	uint16_t	ws_col;
	uint16_t	ws_xpixel;
	uint16_t	ws_ypixel;
};

/* select(2)/poll(2) as <sys/select.h> and <poll.h> lay them out. */
struct timeval {
	long	tv_sec;
	int	tv_usec;
	int	tv_pad;
};

struct pollfd {
	int	fd;
	short	events;
	short	revents;
};

#define	NFDBITS		32
#define	FD_WORDS	(1024 / NFDBITS)
#define	POLLIN		0x0001
#define	POLLOUT		0x0004
#define	POLLHUP		0x0010
#define	POLLNVAL	0x0020
#define	EBADF		9

extern int	*__error(void);
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern long	 write(int fd, const void *buf, unsigned long n);
extern int	 close(int fd);
extern int	 pipe(int fds[2]);
extern int	 select(int nfds, void *r, void *w, void *e, const void *tv);
extern int	 poll(void *fds, unsigned int nfds, int timeout);
extern int	 gettimeofday(void *tv, void *tz);
extern int	 ioctl(int fd, unsigned long request, ...);
extern int	 tcgetattr(int fd, void *tp);
extern int	 tcsetattr(int fd, int action, const void *tp);
extern unsigned long cfgetispeed(const void *tp);
extern unsigned long cfgetospeed(const void *tp);
extern int	 isatty(int fd);
extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);

#define	AFILE		"/etc/hello.txt"	/* off the image, always there */

static int	fails;

static void
fail(const char *what)
{

	printf("ttyprobe: FAIL %s\n", what);
	fails++;
}

static void
fd_zero(uint32_t *set)
{
	int	i;

	for (i = 0; i < FD_WORDS; i++)
		set[i] = 0;
}

static void
fd_set_(uint32_t *set, int fd)
{

	set[fd / NFDBITS] |= (uint32_t)1 << (fd % NFDBITS);
}

static int
fd_isset(const uint32_t *set, int fd)
{

	return ((set[fd / NFDBITS] >> (fd % NFDBITS)) & 1u);
}

static long
now_ms(void)
{
	struct timeval	tv;

	tv.tv_sec  = 0;
	tv.tv_usec = 0;
	(void)gettimeofday(&tv, NULL);
	return (tv.tv_sec * 1000 + tv.tv_usec / 1000);
}

/*
 * 9-12: readiness on a pipe.  Run after the terminal is restored, so a
 * failure here still leaves the shell its terminal.
 */
static void
readiness_checks(void)
{
	uint32_t	rset[FD_WORDS];
	uint32_t	wset[FD_WORDS];
	struct timeval	tv;
	struct pollfd	pf;
	long		t0;
	long		t1;
	int		fds[2];
	int		n;
	char		c;

	if (pipe(fds) != 0) {
		fail("cannot make a pipe for the readiness checks");
		return;
	}

	/* 9. asked once: not ready, then ready after a byte. */
	fd_zero(rset);
	fd_set_(rset, fds[0]);
	fd_zero(wset);
	fd_set_(wset, fds[1]);
	tv.tv_sec  = 0;
	tv.tv_usec = 0;
	tv.tv_pad  = 0;
	n = select(fds[1] + 1, rset, wset, NULL, &tv);
	if (n != 1 || fd_isset(rset, fds[0]) || !fd_isset(wset, fds[1]))
		fail("an empty pipe: its read end should not be ready and "
		    "its write end should");
	else if (write(fds[1], "x", 1) != 1)
		fail("cannot write into the pipe");
	else {
		fd_zero(rset);
		fd_set_(rset, fds[0]);
		n = select(fds[0] + 1, rset, NULL, NULL, &tv);
		if (n != 1 || !fd_isset(rset, fds[0]))
			fail("a byte in the pipe did not make its read end "
			    "ready");
		else
			printf("ttyprobe: PASS select, asked once, sees a "
			    "pipe go from empty to readable\n");
	}

	/* 10. a wait the clock has to end. */
	if (read(fds[0], &c, 1) != 1 || c != 'x')
		fail("the byte did not come back out of the pipe");
	fd_zero(rset);
	fd_set_(rset, fds[0]);
	tv.tv_sec  = 0;
	tv.tv_usec = 40000;
	t0 = now_ms();
	n  = select(fds[0] + 1, rset, NULL, NULL, &tv);
	t1 = now_ms();
	if (n != 0)
		fail("an empty pipe was reported ready during a timed wait");
	else if (t1 - t0 < 40)
		fail("select came back before its 40 ms were up");
	else
		printf("ttyprobe: PASS a 40 ms wait on an empty pipe parked "
		    "and was ended by the clock, after %ld ms\n", t1 - t0);

	/* 11. the writer leaves with a byte still inside. */
	if (write(fds[1], "y", 1) != 1)
		fail("cannot write the second byte");
	(void)close(fds[1]);
	pf.fd      = fds[0];
	pf.events  = POLLIN;
	pf.revents = 0;
	n = poll(&pf, 1, 0);
	if (n != 1 || (pf.revents & POLLIN) == 0 ||
	    (pf.revents & POLLHUP) == 0)
		fail("poll did not report both the byte and the writer's "
		    "departure");
	else
		printf("ttyprobe: PASS poll reports POLLIN and POLLHUP on a "
		    "read end whose writer has gone\n");
	(void)close(fds[0]);

	/* 12. a descriptor that is not there. */
	fd_zero(rset);
	fd_set_(rset, fds[0]);
	tv.tv_usec = 0;
	n = select(fds[0] + 1, rset, NULL, NULL, &tv);
	if (n != -1 || *__error() != EBADF)
		fail("select accepted a closed descriptor");
	pf.fd      = fds[0];
	pf.events  = POLLIN;
	pf.revents = 0;
	n = poll(&pf, 1, 0);
	if (n != 1 || pf.revents != POLLNVAL)
		fail("poll did not flag a closed descriptor");
	else
		printf("ttyprobe: PASS a closed descriptor is EBADF to select "
		    "and POLLNVAL to poll\n");
}

int
entry(void)
{
	struct termios	saved;
	struct termios	tio;
	struct winsize	ws;
	char		c;
	long		got;
	int		fds[2];
	int		fd;

	printf("ttyprobe: the terminal, from ring 3\n");

	/* 1. what a terminal nobody has touched says about itself. */
	if (tcgetattr(0, &saved) != 0) {
		printf("ttyprobe: fd 0 is not a terminal here (errno %d) -- "
		    "nothing to probe; skipped\n", *__error());
		exit(0);
	}
	if (!isatty(0))
		fail("tcgetattr answered for something isatty calls not a tty");
	if ((saved.c_lflag & ICANON) == 0 || (saved.c_lflag & ECHO) == 0)
		fail("a fresh terminal is not canonical with echo on");
	else
		printf("ttyprobe: PASS a fresh terminal is canonical, echo on, "
		    "%lu baud in and %lu out\n",
		    cfgetispeed(&saved), cfgetospeed(&saved));

	/* 2. the size, which is the screen's and not a convention. */
	if (ioctl(0, TIOCGWINSZ, &ws) != 0)
		fail("the terminal will not say how big it is");
	else if (ws.ws_row == 0 || ws.ws_col == 0)
		fail("the terminal claims a zero dimension");
	else
		printf("ttyprobe: PASS the window is %u rows by %u columns, "
		    "%u by %u pixels\n", ws.ws_row, ws.ws_col, ws.ws_xpixel,
		    ws.ws_ypixel);

	/* 3. what is not a terminal. */
	fd = open(AFILE, O_RDONLY);
	if (fd < 0)
		printf("ttyprobe: %s is not on this volume, so the file half "
		    "of the ENOTTY check is skipped\n", AFILE);
	else {
		if (tcgetattr(fd, &tio) == 0 || *__error() != ENOTTY)
			fail("a file answered a terminal question");
		else if (isatty(fd))
			fail("isatty calls a file a terminal");
		else
			printf("ttyprobe: PASS a file on the volume answers "
			    "ENOTTY\n");
		(void)close(fd);
	}
	if (pipe(fds) != 0)
		fail("cannot make a pipe");
	else {
		if (tcgetattr(fds[0], &tio) == 0 || *__error() != ENOTTY)
			fail("a pipe answered a terminal question");
		else
			printf("ttyprobe: PASS a pipe answers ENOTTY\n");
		(void)close(fds[0]);
		(void)close(fds[1]);
	}

	/* 4. and a request the terminal does not know. */
	if (ioctl(0, TIOCJUNK, &ws) == 0 || *__error() != ENOTTY)
		fail("an unknown ioctl was not refused");
	else if (ioctl(0, TIOCSWINSZ, &ws) == 0 || *__error() != EINVAL)
		fail("the terminal accepted a size it cannot have");
	else
		printf("ttyprobe: PASS an unknown request and a resize are "
		    "both refused, each in its own way\n");

	/*
	 * 5. Raw.  Set with tcsetattr and confirmed with a fresh tcgetattr,
	 * so it is the kernel that must have kept it, not a copy we sent.
	 */
	tio = saved;
	tio.c_lflag &= ~(ICANON | ECHO | ISIG);
	tio.c_cc[VMIN]  = 0;
	tio.c_cc[VTIME] = 0;
	if (tcsetattr(0, TCSANOW, &tio) != 0) {
		fail("tcsetattr would not take raw mode");
		exit(1);
	}
	tio.c_lflag = 0;			/* forget what we asked for */
	if (tcgetattr(0, &tio) != 0)
		fail("tcgetattr failed after setting raw");
	else if ((tio.c_lflag & ICANON) != 0 || (tio.c_lflag & ECHO) != 0)
		fail("the terminal did not keep the raw setting");
	else if (tio.c_cc[VMIN] != 0)
		fail("the terminal did not keep VMIN");
	else
		printf("ttyprobe: PASS the terminal is raw, and says so when "
		    "asked again\n");

	/* 6. VMIN=0: a read that finds nothing returns instead of waiting. */
	got = read(0, &c, 1);
	if (got != 0)
		fail("a VMIN=0 read did not come back empty");
	else
		printf("ttyprobe: PASS a read with nothing typed returned 0 "
		    "rather than waiting\n");

	/*
	 * 7. The driver fed exactly one byte, no line ending, before
	 * spawning us; a canonical terminal would still be holding it.
	 */
	tio.c_cc[VMIN] = 1;
	if (tcsetattr(0, TCSANOW, &tio) != 0)
		fail("tcsetattr would not take VMIN=1");
	got = read(0, &c, 1);
	if (got != 1)
		fail("no byte arrived in raw mode");
	else
		printf("ttyprobe: PASS one keystroke ('%c') arrived with no "
		    "newline behind it -- canonical mode would still be "
		    "waiting\n", c);

	/* 8. Restore: the next program here is a shell. */
	if (tcsetattr(0, TCSAFLUSH, &saved) != 0)
		fail("tcsetattr would not restore the terminal");
	else if (tcgetattr(0, &tio) != 0)
		fail("tcgetattr failed after the restore");
	else if ((tio.c_lflag & ICANON) == 0 || (tio.c_lflag & ECHO) == 0)
		fail("the terminal did not come back canonical");
	else
		printf("ttyprobe: PASS the terminal is canonical again, with "
		    "echo\n");

	readiness_checks();

	if (fails != 0) {
		printf("ttyprobe: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("ttyprobe: done -- every check passed\n");
	exit(0);
	return (0);
}
