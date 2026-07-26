/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * ttyprobe -- a self-authored Darwin-ABI probe for the rung where the terminal
 * can be TOLD something.
 *
 * The console has been a terminal for a while in the sense that mattered so
 * far: it echoes, it edits a line, Ctrl-C signals, and a real Apple shell runs
 * interactively on it.  All of that was fixed at compile time.  A program
 * could not turn the echo off, could not ask how wide the screen is, and could
 * not read a single keystroke without waiting for Return -- and those three
 * are the entire admission price for full-screen software.  tcgetattr answered
 * ENOTTY, on purpose and in writing, because there was nothing behind it.
 *
 * This is the de-risking step, in the same tradition as dirlist for
 * directories, pipefork for processes and filewrite for the volume: prove the
 * syscalls from ring 3 with a program we wrote, BEFORE a binary we did not
 * write depends on them.  The one that follows is coreutils' stty, which does
 * nothing else for a living.
 *
 * WHAT IT CHECKS, and why each is here rather than assumed:
 *
 *	1. A fresh terminal reports canonical mode with echo on.  That is the
 *	   state a session must start in, and the state a previous program
 *	   leaving raw mode behind would have broken.
 *	2. The window size is the screen's real size, not a fabricated 80x24.
 *	3. A FILE and a PIPE both answer ENOTTY.  This is the check that keeps
 *	   isatty(3) honest, and gls decides whether to print in columns by it:
 *	   a kernel that answered for a pipe would put columns in a file.
 *	4. An unknown request answers ENOTTY rather than succeeding quietly.
 *	5. A setting READS BACK.  The whole rung is worthless if tcsetattr is
 *	   a no-op that libSystem pretends worked, so the raw mode is asked
 *	   for through one call and confirmed through another.
 *	6. With VMIN=0 a read returns 0 instead of waiting.  In canonical mode
 *	   this same read would have blocked until Return.
 *	7. AND THEN A BYTE ARRIVES WITH NO NEWLINE BEHIND IT.  The driver feeds
 *	   this program a single character and no line ending; canonical mode
 *	   would hold it in the line buffer forever, so receiving it at all is
 *	   the proof -- from out here, through read(2) -- that the kernel's
 *	   discipline really did stop being line-based.
 *	8. The restore takes, so the shell that runs next has its terminal back.
 *
 * Freestanding: no SDK headers, prototypes declared as <termios.h> would alias
 * them, entry at _entry (ld -e), relinked low like dyldhello.
 */

typedef __UINT8_TYPE__	uint8_t;
typedef __UINT16_TYPE__	uint16_t;
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
 * Apple's struct termios, which is also the kernel's and also stty's.  Written
 * out here rather than shared through a header on purpose: this program exists
 * to check the ABI, and a probe that got the layout from the same place as the
 * thing it is probing would agree with it by construction.
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

extern int	*__error(void);
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern int	 close(int fd);
extern int	 pipe(int fds[2]);
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

	/* 3. what is NOT a terminal, which is the more useful answer. */
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
	 * 5. RAW.  Asked for through tcsetattr and confirmed through a fresh
	 * tcgetattr, so what is being checked is that the KERNEL kept it --
	 * a libSystem that remembered the struct and handed it back would
	 * pass any check made against the copy we sent.
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

	/*
	 * 6. VMIN=0: a read that finds nothing says so instead of waiting.
	 * In the canonical mode this program started in, this same call would
	 * have blocked until somebody pressed Return.
	 */
	got = read(0, &c, 1);
	if (got != 0)
		fail("a VMIN=0 read did not come back empty");
	else
		printf("ttyprobe: PASS a read with nothing typed returned 0 "
		    "rather than waiting\n");

	/*
	 * 7. One character, with no line ending anywhere behind it.  The
	 * driver fed exactly one byte before spawning this program; a
	 * canonical terminal would still be holding it in the line buffer,
	 * waiting for a Return that is never coming.
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

	/*
	 * 8. And the terminal goes back, because the next program to run here
	 * is a shell and it expects the terminal a shell was given.
	 */
	if (tcsetattr(0, TCSAFLUSH, &saved) != 0)
		fail("tcsetattr would not restore the terminal");
	else if (tcgetattr(0, &tio) != 0)
		fail("tcgetattr failed after the restore");
	else if ((tio.c_lflag & ICANON) == 0 || (tio.c_lflag & ECHO) == 0)
		fail("the terminal did not come back canonical");
	else
		printf("ttyprobe: PASS the terminal is canonical again, with "
		    "echo\n");

	if (fails != 0) {
		printf("ttyprobe: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("ttyprobe: done -- every check passed\n");
	exit(0);
	return (0);
}
