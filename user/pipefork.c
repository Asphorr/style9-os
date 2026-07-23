/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * pipefork -- the self-authored Darwin-ABI probe for the multi-process rung:
 * fork(2), execve(2), wait4(2), pipe(2), dup2(2).  Like dirlist before tree,
 * it is the "small probe built with the real toolchain" that de-risks the
 * process syscalls before a genuine Apple binary (env, timeout, a shell)
 * depends on them.  Bound by our dyld against our libSystem, it imports the
 * same symbols a real binary would.
 *
 * Two scenes:
 *	A. bare fork + wait4: the child _exits with a known code; the parent
 *	   must see exactly that code in the wait4 status.
 *	B. the full shell-redirection dance: pipe, fork, the child dup2s the
 *	   write end onto stdout and execve's /bin/gfactor 42, the parent
 *	   captures the pipe until EOF and reaps the child.  What flows
 *	   through the pipe is a REAL Apple binary's stdout crossing a task
 *	   boundary through a kernel pipe.
 *
 * Freestanding (-fno-builtin, no SDK headers); entry is _entry (ld -e), no
 * crt; relinked low like dyldhello.
 */

#define	NULL	((void *)0)

typedef unsigned long	size_t;

extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);
extern void	 _exit(int code);
extern int	 fork(void);
extern int	 execve(const char *path, char *const argv[],
		    char *const envp[]);
extern int	 wait4(int pid, int *status, int options, void *rusage);
extern int	 pipe(int fds[2]);
extern int	 dup2(int oldfd, int newfd);
extern int	 close(int fd);
extern long	 read(int fd, void *buf, unsigned long n);
extern int	 getpid(void);
extern int	 getppid(void);
extern int	*__error(void);
extern long	 write(int fd, const void *buf, unsigned long n);
extern void	*signal(int sig, void *handler);

#define	SIG_DFL	((void *)0)
#define	SIG_IGN	((void *)1)
#define	SIGPIPE	13
#define	EPIPE	32

static int	failures;

static void
check(int ok, const char *what)
{

	if (ok)
		printf("[pipefork] ok: %s\n", what);
	else {
		printf("[pipefork] FAIL: %s\n", what);
		failures++;
	}
}

/* Scene A: bare fork + wait4 round trip. */
static void
scene_fork_wait(void)
{
	int	pid;
	int	rpid;
	int	status;

	pid = fork();
	if (pid < 0) {
		check(0, "fork (errno set)");
		return;
	}
	if (pid == 0) {
		/* Child: prove identity calls work, then exit 7. */
		if (getppid() <= 0)
			_exit(99);
		_exit(7);
	}
	status = -1;
	rpid = wait4(pid, &status, 0, NULL);
	check(rpid == pid, "wait4 returns the forked pid");
	check(status == (7 << 8), "wait4 status carries exit(7)");
}

/* Scene B: pipe + fork + dup2 + execve gfactor + capture + reap. */
static void
scene_pipeline(void)
{
	static char	 capture[512];
	char		*argv_child[3];
	long		 n;
	int		 fds[2];
	int		 got;
	int		 pid;
	int		 rpid;
	int		 status;

	if (pipe(fds) != 0) {
		check(0, "pipe");
		return;
	}
	printf("[pipefork] pipe: r=%d w=%d\n", fds[0], fds[1]);

	pid = fork();
	if (pid < 0) {
		check(0, "fork for pipeline");
		return;
	}
	if (pid == 0) {
		/*
		 * Child: stdout -> pipe write end, drop both pipe fds,
		 * become gfactor.  Reaching the _exit(127) line means the
		 * exec failed.
		 */
		if (dup2(fds[1], 1) != 1)
			_exit(126);
		close(fds[0]);
		close(fds[1]);
		argv_child[0] = "gfactor";
		argv_child[1] = "42";
		argv_child[2] = NULL;
		execve("/bin/gfactor", argv_child, NULL);
		_exit(127);
	}

	close(fds[1]);		/* parent keeps only the read end */
	got = 0;
	for (;;) {
		n = read(fds[0], capture + got,
		    (unsigned long)(sizeof(capture) - 1 - (size_t)got));
		if (n <= 0)
			break;
		got += (int)n;
		if ((size_t)got >= sizeof(capture) - 1)
			break;
	}
	capture[got] = '\0';
	close(fds[0]);

	status = -1;
	rpid = wait4(pid, &status, 0, NULL);

	printf("[pipefork] captured %d bytes through the pipe: %s",
	    got, capture);
	check(got > 0, "child stdout arrived through the pipe");
	check(capture[0] == '4' && capture[1] == '2' && capture[2] == ':',
	    "capture starts with '42:'");
	check(rpid == pid, "wait4 reaps the exec'd child");
	check(status == 0, "gfactor exited 0");
}

/*
 * Scene C: SIGPIPE.  A write to a pipe with no readers posts SIGPIPE.
 * Ignored, the write fails EPIPE and we run on; at the default disposition
 * it terminates the writer, which the parent reads out of wait4 as a
 * WIFSIGNALED status carrying signal 13.
 */
static void
scene_sigpipe(void)
{
	long	n;
	int	fds[2];
	int	pid;
	int	rpid;
	int	status;
	char	byte;

	/*
	 * Part 1: SIG_IGN -- the broken-pipe write must not terminate us.
	 * libSystem's write() returns the raw kernel result (it does not fold
	 * the carry flag into -1/errno), so a broken pipe surfaces as the
	 * positive EPIPE code rather than the 1 a good write returns; reaching
	 * the check at all proves the ignored SIGPIPE let the writer live.
	 */
	signal(SIGPIPE, SIG_IGN);
	if (pipe(fds) != 0) {
		check(0, "pipe for sigpipe(ign)");
		return;
	}
	close(fds[0]);			/* no readers */
	byte = 'x';
	n = write(fds[1], &byte, 1);
	check(n == EPIPE, "ignored SIGPIPE: broken-pipe write -> EPIPE, writer lives");
	close(fds[1]);

	/* Part 2: SIG_DFL -- the child writer is terminated by SIGPIPE. */
	signal(SIGPIPE, SIG_DFL);
	if (pipe(fds) != 0) {
		check(0, "pipe for sigpipe(dfl)");
		return;
	}
	close(fds[0]);			/* readers 0 BEFORE fork: deterministic */
	pid = fork();
	if (pid < 0) {
		check(0, "fork for sigpipe");
		return;
	}
	if (pid == 0) {
		byte = 'x';
		(void)write(fds[1], &byte, 1);	/* reader-less -> SIGPIPE */
		_exit(55);			/* NOTREACHED if SIGPIPE fires */
	}
	close(fds[1]);
	status = -1;
	rpid = wait4(pid, &status, 0, NULL);
	check(rpid == pid, "wait4 reaps the sigpipe'd child");
	check((status & 0x7f) == 13, "child terminated by SIGPIPE (13)");
}

int
entry(void)
{

	printf("[pipefork] pid=%d ppid=%d\n", getpid(), getppid());
	scene_fork_wait();
	scene_pipeline();
	scene_sigpipe();
	if (failures == 0)
		printf("[pipefork] ALL TESTS PASSED\n");
	else
		printf("[pipefork] %d FAILURES\n", failures);
	return (failures == 0 ? 0 : 1);
}
