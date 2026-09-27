/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * lockprobe -- POSIX record locks between processes, as sqlite3 takes
 * them: fcntl F_GETLK, F_SETLK and F_SETLKW.  A self-authored Darwin-ABI
 * probe like filewrite, bound by our dyld against our libSystem.
 *
 *	1. With nothing held, F_GETLK answers F_UNLCK.
 *	2. A process's own locks never conflict, and a read lock inside its
 *	   own write lock splits it rather than stacking on it.
 *	3. Another process's read lock inside a write lock is refused with
 *	   EAGAIN, and F_GETLK names the holder: its type, range and pid.
 *	4. A lock beside the held range is granted.
 *	5. F_SETLKW sleeps until the holder closes the file -- through a
 *	   descriptor other than the one it locked with, which still drops
 *	   every lock it had there -- and then takes the lock.
 *	6. Exit drops what a process held.
 *	7. Unlocking the middle of a range leaves both ends locked.
 *
 * Children print their own checks and exit with their failure count.
 * Freestanding: prototypes declared here, entry at _entry (ld -e).
 */

typedef __INT16_TYPE__	int16_t;
typedef __INT32_TYPE__	int32_t;
typedef __INT64_TYPE__	int64_t;

#define	NULL		((void *)0)

#define	O_RDWR		0x0002
#define	O_CREAT		0x0200
#define	O_TRUNC		0x0400

#define	F_GETLK		7
#define	F_SETLK		8
#define	F_SETLKW	9
#define	F_RDLCK		1
#define	F_UNLCK		2
#define	F_WRLCK		3

#define	EAGAIN		35

#define	PATH		"/etc/lockprobe.dat"

struct flock {
	int64_t		l_start;
	int64_t		l_len;
	int32_t		l_pid;
	int16_t		l_type;
	int16_t		l_whence;
};

struct timeval {
	long		tv_sec;
	int32_t		tv_usec;
	int32_t		tv_pad;
};

extern int	*__error(void);
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern long	 write(int fd, const void *buf, unsigned long n);
extern int	 close(int fd);
extern int	 fcntl(int fd, int cmd, ...);
extern int	 fork(void);
extern int	 getpid(void);
extern int	 pipe(int fds[2]);
extern int	 waitpid(int pid, int *status, int options);
extern int	 select(int n, void *r, void *w, void *e, struct timeval *tv);
extern int	 unlink(const char *path);
extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);

static const char	*who = "lockprobe";
static int		 fails;

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

static int
lk(int fd, int cmd, int type, long start, long len)
{
	struct flock	fl;

	fl.l_start  = start;
	fl.l_len    = len;
	fl.l_pid    = 0;
	fl.l_type   = (int16_t)type;
	fl.l_whence = 0;
	return (fcntl(fd, cmd, &fl));
}

/* F_GETLK for a write lock on [start, start + len): the answer's type. */
static int
held(int fd, long start, long len, struct flock *fl)
{

	fl->l_start  = start;
	fl->l_len    = len;
	fl->l_pid    = 0;
	fl->l_type   = F_WRLCK;
	fl->l_whence = 0;
	if (fcntl(fd, F_GETLK, fl) != 0)
		return (-1);
	return (fl->l_type);
}

static void
nap(int ms)
{
	struct timeval	tv;

	tv.tv_sec  = 0;
	tv.tv_usec = ms * 1000;
	tv.tv_pad  = 0;
	(void)select(0, NULL, NULL, NULL, &tv);
}

/* Reap a child: did it exit 0, every check passed? */
static int
child_ok(int pid)
{
	int	status;

	if (waitpid(pid, &status, 0) != pid)
		return (0);
	return (status == 0);
}

/* Steps 3 to 5, in the child. */
static void
contend(int ppid, int tell)
{
	struct flock	fl;
	int		fd;

	who = "lockprobe[child]";
	fd = open(PATH, O_RDWR);
	if (fd < 0) {
		fail("could not open the file");
		exit(fails);
	}
	if (lk(fd, F_SETLK, F_RDLCK, 5, 1) != -1 || *__error() != EAGAIN)
		fail("a read lock inside another's write lock was not "
		    "refused with EAGAIN");
	else
		pass("a read lock inside the parent's write lock is EAGAIN");

	if (held(fd, 0, 0, &fl) != F_WRLCK || fl.l_start != 0 ||
	    fl.l_len != 10 || fl.l_pid != ppid)
		fail("F_GETLK did not name the holder (write, 0+10, the "
		    "parent's pid)");
	else
		pass("F_GETLK names the holder -- write lock, 0+10, the "
		    "parent's pid");

	if (lk(fd, F_SETLK, F_WRLCK, 20, 5) != 0)
		fail("a lock beside the held range was refused");
	else
		pass("a lock beside the held range is granted");

	/* Say so, then wait for the parent's lock. */
	(void)write(tell, "w", 1);
	if (lk(fd, F_SETLKW, F_WRLCK, 0, 1) != 0)
		fail("F_SETLKW did not get the lock once the holder let go");
	else
		pass("F_SETLKW slept until the parent closed the file, "
		    "then took the lock");
	exit(fails);
}

/* Step 7, in a second child. */
static void
hole(void)
{
	int	fd;

	who = "lockprobe[child]";
	fd = open(PATH, O_RDWR);
	if (fd < 0) {
		fail("could not open the file");
		exit(fails);
	}
	if (lk(fd, F_SETLK, F_WRLCK, 45, 1) != 0)
		fail("the unlocked middle was not free");
	else if (lk(fd, F_SETLK, F_WRLCK, 39, 1) != -1 ||
	    lk(fd, F_SETLK, F_WRLCK, 60, 1) != -1)
		fail("an end of the split range was free");
	else
		pass("unlocking the middle of a range left both ends locked");
	exit(fails);
}

int
entry(void)
{
	struct flock	fl;
	char		c;
	int		pfd[2];
	int		fd;
	int		fd2;
	int		pid;
	int		ppid;

	printf("lockprobe: record locks between processes\n");
	fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0) {
		fail("could not create the file");
		goto out;
	}
	(void)write(fd, "0123456789abcdefghij", 20);
	ppid = getpid();

	if (held(fd, 0, 0, &fl) != F_UNLCK)
		fail("F_GETLK with nothing held did not answer F_UNLCK");
	else
		pass("nothing held -- F_GETLK answers F_UNLCK");

	if (lk(fd, F_SETLK, F_WRLCK, 0, 10) != 0 ||
	    lk(fd, F_SETLK, F_WRLCK, 0, 10) != 0 ||
	    lk(fd, F_SETLK, F_RDLCK, 2, 3) != 0 ||
	    lk(fd, F_SETLK, F_WRLCK, 0, 10) != 0)
		fail("a process's own locks conflicted");
	else
		pass("own locks never conflict -- a write lock twice, a read "
		    "lock inside it, the write lock again");

	if (pipe(pfd) != 0) {
		fail("pipe");
		goto out;
	}
	pid = fork();
	if (pid == 0) {
		(void)close(pfd[0]);
		contend(ppid, pfd[1]);
	}
	(void)close(pfd[1]);

	/*
	 * Once the child is about to wait, close a second descriptor on the
	 * file: that drops every lock this process holds there.
	 */
	fd2 = open(PATH, O_RDWR);
	if (read(pfd[0], &c, 1) != 1)
		fail("the child never reached F_SETLKW");
	nap(30);
	(void)close(fd2);
	if (!child_ok(pid))
		fail("the child's checks failed");

	if (held(fd, 20, 5, &fl) != F_UNLCK || held(fd, 0, 1, &fl) != F_UNLCK)
		fail("a dead process's lock is still held");
	else
		pass("exit dropped the child's locks");

	if (lk(fd, F_SETLK, F_WRLCK, 0, 100) != 0 ||
	    lk(fd, F_SETLK, F_UNLCK, 40, 20) != 0)
		fail("could not lock 0+100 and unlock 40+20");
	pid = fork();
	if (pid == 0)
		hole();
	if (!child_ok(pid))
		fail("the second child's checks failed");

	(void)close(fd);
	(void)unlink(PATH);
out:
	if (fails != 0) {
		printf("lockprobe: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("lockprobe: done -- every check passed\n");
	exit(0);
	return (0);
}
