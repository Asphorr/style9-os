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
 * Scenes:
 *	A. bare fork + wait4: the child _exits with a known code; the parent
 *	   must see exactly that code in the wait4 status.
 *	B. the full shell-redirection dance: pipe, fork, the child dup2s the
 *	   write end onto stdout and execve's /bin/gfactor 42, the parent
 *	   captures the pipe until EOF and reaps the child.  What flows
 *	   through the pipe is a REAL Apple binary's stdout crossing a task
 *	   boundary through a kernel pipe.
 *	C. SIGPIPE at its two default dispositions: ignored, and terminating.
 *	D. a CAUGHT SIGPIPE: the handler runs on the user stack and execution
 *	   resumes at the interrupted instruction.
 *	E. a caught SIGINT raised at a syscall boundary (self-kill).
 *	F. a caught SIGINT delivered ASYNCHRONOUSLY, into a ring-3 compute
 *	   loop that never enters the kernel, and resumed bit-exact.
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
extern int	 kill(int pid, int sig);

#define	SIG_DFL	((void *)0)
#define	SIG_IGN	((void *)1)
#define	SIGINT	2
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

static volatile int	sigpipe_caught;

static void
on_sigpipe(int signo)
{

	sigpipe_caught = signo;
}

/*
 * Scene D: a CAUGHT SIGPIPE runs its ring-3 handler and execution resumes.
 * The write to a reader-less pipe posts SIGPIPE; the kernel delivers it to
 * on_sigpipe on the user stack; sigreturn brings us back so the write still
 * reports EPIPE and the program runs on.
 */
static void
scene_sigpipe_handler(void)
{
	long	n;
	int	fds[2];

	sigpipe_caught = 0;
	signal(SIGPIPE, (void *)on_sigpipe);
	if (pipe(fds) != 0) {
		check(0, "pipe for sigpipe(handler)");
		return;
	}
	close(fds[0]);			/* no readers */
	n = write(fds[1], "x", 1);	/* broken pipe -> SIGPIPE -> on_sigpipe */
	check(sigpipe_caught == 13, "caught SIGPIPE ran its handler");
	check(n == EPIPE, "execution resumed after handler (write -> EPIPE)");
	close(fds[1]);
	signal(SIGPIPE, SIG_DFL);
}

static volatile int	sigint_caught;

static void
on_sigint(int signo)
{

	sigint_caught = signo;
}

/*
 * Scene E: a caught SIGINT delivered to a handler.  Self-raised via
 * kill(getpid(), SIGINT) -- the kernel applies a self-signal at that kill's
 * own syscall exit, so the handler runs and kill returns normally.  Same
 * delivery path a Ctrl-C (SIGINT from the console) takes.
 */
static void
scene_sigint_handler(void)
{

	sigint_caught = 0;
	signal(SIGINT, (void *)on_sigint);
	kill(getpid(), SIGINT);
	check(sigint_caught == 2, "caught SIGINT (self-kill) ran its handler");
	signal(SIGINT, SIG_DFL);
}

static volatile int	spin_sig;

static void
on_sigint_spin(int signo)
{

	spin_sig = signo;
}

/*
 * Sentinels parked in %rcx and %r11 across the spin loop.  Those two are the
 * registers the SYSCALL instruction destroys, so a signal resumed by SYSRET
 * structurally cannot bring them back; only the IRETQ return path can.
 */
#define	RCX_SENTINEL	0x1234567890ABCDEFUL
#define	R11_SENTINEL	0x0FEDCBA987654321UL

/*
 * Bound on the spin so a failure to deliver ends the scene instead of hanging
 * the boot.  Delivery needs one timer tick; this is many thousands of them.
 */
#define	SPIN_LIMIT	200000000UL

/*
 * Spin on a volatile flag, touching nothing but registers and one memory
 * word -- no syscall, no library call, nothing that would enter the kernel
 * voluntarily.  Returns 1 if both sentinels survived whatever broke the loop.
 */
static int
spin_until_signal(unsigned long limit)
{
	register unsigned long	rcx __asm__("rcx");
	register unsigned long	r11 __asm__("r11");

	rcx = RCX_SENTINEL;
	r11 = R11_SENTINEL;
	__asm__ __volatile__ (
	    "1:	cmpl	$0, %3		\n\t"
	    "	jne	2f		\n\t"
	    "	decq	%2		\n\t"
	    "	jnz	1b		\n\t"
	    "2:				\n\t"
	    : "+r" (rcx), "+r" (r11), "+r" (limit)
	    : "m" (spin_sig)
	    : "cc");
	return (rcx == RCX_SENTINEL && r11 == R11_SENTINEL);
}

/*
 * Scene F: asynchronous delivery into a pure compute loop.  The child arms a
 * SIGINT handler and then spins in ring 3 without ever entering the kernel;
 * the parent, a separate task, kills it.  Only the timer IRQ brings the child
 * in, so the handler can run at all only if the kernel delivers signals off
 * the interrupt-return path -- and the resume has to be exact enough that the
 * sentinels come back intact.  A pipe byte sequences the kill after the
 * handler is armed, so the scene is deterministic rather than racy.
 */
static void
scene_async_sigint(void)
{
	int	fds[2];
	int	pid;
	int	rpid;
	int	status;
	char	byte;

	if (pipe(fds) != 0) {
		check(0, "pipe for async sigint");
		return;
	}
	pid = fork();
	if (pid < 0) {
		check(0, "fork for async sigint");
		return;
	}
	if (pid == 0) {
		close(fds[0]);
		spin_sig = 0;
		signal(SIGINT, (void *)on_sigint_spin);
		byte = 'r';
		(void)write(fds[1], &byte, 1);	/* handler is armed */
		if (!spin_until_signal(SPIN_LIMIT))
			_exit(46);		/* resumed with a wrong %rcx/%r11 */
		if (spin_sig != SIGINT)
			_exit(45);		/* loop ran out: never delivered  */
		_exit(44);
	}
	close(fds[1]);
	byte = 0;
	(void)read(fds[0], &byte, 1);		/* wait for "armed" */
	close(fds[0]);
	kill(pid, SIGINT);

	status = -1;
	rpid = wait4(pid, &status, 0, NULL);
	printf("[pipefork] async child status=0x%x (44=ok 45=undelivered "
	    "46=bad context)\n", status);
	check(rpid == pid, "wait4 reaps the spinning child");
	check(status == (44 << 8),
	    "SIGINT reached a handler inside a pure ring-3 compute loop");
}

/*
 * Scene G: copy-on-write isolation.
 *
 * fork(2) no longer copies the parent's pages; it hands the child the same
 * frames with the write bit cleared, and the ONLY thing between that and two
 * processes quietly sharing one variable is the kernel's write fault.  So the
 * thing to test is not that fork works -- every scene above already needs
 * that -- but that a write on either side stays on that side.
 *
 * Three things make this a real test rather than a shape of one:
 *
 *	- The buffer spans several pages.  A one-page version passes even if
 *	  the fault handler resolves the wrong page, which is the mistake most
 *	  worth catching.
 *
 *	- It is filled BEFORE the fork, so every page of it is genuinely
 *	  present and genuinely shared at that moment.  A buffer first touched
 *	  afterwards would be faulted in privately and prove nothing.
 *
 *	- The PARENT writes first, and the order is the whole point.  The
 *	  half-fix worth catching is a kernel that clears the write bit only
 *	  in the child and leaves the parent's page table writable over the
 *	  shared frame.  Let the child write first and that bug hides: the
 *	  child faults, takes its private copy, and the parent's later store
 *	  has nothing left to corrupt.  So the parent overwrites its whole
 *	  buffer while the child is still blocked and still sharing every
 *	  page of it, and the child then checks that it saw none of it.
 *
 *	- Both directions are checked, with two pipes as the clock: neither
 *	  process looks at its buffer until the other has certainly written.
 */
#define	COW_PAGES	3
#define	COW_BYTES	(COW_PAGES * 4096)

static char	cow_buf[COW_BYTES];

static void
cow_fill(int v)
{
	int	i;

	for (i = 0; i < COW_BYTES; i++)
		cow_buf[i] = (char)(v + (i & 0x0F));
}

static int
cow_intact(int v)
{
	int	i;

	for (i = 0; i < COW_BYTES; i++)
		if (cow_buf[i] != (char)(v + (i & 0x0F)))
			return (0);
	return (1);
}

static void
scene_cow(void)
{
	char	tok[1];
	int	to_child[2];
	int	to_parent[2];
	int	pid;
	int	rpid;
	int	status;

	if (pipe(to_child) != 0 || pipe(to_parent) != 0) {
		check(0, "pipes for the copy-on-write scene");
		return;
	}

	cow_fill('A');

	pid = fork();
	if (pid < 0) {
		check(0, "fork for the copy-on-write scene");
		return;
	}
	if (pid == 0) {
		int	unseen;
		int	own;

		/*
		 * Touch nothing until the parent says it has rewritten its
		 * whole buffer.  Every page here is still shared at this
		 * instant, which is exactly the state the check needs.
		 */
		(void)read(to_child[0], tok, 1);
		unseen = cow_intact('A');

		cow_fill('B');
		own = cow_intact('B');
		(void)write(to_parent[1], "b", 1);
		_exit((unseen && own) ? 0 : 1);
	}

	/*
	 * Parent: overwrite everything before the child has had a chance to
	 * fault a single page of it, then let the child look.
	 */
	cow_fill('C');
	(void)write(to_child[1], "c", 1);

	(void)read(to_parent[0], tok, 1);
	check(cow_intact('C'), "the child's writes stayed out of the parent");

	status = -1;
	rpid = wait4(pid, &status, 0, NULL);
	check(rpid == pid, "wait4 reaps the copy-on-write child");
	check(status == 0, "the parent's writes stayed out of the child");

	(void)close(to_child[0]);
	(void)close(to_child[1]);
	(void)close(to_parent[0]);
	(void)close(to_parent[1]);
}

/*
 * Scene H: a signal interrupts a wait.
 *
 * A read on a pipe nobody is writing to waits, and until this scene existed
 * the only thing that could end that wait was data, end-of-file, or the task
 * being killed outright.  A signal -- SIGINT, SIGTERM, anything a program
 * would normally catch -- sat pending and did nothing.  Three of this
 * personality's four waits were like that; the console read was the only one
 * that asked.
 *
 * The child arms the sequence with a byte on a second pipe, so the parent is
 * already inside the read before the kill is sent.  Then, and this is the part
 * that matters for a test that lives in a boot, the child WRITES ANYWAY after
 * a long ring-3 delay: a kernel that ignores the signal finishes the read and
 * fails this check, instead of hanging the boot and telling nobody why.
 */
#define	EINTR_DELAY	60000000UL
#define	EINTR_SIG	SIGINT
#define	EINTR_ERRNO	4

static volatile int	eintr_caught;

static void
on_sigint_eintr(int sig)
{

	eintr_caught = sig;
}

static void
burn(unsigned long n)
{
	volatile unsigned long	i;

	for (i = 0; i < n; i++)
		continue;
}

static void
scene_signal_interrupts_read(void)
{
	int	data[2];
	int	sync[2];
	int	pid;
	int	rpid;
	int	status;
	char	byte;
	long	n;

	if (pipe(data) != 0) {
		check(0, "pipe for the interrupted read");
		return;
	}
	if (pipe(sync) != 0) {
		check(0, "sync pipe for the interrupted read");
		(void)close(data[0]);
		(void)close(data[1]);
		return;
	}

	eintr_caught = 0;
	signal(EINTR_SIG, (void *)on_sigint_eintr);

	pid = fork();
	if (pid < 0) {
		check(0, "fork for the interrupted read");
		return;
	}
	if (pid == 0) {
		(void)close(data[0]);
		(void)close(sync[1]);
		byte = 0;
		(void)read(sync[0], &byte, 1);	/* parent is about to block */
		kill(getppid(), EINTR_SIG);
		/*
		 * Long enough that a kernel which honours the signal answered
		 * many times over; short enough that one which does not still
		 * ends the scene with a verdict.
		 */
		burn(EINTR_DELAY);
		byte = 'x';
		(void)write(data[1], &byte, 1);
		_exit(0);
	}

	(void)close(data[1]);
	(void)close(sync[0]);
	byte = 'g';
	(void)write(sync[1], &byte, 1);

	byte = 0;
	n = read(data[0], &byte, 1);
	printf("[pipefork] interrupted read: n=%ld errno=%d caught=%d\n",
	    n, *__error(), eintr_caught);

	/*
	 * Delivery happens on the way out of the syscall, so by the time the
	 * return value is readable here the handler has already run.  Both
	 * halves are checked: the wait ended, AND it ended because of the
	 * signal rather than because the byte turned up.
	 */
	check(n < 0, "a signal ends a read that nothing was going to satisfy");
	if (n < 0) {
		if (*__error() != EINTR_ERRNO)
			printf("[pipefork] errno was %d, expected %d\n",
			    *__error(), EINTR_ERRNO);
		check(*__error() == EINTR_ERRNO, "and it ends it with EINTR");
	} else
		printf("[pipefork] read returned %ld ('%c') -- the signal was "
		    "ignored and the write finished the wait\n", n, byte);
	check(eintr_caught == EINTR_SIG, "the handler ran");

	signal(EINTR_SIG, SIG_DFL);
	status = -1;
	rpid = wait4(pid, &status, 0, NULL);
	printf("[pipefork] reap: rpid=%d (want %d) status=0x%x errno=%d\n",
	    rpid, pid, status, *__error());
	check(rpid == pid, "wait4 reaps the signalling child");

	(void)close(data[0]);
	(void)close(sync[1]);
}

int
entry(void)
{

	printf("[pipefork] pid=%d ppid=%d\n", getpid(), getppid());
	scene_fork_wait();
	scene_pipeline();
	scene_sigpipe();
	scene_sigpipe_handler();
	scene_sigint_handler();
	scene_async_sigint();
	scene_cow();
	scene_signal_interrupts_read();
	if (failures == 0)
		printf("[pipefork] ALL TESTS PASSED\n");
	else
		printf("[pipefork] %d FAILURES\n", failures);
	return (failures == 0 ? 0 : 1);
}
