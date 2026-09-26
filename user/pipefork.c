/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * pipefork -- the self-authored Darwin-ABI probe for fork(2), execve(2),
 * wait4(2), pipe(2) and dup2(2): as dirlist did for tree, it proves the
 * process syscalls before a genuine Apple binary (env, timeout, a shell)
 * depends on them.  Bound by our dyld against our libSystem, it imports the
 * same symbols a real binary would.
 *
 * Scenes:
 *	A. fork + wait4: the parent sees the child's exit code exactly.
 *	B. shell redirection: pipe, fork, the child dup2s the write end onto
 *	   stdout and execs /bin/gfactor 42, the parent reads to EOF and
 *	   reaps.  A real Apple binary's stdout crosses a task boundary.
 *	C. SIGPIPE ignored, then at its terminating default.
 *	D. a caught SIGPIPE: the handler runs on the user stack and execution
 *	   resumes.
 *	E. a caught SIGINT raised at a syscall boundary (self-kill).
 *	F. a caught SIGINT delivered asynchronously into a ring-3 loop that
 *	   never enters the kernel, resumed bit-exact.
 *	G. copy-on-write isolation after fork, in both directions.
 *	H. a signal ends a blocked pipe read with EINTR.
 *	I. what waiting costs: reap and pipe-wake latency.
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
 * Ignored, the write fails EPIPE; at the default disposition it kills the
 * writer, and wait4 reports WIFSIGNALED with signal 13.
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
	 * Part 1: SIG_IGN -- the write must not kill us.  libSystem's write()
	 * returns the raw kernel result (no carry-flag fold into -1/errno), so
	 * a broken pipe shows as positive EPIPE.
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
 * Scene D: a caught SIGPIPE.  The kernel runs on_sigpipe on the user stack,
 * sigreturn brings us back, and the write still reports EPIPE.
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
 * Scene E: a caught SIGINT, self-raised with kill(getpid(), SIGINT).  The
 * kernel delivers a self-signal at that kill's own syscall exit, so the
 * handler runs before kill returns.
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
 * Sentinels in %rcx and %r11 across the spin loop.  SYSCALL/SYSRET destroy
 * those two, so only a resume through IRETQ can bring them back.
 */
#define	RCX_SENTINEL	0x1234567890ABCDEFUL
#define	R11_SENTINEL	0x0FEDCBA987654321UL

/*
 * Bound on the spin so a failed delivery ends the scene instead of hanging
 * the boot.  Delivery needs one timer tick; this is thousands.
 */
#define	SPIN_LIMIT	200000000UL

/*
 * Spin on a volatile flag without entering the kernel.  Returns 1 if both
 * sentinels survived whatever broke the loop.
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
 * SIGINT handler and spins in ring 3; the parent kills it.  Only the timer
 * IRQ brings the child into the kernel, so the handler runs only if signals
 * are delivered on the interrupt-return path, and the sentinels survive
 * only if the resume is exact.  A pipe byte orders the kill after the
 * handler is armed.
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
 * Scene G: copy-on-write isolation.  fork(2) gives the child the parent's
 * frames with the write bit cleared, and only the write fault keeps the two
 * from sharing a variable.  The test is that a write on either side stays
 * there:
 *
 *	- The buffer spans several pages; one page would pass even if the
 *	  fault handler resolved the wrong page.
 *	- It is filled before the fork, so every page is present and shared;
 *	  a buffer first touched afterwards would be private anyway.
 *	- The parent writes first.  A kernel that write-protects only the
 *	  child's mapping leaves the parent writing into the shared frame;
 *	  if the child wrote first it would take a private copy and hide
 *	  that.  So the parent rewrites everything while the child still
 *	  shares every page, and the child checks it saw none of it.
 *	- Both directions are checked, with two pipes ordering the steps.
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
		 * Touch nothing until the parent has rewritten its buffer;
		 * every page is still shared until then.
		 */
		(void)read(to_child[0], tok, 1);
		unseen = cow_intact('A');

		cow_fill('B');
		own = cow_intact('B');
		(void)write(to_parent[1], "b", 1);
		_exit((unseen && own) ? 0 : 1);
	}

	/* Parent: overwrite everything before the child faults a page. */
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
 * Scene H: a caught signal ends a pipe read that nothing would satisfy,
 * with EINTR.  A byte on a second pipe sequences the kill after the parent
 * heads into the read.  The child then writes anyway after a long delay, so
 * a kernel that ignores the signal fails the check instead of hanging the
 * boot.
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
		/*
		 * The sync byte says the parent is about to read, not that it
		 * is parked in read(2).  A signal landing before then is
		 * delivered in ring 3 and interrupts nothing, so wait for the
		 * parent to get there.
		 */
		burn(EINTR_DELAY / 4);
		kill(getppid(), EINTR_SIG);
		/*
		 * Long enough for a working kernel to have answered, short
		 * enough that a broken one still ends the scene.
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
	 * Delivery happens on the way out of the syscall, so the handler has
	 * run by now.  Check that the wait ended, and that the signal ended
	 * it rather than the byte.
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

/*
 * Scene I: what waiting costs.  The scenes above check that a wait ends
 * correctly; a parent that polls instead of sleeping passes all of them.
 * This one measures how long the news takes.
 */
struct pf_timeval {
	long	tv_sec;
	int	tv_usec;
	int	tv_pad;
};

extern int	gettimeofday(struct pf_timeval *tv, void *tz);

/* Wall clock in microseconds, or -1 if the clock will not answer. */
static long
pf_now_us(void)
{
	struct pf_timeval	tv;

	if (gettimeofday(&tv, NULL) != 0)
		return (-1);
	return (tv.tv_sec * 1000000L + (long)tv.tv_usec);
}

/*
 * Reap latency: the child stamps the clock and _exits, the parent stamps it
 * again when wait4 returns.  Throughput would not show a polling parent (a
 * yield hands the CPU straight back, so it rarely looks); latency does, as
 * news up to a quantum old -- tens of milliseconds per command at 100 Hz
 * with a five-tick slice.  A parked parent is woken by the exit itself.
 *
 * Each round is timed in two legs.  The wake leg ends when the syscall the
 * parent parked in returns, and the kernel owns it end to end.  The rest is
 * the parent waiting for the child to get further -- in the read-first
 * order, a child that wrote its byte but must run again to reach _exit.
 * That is queueing, scaling with the quantum and the run queue, not wake
 * cost: one 103 ms total held an 89 us pipe wake.
 */
#define	LAT_ROUNDS	5

/*
 * The bound is a hang detector and nothing finer.  The figure includes the
 * parent's wait behind every other runnable thread, which this test does not
 * control, so a tight bound (10 ms) fails on a busy boot with a correct
 * kernel.  The load-independent check lives in the kernel's wait counters:
 * "0 explained by a lost wake".
 */
#define	LAT_MAX_US	1000000L	/* 1 s: only a real hang trips this */

static long
reap_latency_once(int read_first, long *wake_leg)
{
	long	stamp;
	long	after;
	long	first;
	int	fd[2];
	int	status;
	int	pid;

	*wake_leg = -1;
	if (pipe(fd) != 0)
		return (-1);
	pid = fork();
	if (pid == 0) {
		(void)close(fd[0]);
		stamp = pf_now_us();
		(void)write(fd[1], &stamp, sizeof(stamp));
		_exit(0);
	}
	(void)close(fd[1]);
	if (pid < 0) {
		(void)close(fd[0]);
		return (-1);
	}
	/*
	 * The order picks the wake being timed.  Reading first parks on the
	 * pipe and is woken by the child's write; waiting first parks on the
	 * wait channel and is woken by the exit.  The second call finds its
	 * answer already there and does not park.
	 */
	stamp  = -1;
	status = -1;
	if (read_first) {
		if (read(fd[0], &stamp, sizeof(stamp)) !=
		    (long)sizeof(stamp)) {
			(void)close(fd[0]);
			return (-1);
		}
		first = pf_now_us();
		(void)wait4(pid, &status, 0, NULL);
	} else {
		(void)wait4(pid, &status, 0, NULL);
		first = pf_now_us();
		if (read(fd[0], &stamp, sizeof(stamp)) !=
		    (long)sizeof(stamp)) {
			(void)close(fd[0]);
			return (-1);
		}
	}
	after = pf_now_us();
	(void)close(fd[0]);
	if (stamp < 0 || after < stamp)
		return (-1);
	if (first >= stamp)
		*wake_leg = first - stamp;
	return (after - stamp);
}

static void
scene_wait_cost(void)
{
	long	lat;
	long	leg;
	long	best;
	long	worst;
	long	total;
	long	wake_total;
	long	wake_worst;
	int	order;
	int	i;
	int	n;
	int	wake_n;

	printf("[pipefork] --- what waiting costs ---\n");
	if (pf_now_us() < 0) {
		printf("[pipefork] no wall clock -- skipping the reap-latency "
		    "measurement rather than reporting a number it did not "
		    "measure\n");
		return;
	}

	for (order = 0; order < 2; order++) {
		best       = -1;
		worst      = -1;
		total      = 0;
		n          = 0;
		wake_total = 0;
		wake_worst = -1;
		wake_n     = 0;
		for (i = 0; i < LAT_ROUNDS; i++) {
			lat = reap_latency_once(order, &leg);
			if (lat < 0)
				continue;
			if (best < 0 || lat < best)
				best = lat;
			if (lat > worst)
				worst = lat;
			total += lat;
			n++;
			if (leg >= 0) {
				wake_total += leg;
				if (leg > wake_worst)
					wake_worst = leg;
				wake_n++;
			}
		}
		if (n == 0) {
			printf("[pipefork] FAIL no %s round completed\n",
			    order ? "pipe-wake" : "reap");
			failures++;
			continue;
		}
		printf("[pipefork] %s over %d round(s): best %ld us, "
		    "worst %ld us, mean %ld us\n",
		    order ? "pipe-then-reap latency" : "reap latency",
		    n, best, worst, total / n);
		/*
		 * The wake leg alone, the part the kernel decides.  For the
		 * reap order it is nearly the whole figure; for the other it
		 * is the pipe wake, and the rest is the child queueing to
		 * reach _exit.
		 */
		if (wake_n != 0)
			printf("[pipefork]   the wake itself: worst %ld us, "
			    "mean %ld us -- the rest is %s\n",
			    wake_worst, wake_total / wake_n,
			    order ? "the child waiting its turn to exit" :
			    "the trailing read, which never parks");
		if (worst > LAT_MAX_US)
			printf("[pipefork] that is long enough to be a wake "
			    "that never came rather than one that queued\n");
		/*
		 * Only the reap order is judged, and only against the hang
		 * bound: the other order's total adds a queueing cost this
		 * test does not control.  Its wake leg is reported, not
		 * asserted (see LAT_MAX_US).
		 */
		if (!order)
			check(worst <= LAT_MAX_US,
			    "a parent hears about its child when it exits");
	}
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
	scene_wait_cost();
	if (failures == 0)
		printf("[pipefork] ALL TESTS PASSED\n");
	else
		printf("[pipefork] %d FAILURES\n", failures);
	return (failures == 0 ? 0 : 1);
}
