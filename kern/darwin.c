/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "darwin.h"
#include "fs.h"
#include "gdt.h"
#include "host.h"
#include "intr.h"
#include "kmem.h"
#include "kprintf.h"
#include "macho.h"
#include "panic.h"
#include "pmap.h"
#include "port.h"
#include "progreg.h"
#include "sched.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"
#include "tty.h"
#include "vm.h"
#include "vm_object.h"

/*
 * Darwin (XNU) syscall personality dispatcher -- S2 of the Mach-O ladder.
 * syscall_dispatch routes a TASK_PERSONALITY_DARWIN task here; we decode the
 * Apple class/number out of %rax and translate each onto the style9 primitive
 * that already implements it.  The interesting part is not the translation
 * but the ABI boundary: how the caller passes arguments (already aligned with
 * style9 -- rdi/rsi/rdx/r10/r8/r9) and how it reads results.
 *
 * Return convention, the crux of S2:
 *	- Unix/BSD (class 2): libSystem reads the carry flag.  CF clear means
 *	  %rax is the result; CF set means %rax is a positive errno.  style9
 *	  has no errno (it speaks MACH_E_* and ELF_E_* codes), so the error path
 *	  maps an internal failure onto a Darwin errno and sets carry.
 *	- Mach (class 1): the trap returns a port name or kern_return_t in %rax
 *	  with no carry convention; we clear carry and return the value.
 *
 * The carry flag lives in the saved user RFLAGS the entry stub sysrets with
 * (syscall_entry.S restores %r11 from sf_user_rflags), so every return funnels
 * through darwin_ok()/darwin_err() to set it deterministically -- a Darwin
 * syscall never inherits stale carry from the thread's last user instruction.
 */

#define	RFLAGS_CF	(1u << 0)

/*
 * The synthetic /bin: where darwin_bin_lookup (below) presents the program
 * registry as a directory.  Inode numbers are synthesized well clear of
 * the FAT volume's cluster-derived ones.
 */
#define	DARWIN_BIN_DIR		"/bin"
#define	DARWIN_BIN_INO_BASE	0xB1000000u

/*
 * Darwin console input -- a terminal, not a mailbox.
 *
 * The DARWIN_OF_CONSOLE / implicit-stdin read path drains this; producers
 * push into it one character at a time through darwin_cons_input(), which is
 * the LINE DISCIPLINE: it echoes, it lets a typo be erased, it turns Ctrl-C
 * into a signal and Ctrl-D into end-of-file, and it hands the reader only
 * completed lines.  That division is the point.  A terminal in canonical
 * mode is not a byte pipe with a read() on the end; the editing and the
 * signals happen at the moment a key ARRIVES, whether or not anybody is
 * currently reading, and a reader that tried to do them would echo a
 * character only once it got round to consuming it.
 *
 * There are two producers and they are not alike.  The keyboard driver
 * thread routes live keystrokes here whenever a Darwin task has claimed the
 * console (see darwin_cons_sink), which is what makes an interactive shell
 * interactive.  The SYS_CONS_FEED native syscall pushes a canned script and
 * then declares end-of-input, which is what makes the boot demo
 * reproducible.  Both go through the same discipline, so the scripted path
 * exercises the code the live path uses rather than a parallel one.
 *
 * The ring is deliberately ISOLATED from the kbd/uart Mach ports the native
 * shell consumes: nothing a Darwin binary leaves behind can leak into the
 * native surface, and the two never race for the same keystroke -- the
 * claim decides who gets it, once, at the driver.
 */
#define	DARWIN_CONS_BUF		512u
#define	DARWIN_CONS_MASK	(DARWIN_CONS_BUF - 1u)

/* Longest line the discipline will accumulate before forcing it out. */
#define	DARWIN_CONS_LINE	256u

#define	DARWIN_CONS_INTR	0x03	/* Ctrl-C */
#define	DARWIN_CONS_EOT		0x04	/* Ctrl-D */
#define	DARWIN_CONS_ERASE	0x08	/* Ctrl-H / Backspace */
#define	DARWIN_CONS_DEL		0x7F

static long	darwin_unix(struct syscall_frame *f, uint32_t nr);
static long	darwin_mach(struct syscall_frame *f, uint32_t trap);
static long	darwin_mach_msg(struct syscall_frame *f);
static long	darwin_mach_msg_err(long rv, bool sending);
static long	darwin_style9(struct syscall_frame *f, uint32_t num);
static long	darwin_s9_map_image(struct syscall_frame *f);
static long	darwin_s9_fs_stat(struct syscall_frame *f);
static long	darwin_s9_fs_readdir(struct syscall_frame *f);
static long	darwin_s9_uname(struct syscall_frame *f);
static long	darwin_s9_fs_fstat(struct syscall_frame *f);
static long	darwin_s9_fs_fdpath(struct syscall_frame *f);
static bool	darwin_streq(const char *a, const char *b);
static const struct progreg_entry *darwin_bin_lookup(const char *path);

static struct darwin_pipe *darwin_pipe_create(void);
static void	darwin_pipe_drop(struct darwin_pipe *p, bool writer);
static long	darwin_pipe_read(struct syscall_frame *f,
		    struct darwin_pipe *p, void *ubuf, size_t n);
static long	darwin_pipe_write(struct syscall_frame *f,
		    struct darwin_pipe *p, const void *ubuf, size_t n);
static void	darwin_ofile_clear(struct darwin_ofile *of);
static int	darwin_dup_install(struct task *t, int oldfd, int newfd);
static bool	darwin_zombie_reap(uint64_t ppid, uint64_t pid,
		    int *status_out, uint64_t *pid_out);
static long	darwin_cons_read(struct syscall_frame *f, void *ubuf,
		    size_t n);

/*
 * Lock key for the console state below:
 *	(c) darwin_cons_lock
 *	(a) atomic / single-writer, read without the lock
 *
 * The lock exists because the two producers run in different threads (the
 * keyboard driver thread and whichever task calls SYS_CONS_FEED) and the
 * consumer is a third.  Nothing under it sleeps -- the wake is posted after
 * the lock is dropped, because a thread that blocks holding a spinlock in
 * this kernel is never woken again.
 */
static struct spinlock	darwin_cons_lock = SPINLOCK_INIT("darwin-cons");

static char	darwin_cons_buf[DARWIN_CONS_BUF];  /* (c) cooked, deliverable */
static uint32_t	darwin_cons_head;	/* (c) producer end             */
static uint32_t	darwin_cons_tail;	/* (c) consumer end             */
static bool	darwin_cons_eof;	/* (c) no more input is coming  */

static char	darwin_cons_line[DARWIN_CONS_LINE];	/* (c) being typed */
static uint32_t	darwin_cons_line_len;			/* (c) */

/*
 * A scripted session waiting to be "typed".  SYS_CONS_FEED leaves the whole
 * script here rather than pushing it into the ring, and the reader releases
 * one line of it whenever it would otherwise have to wait.
 *
 * That indirection is what keeps the demo honest.  Pushing the script in one
 * go would echo every command before the program had started, and a
 * transcript where the commands all appear above their output proves less
 * than one that interleaves -- while a script that did not echo at all would
 * exercise a different path from the keyboard's.  Releasing a line at a time,
 * on demand, through the same discipline a keystroke uses, is the model that
 * matches what it claims to be: something typing on your behalf, at the speed
 * the program is willing to read.
 */
static char	darwin_cons_script[DARWIN_CONS_BUF];	/* (c) */
static uint32_t	darwin_cons_script_len;			/* (c) */
static uint32_t	darwin_cons_script_off;			/* (c) */

/*
 * WHAT THE TERMINAL HAS BEEN TOLD.
 *
 * Until this, the discipline above was canonical because it was written that
 * way -- the comment on it said as much: the only mode worth having before
 * there is a tcsetattr to leave it with.  This is that tcsetattr's other end.
 * The flags are not decoration: a full-screen program's first act is to turn
 * ICANON and ECHO off, and until it can, every one of them is locked out.
 *
 * One terminal, one setting, no per-descriptor copy.  That is not a
 * simplification of Unix -- it is Unix: the state belongs to the DEVICE, which
 * is why two shells sharing a terminal fight over it, and why `stty` run from
 * one of them changes what the other sees.
 *
 * Guarded by darwin_cons_lock: the discipline reads these on the producer's
 * thread while an ioctl may be writing them on the consumer's.
 */
static struct darwin_termios	darwin_cons_tio;	/* (c) */

/*
 * A terminal nobody has configured, which is the state a session starts in and
 * the state one that ends must be put back into.  Real Unix does NOT do the
 * putting back -- that is why a program killed in raw mode leaves a shell
 * typing blind and why `reset` exists -- but here the console has exactly one
 * claimant at a time, and a wedged terminal would need a REBOOT to clear.  So
 * darwin_cons_release restores this, and that is a deliberate difference,
 * written down rather than discovered.
 */
static void
darwin_cons_tio_default(struct darwin_termios *t)
{
	uint32_t	i;

	t->c_iflag = DARWIN_BRKINT | DARWIN_ICRNL | DARWIN_IXON |
	    DARWIN_IMAXBEL;
	t->c_oflag = DARWIN_OPOST | DARWIN_ONLCR;
	t->c_cflag = DARWIN_CS8 | DARWIN_CREAD | DARWIN_CLOCAL;
	t->c_lflag = DARWIN_ICANON | DARWIN_ISIG | DARWIN_IEXTEN |
	    DARWIN_ECHO | DARWIN_ECHOE | DARWIN_ECHOK | DARWIN_ECHOKE;
	for (i = 0; i < DARWIN_NCCS; i++)
		t->c_cc[i] = 0xFF;		/* _POSIX_VDISABLE */
	for (i = 0; i < sizeof(t->c_pad); i++)
		t->c_pad[i] = 0;
	t->c_cc[DARWIN_VEOF]   = DARWIN_CONS_EOT;	/* ^D */
	t->c_cc[DARWIN_VERASE] = DARWIN_CONS_DEL;	/* ^? */
	t->c_cc[DARWIN_VKILL]  = 0x15;			/* ^U */
	t->c_cc[DARWIN_VINTR]  = DARWIN_CONS_INTR;	/* ^C */
	t->c_cc[DARWIN_VQUIT]  = 0x1C;	/* ^\ */
	t->c_cc[DARWIN_VSUSP]  = 0x1A;	/* ^Z */
	t->c_cc[DARWIN_VSTART] = 0x11;	/* ^Q */
	t->c_cc[DARWIN_VSTOP]  = 0x13;	/* ^S */
	t->c_cc[DARWIN_VMIN]   = 1;
	t->c_cc[DARWIN_VTIME]  = 0;
	t->c_ispeed = DARWIN_B38400;
	t->c_ospeed = DARWIN_B38400;
}

static bool	darwin_cons_tio_ready;			/* (c) */

/*
 * The settings, made real on first use.  A boot-time init hook would do the
 * same job and would be one more thing a future caller could arrive before;
 * asking for the settings is the only way to reach them, so the question is
 * the safest place to answer it.  Caller holds darwin_cons_lock.
 */
static struct darwin_termios *
darwin_cons_tio_locked(void)
{

	if (!darwin_cons_tio_ready) {
		darwin_cons_tio_default(&darwin_cons_tio);
		darwin_cons_tio_ready = true;
	}
	return (&darwin_cons_tio);
}

/*
 * What the terminal did.  vo_n_wait is the one that earns its keep: it counts
 * trips round the wait loop that produced nothing, which is the same number
 * in either implementation and therefore the honest way to compare them.  A
 * reader that sleeps takes one trip per wake; a reader that spun on
 * thread_yield took one per timeslice it was handed, for as long as the
 * prompt sat there.
 */
static uint64_t	darwin_cons_n_read;	/* (c) read(2) calls served      */
static uint64_t	darwin_cons_n_wait;	/* (c) fruitless trips round it  */
static uint64_t	darwin_cons_n_key;	/* (c) characters typed          */
static uint64_t	darwin_cons_n_script;	/* (c) characters scripted       */

/*
 * The one thread parked in darwin_cons_read, or NULL.  Single-consumer: the
 * console has one foreground task and that task has one thread in read(2).
 * Registered and cleared under the lock, woken outside it.
 */
static struct thread *darwin_cons_waiter;	/* (c) */

/*
 * The clean-room libSystem.B.dylib (user/libsystem.c), embedded as a Mach-O
 * blob.  The dyld backchannel maps it by path on demand -- it is the only
 * dependency the S4 programs name.  objcopy derives these symbols from the
 * input file name "libSystem.B.dylib" (every non-alphanumeric byte -> '_').
 * As Tier-1 grows, add a row to darwin_dylibs[] and embed the matching dylib;
 * dyld + this service already resolve an arbitrary canonical path against the
 * table, so no linker rewrite is needed -- only more dylibs and more symbols.
 */
extern uint8_t	_binary_libSystem_B_dylib_start[];
extern uint8_t	_binary_libSystem_B_dylib_end[];

/*
 * libgmp (GMP 6.3.0, a real Homebrew x86-64 bottle).  gfactor's second
 * dependency, and the first dylib registered here beyond libSystem -- the proof
 * that the dyld resolves a multi-dylib closure.  It is keyed on its LITERAL
 * install name: a Homebrew bottle leaves the @@HOMEBREW_PREFIX@@ placeholder
 * unrelocated, and that is exactly the byte string gfactor's LC_LOAD_DYLIB
 * names, so map_image matches it verbatim (no path rewriting in the kernel).
 */
extern uint8_t	_binary_libgmp_10_dylib_start[];
extern uint8_t	_binary_libgmp_10_dylib_end[];

/*
 * libedit (clean-room stub, user/libedit_stub.c).  dash names Apple's
 * /usr/lib/libedit.3.dylib for its interactive line editor and only ever
 * calls into it when stdin is a tty; this stub answers the bind with
 * el_init returning NULL, which dash's own guards treat as "no editor".
 * Like libSystem it is OUR code -- the third dylib in the registry and the
 * second clean-room one.
 */
extern uint8_t	_binary_libedit_3_dylib_start[];
extern uint8_t	_binary_libedit_3_dylib_end[];

#define	DARWIN_DYLIB_PATH_MAX	256

struct darwin_dylib {
	const char	*dy_path;
	const uint8_t	*dy_start;
	const uint8_t	*dy_end;
};

static const struct darwin_dylib	darwin_dylibs[] = {
	{ "/usr/lib/libSystem.B.dylib",
	    _binary_libSystem_B_dylib_start, _binary_libSystem_B_dylib_end },
	{ "@@HOMEBREW_PREFIX@@/opt/gmp/lib/libgmp.10.dylib",
	    _binary_libgmp_10_dylib_start, _binary_libgmp_10_dylib_end },
	{ "/usr/lib/libedit.3.dylib",
	    _binary_libedit_3_dylib_start, _binary_libedit_3_dylib_end },
};

#define	DARWIN_NDYLIBS	(sizeof(darwin_dylibs) / sizeof(darwin_dylibs[0]))

/* Success: carry clear, `val` in %rax. */
static long
darwin_ok(struct syscall_frame *f, long val)
{

	f->sf_user_rflags &= ~(uint64_t)RFLAGS_CF;
	return (val);
}

/* Error: carry set, positive `err` (a Darwin errno) in %rax. */
static long
darwin_err(struct syscall_frame *f, int err)
{

	f->sf_user_rflags |= RFLAGS_CF;
	return ((long)err);
}

long
darwin_dispatch(struct syscall_frame *f)
{
	uint32_t	class;
	uint32_t	num;

	class = (uint32_t)((f->sf_nr >> DARWIN_SYSCALL_CLASS_SHIFT) &
	    DARWIN_SYSCALL_CLASS_MASK);
	num = (uint32_t)(f->sf_nr & DARWIN_SYSCALL_NUMBER_MASK);

	switch (class) {
	case DARWIN_SYSCALL_CLASS_UNIX:
		return (darwin_unix(f, num));
	case DARWIN_SYSCALL_CLASS_MACH:
		return (darwin_mach(f, num));
	case DARWIN_SYSCALL_CLASS_STYLE9:
		return (darwin_style9(f, num));
	default:
		kprintf("darwin: unhandled syscall class %u (nr=0x%llx)\n",
		    (unsigned)class, (unsigned long long)f->sf_nr);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/* ---- pipes --------------------------------------------------------------- */

/*
 * A kernel pipe: one fixed ring shared by every fd cloned from either
 * end (dup2 within a task, fork across tasks).  p_readers/p_writers
 * count the live fds per end; the object frees itself when both hit
 * zero, and the counts ARE the protocol -- a read on an empty ring with
 * p_writers == 0 is EOF, a write with p_readers == 0 is EPIPE.  All
 * fields under p_lock; the lock is only ever held for ring arithmetic
 * (user copies stage through a bounce buffer outside it).
 *
 * Blocking is a yield-spin: the reader (or a writer facing a full ring)
 * re-checks after thread_yield, bailing to EINTR when its task has a
 * kill pending.  A wait-queue wake is the obvious upgrade once a real
 * blocking primitive grows a timeout story; the spin is correct and
 * storm-proof for the pipeline lengths this serves.
 */
#define	DARWIN_PIPE_BUF		4096u
#define	DARWIN_PIPE_CHUNK	512u	/* bounce-buffer granularity */

struct darwin_pipe {
	struct spinlock	p_lock;
	uint32_t	p_rpos;		/* (p) ring read position  */
	uint32_t	p_count;	/* (p) bytes in the ring   */
	uint32_t	p_readers;	/* (p) live PIPE_R fds     */
	uint32_t	p_writers;	/* (p) live PIPE_W fds     */
	uint8_t		p_buf[DARWIN_PIPE_BUF];
};

/* Fresh pipe accounting for the two fds pipe(2) is about to install. */
static struct darwin_pipe *
darwin_pipe_create(void)
{
	struct darwin_pipe	*p;

	p = kmalloc(sizeof(*p));
	if (p == NULL)
		return (NULL);
	spin_init(&p->p_lock, "dpipe");
	p->p_rpos    = 0;
	p->p_count   = 0;
	p->p_readers = 1;
	p->p_writers = 1;
	return (p);
}

/* Drop one end's reference; the last reference of all frees the pipe. */
static void
darwin_pipe_drop(struct darwin_pipe *p, bool writer)
{
	bool	dead;

	spin_lock(&p->p_lock);
	if (writer) {
		KASSERT(p->p_writers > 0, "darwin_pipe_drop: writer underflow");
		p->p_writers--;
	} else {
		KASSERT(p->p_readers > 0, "darwin_pipe_drop: reader underflow");
		p->p_readers--;
	}
	dead = (p->p_readers == 0 && p->p_writers == 0);
	spin_unlock(&p->p_lock);
	if (dead)
		kfree(p);
}

static long
darwin_pipe_read(struct syscall_frame *f, struct darwin_pipe *p,
    void *ubuf, size_t n)
{
	uint8_t		bounce[DARWIN_PIPE_CHUNK];
	uint32_t	i;
	uint32_t	take;

	if (n == 0)
		return (darwin_ok(f, 0));
	if (n > sizeof(bounce))
		n = sizeof(bounce);	/* short reads are POSIX-legal */
	for (;;) {
		spin_lock(&p->p_lock);
		if (p->p_count > 0) {
			take = p->p_count < (uint32_t)n ?
			    p->p_count : (uint32_t)n;
			for (i = 0; i < take; i++) {
				bounce[i] = p->p_buf[p->p_rpos];
				p->p_rpos = (p->p_rpos + 1) %
				    DARWIN_PIPE_BUF;
			}
			p->p_count -= take;
			spin_unlock(&p->p_lock);
			if (syscall_copyout(ubuf, bounce, take) != 0)
				return (darwin_err(f, DARWIN_EFAULT));
			return (darwin_ok(f, (long)take));
		}
		if (p->p_writers == 0) {
			spin_unlock(&p->p_lock);
			return (darwin_ok(f, 0));	/* EOF */
		}
		spin_unlock(&p->p_lock);
		/*
		 * Two different reasons to stop waiting, and for a long time
		 * only the first was asked about: the task has been killed, or
		 * it has a signal posted that it is not blocking.  Without the
		 * second, a reader blocked on a pipe nobody is writing to
		 * ignores SIGINT and SIGTERM entirely -- the signal sits
		 * pending, delivered only if the read happens to finish.
		 */
		if (task_kill_pending(current_thread->th_task) ||
		    darwin_signal_pending(current_thread->th_task))
			return (darwin_err(f, DARWIN_EINTR));
		thread_yield();
	}
}

/*
 * Write the whole buffer, blocking on a full ring -- full-length
 * completion is what a libc that does not retry short writes needs.
 * A reader-less pipe returns the bytes already moved, or EPIPE if
 * nothing was (signal-free EPIPE; SIGPIPE does not exist here yet).
 */
static long
darwin_pipe_write(struct syscall_frame *f, struct darwin_pipe *p,
    const void *ubuf, size_t n)
{
	uint8_t		 bounce[DARWIN_PIPE_CHUNK];
	const uint8_t	*src;
	size_t		 chunk;
	size_t		 done;
	size_t		 off;
	uint32_t	 i;
	uint32_t	 put;
	uint32_t	 space;
	uint32_t	 wpos;

	src  = (const uint8_t *)ubuf;
	done = 0;
	while (done < n) {
		chunk = n - done;
		if (chunk > sizeof(bounce))
			chunk = sizeof(bounce);
		if (syscall_copyin(bounce, src + done, chunk) != 0) {
			if (done > 0)
				return (darwin_ok(f, (long)done));
			return (darwin_err(f, DARWIN_EFAULT));
		}
		off = 0;
		while (off < chunk) {
			spin_lock(&p->p_lock);
			if (p->p_readers == 0) {
				spin_unlock(&p->p_lock);
				if (done > 0)
					return (darwin_ok(f, (long)done));
				/*
				 * Reader-less pipe: post SIGPIPE to the writer.
				 * Ignored or caught, the write just fails with
				 * EPIPE (POSIX); otherwise the default-terminate
				 * signal retires the writer when this syscall
				 * returns and the EPIPE is never observed.
				 */
				darwin_signal_post(current_thread->th_task,
				    DARWIN_SIGPIPE);
				return (darwin_err(f, DARWIN_EPIPE));
			}
			space = DARWIN_PIPE_BUF - p->p_count;
			if (space == 0) {
				spin_unlock(&p->p_lock);
				/* Same pair of reasons as the read side. */
				if (task_kill_pending(
				    current_thread->th_task) ||
				    darwin_signal_pending(
				    current_thread->th_task))
					return (darwin_err(f, DARWIN_EINTR));
				thread_yield();
				continue;
			}
			put = (uint32_t)(chunk - off) < space ?
			    (uint32_t)(chunk - off) : space;
			wpos = (p->p_rpos + p->p_count) % DARWIN_PIPE_BUF;
			for (i = 0; i < put; i++) {
				p->p_buf[wpos] = bounce[off + i];
				wpos = (wpos + 1) % DARWIN_PIPE_BUF;
			}
			p->p_count += put;
			spin_unlock(&p->p_lock);
			off  += put;
			done += put;
		}
	}
	return (darwin_ok(f, (long)n));
}

/* ---- open-file table ------------------------------------------------------ */

/*
 * darwin_fd_alloc_from returns the lowest FREE slot at `min` or above;
 * darwin_fd_alloc is the common floor-3 form (0..2 keep their implicit
 * std-stream meaning until dup2 explicitly retargets them).  The floored
 * variant serves fcntl(F_DUPFD): a shell saves its std fds at 10+ before
 * a redirection and restores them after.  Returns -1 when the table is
 * full above the floor.
 */
static int
darwin_fd_alloc_from(struct task *t, int min)
{
	int	i;

	if (min < 3)
		min = 3;
	for (i = min; i < DARWIN_NOFILE; i++) {
		if (t->t_darwin_files[i].of_type == DARWIN_OF_FREE)
			return (i);
	}
	return (-1);
}

static int
darwin_fd_alloc(struct task *t)
{

	return (darwin_fd_alloc_from(t, 3));
}

/*
 * Keep a copy of what a file was opened as.  An fd holds the file's bytes,
 * but mmap needs its *name*: the pager reads pages one at a time, long after
 * the open, and with no vnode layer here a path is the only durable handle a
 * file has.  Returns NULL on allocation failure, which is not fatal -- the fd
 * still works, it just cannot be mapped.
 */
static char *
darwin_path_dup(const char *path)
{
	char	*copy;
	size_t	 n;

	if (path == NULL)
		return (NULL);
	for (n = 0; path[n] != '\0'; n++)
		continue;
	copy = kmalloc(n + 1);
	if (copy == NULL)
		return (NULL);
	for (n = 0; path[n] != '\0'; n++)
		copy[n] = path[n];
	copy[n] = '\0';
	return (copy);
}

/*
 * read(2) from a disk-backed fd: fetch through the filesystem into a bounce
 * buffer, then hand it to the caller.
 *
 * The bounce exists because fs_pread writes into kernel memory and the user
 * buffer must be crossed under an SMAP bracket; reading straight into ring-3
 * memory would also mean holding a user page while the disk read sleeps.  It
 * is capped rather than sized to the request so that one enormous read cannot
 * ask the heap for an enormous allocation -- the loop delivers the whole
 * length regardless, one chunk at a time.
 */
#define	DARWIN_READ_CHUNK	(64u * 1024u)

static long
darwin_file_read(struct syscall_frame *f, struct darwin_ofile *of, void *ubuf,
    size_t n)
{
	uint8_t		*bounce;
	size_t		 done;
	size_t		 chunk;
	uint32_t	 got;
	int		 rv;

	bounce = kmalloc(n < DARWIN_READ_CHUNK ? n : DARWIN_READ_CHUNK);
	if (bounce == NULL)
		return (darwin_err(f, DARWIN_ENOMEM));

	for (done = 0; done < n; done += chunk) {
		chunk = n - done;
		if (chunk > DARWIN_READ_CHUNK)
			chunk = DARWIN_READ_CHUNK;
		got = 0;
		rv = fs_pread(&of->of_handle, of->of_off + done, bounce,
		    (uint32_t)chunk, &got);
		if (rv != FS_E_OK) {
			kfree(bounce);
			return (darwin_err(f, DARWIN_EIO));
		}
		if (got == 0)			/* end of file, short read */
			break;
		if (syscall_copyout((uint8_t *)ubuf + done, bounce,
		    got) != 0) {
			kfree(bounce);
			return (darwin_err(f, DARWIN_EFAULT));
		}
		if (got < chunk) {
			done += got;
			break;
		}
	}
	kfree(bounce);
	of->of_off += (uint32_t)done;
	return (darwin_ok(f, (long)done));
}

/*
 * What a filesystem refusal is called in Darwin's numbering.
 *
 * FS_E_SPREAD is the interesting one and it is a judgement call: the writer
 * moves a file's records in one copy of one node and refuses when they have
 * been split across two, which is a documented edge of the truncate rung and
 * not a full disk.  ENOSPC is the closest true thing a program can be told --
 * room could not be found -- and EIO would say the volume was damaged, which
 * it is not.  A shell prints "No space left on device" and stops, which is the
 * right behaviour for a request this kernel cannot yet carry out.
 */
static int
darwin_fs_errno(int rv)
{

	switch (rv) {
	case FS_E_OK:		return (0);
	case FS_E_NOTFOUND:	return (DARWIN_ENOENT);
	case FS_E_NOMEM:	return (DARWIN_ENOMEM);
	case FS_E_TOOBIG:	return (DARWIN_ENOMEM);
	case FS_E_ROFS:		return (DARWIN_EROFS);
	case FS_E_NOMOUNT:	return (DARWIN_EROFS);
	case FS_E_EXIST:	return (DARWIN_EEXIST);
	case FS_E_ISDIR:	return (DARWIN_EISDIR);
	case FS_E_NOTDIR:	return (DARWIN_ENOTDIR);
	case FS_E_NOTEMPTY:	return (DARWIN_ENOTEMPTY);
	case FS_E_NOALLOC:	return (DARWIN_ENOSPC);
	case FS_E_SPREAD:	return (DARWIN_ENOSPC);
	default:		return (DARWIN_EIO);
	}
}

/*
 * write(2) to a disk-backed fd: bounce through the kernel, then hand it to the
 * filesystem, the mirror image of darwin_file_read and for the same reasons --
 * fs_pwrite reads out of kernel memory, and a user page must not be held while
 * the disk write sleeps.
 *
 * O_APPEND is resolved HERE, per call, against the length the volume has now
 * rather than the one this fd was opened with.  That is the whole content of
 * the flag: two shells appending to the same log must not overwrite each
 * other, and a cursor remembered from open time is exactly how they would.
 *
 * The write is not reported as short unless the filesystem shortened it.  A
 * partial write that returned success would be indistinguishable to the caller
 * from a full one, and a shell writing a line would silently produce half of it.
 */
#define	DARWIN_WRITE_CHUNK	(64u * 1024u)

static long
darwin_file_write(struct syscall_frame *f, struct darwin_ofile *of,
    const void *ubuf, size_t n)
{
	uint8_t		*bounce;
	uint64_t	 at;
	size_t		 done;
	size_t		 chunk;
	uint32_t	 put;
	int		 rv;

	if ((of->of_flags & DARWIN_O_ACCMODE) == DARWIN_O_RDONLY)
		return (darwin_err(f, DARWIN_EBADF));
	if (of->of_handle.fh_kind == FS_HANDLE_NONE)
		return (darwin_err(f, DARWIN_EROFS));
	if (n == 0)
		return (darwin_ok(f, 0));

	bounce = kmalloc(n < DARWIN_WRITE_CHUNK ? n : DARWIN_WRITE_CHUNK);
	if (bounce == NULL)
		return (darwin_err(f, DARWIN_ENOMEM));

	at = ((of->of_flags & DARWIN_O_APPEND) != 0) ?
	    of->of_handle.fh_size : (uint64_t)of->of_off;
	for (done = 0; done < n; done += put) {
		chunk = n - done;
		if (chunk > DARWIN_WRITE_CHUNK)
			chunk = DARWIN_WRITE_CHUNK;
		if (syscall_copyin(bounce, (const uint8_t *)ubuf + done,
		    chunk) != 0) {
			kfree(bounce);
			return (darwin_err(f, DARWIN_EFAULT));
		}
		put = 0;
		rv = fs_pwrite(&of->of_handle, at + done, bounce,
		    (uint32_t)chunk, &put);
		if (rv != FS_E_OK) {
			if (done != 0)
				break;		/* a short write, not an error */
			kfree(bounce);
			kprintf("darwin: write to '%s' refused (rv=%d)\n",
			    of->of_path != NULL ? of->of_path : "?", rv);
			return (darwin_err(f, darwin_fs_errno(rv)));
		}
		if (put == 0)
			break;
	}
	kfree(bounce);

	/*
	 * The cursor follows the bytes, and the fd's idea of the length
	 * follows the handle's -- which fs_pwrite has already moved if the
	 * write ran off the end.
	 */
	of->of_off  = (uint32_t)(at + done);
	of->of_size = (uint32_t)of->of_handle.fh_size;
	return (darwin_ok(f, (long)done));
}

/* Release whatever one slot holds and return it to FREE. */
static void
darwin_ofile_clear(struct darwin_ofile *of)
{

	switch (of->of_type) {
	case DARWIN_OF_DIR:
	case DARWIN_OF_FILE:
		if (of->of_buf != NULL)
			kfree(of->of_buf);
		if (of->of_path != NULL)
			kfree(of->of_path);
		break;
	case DARWIN_OF_PIPE_R:
		darwin_pipe_drop(of->of_pipe, false);
		break;
	case DARWIN_OF_PIPE_W:
		darwin_pipe_drop(of->of_pipe, true);
		break;
	default:
		break;
	}
	of->of_pipe          = NULL;
	of->of_buf           = NULL;
	of->of_path          = NULL;
	of->of_handle.fh_kind = FS_HANDLE_NONE;
	of->of_handle.fh_id   = 0;
	of->of_handle.fh_size = 0;
	of->of_size          = 0;
	of->of_off           = 0;
	of->of_flags         = 0;
	of->of_type          = DARWIN_OF_FREE;
}

/*
 * Foreground task for console input: the id of the Darwin task that last
 * read(2) the console.  It is also the CLAIM -- while it names a live task,
 * the keyboard driver routes keystrokes here instead of to the Mach input
 * port the native shell reads, so exactly one of the two surfaces receives
 * any given key.  A stand-in for process groups, and enough for the case
 * this system actually has: one interactive shell draining the console.
 *
 * Read without the lock, which is safe because a stale id is harmless: the
 * lookup that follows it either finds a live task or does not.
 */
static uint64_t	darwin_cons_fg_id;		/* (a) */

/* Post a signal to whoever currently holds the console, if anyone does. */
static void
darwin_cons_signal_fg(int sig)
{
	struct task	*fg;

	fg = task_lookup_ref(darwin_cons_fg_id);
	if (fg == NULL)
		return;
	darwin_signal_post(fg, sig);
	task_deref(fg);
}

/* Move the line under construction into the ring, dropping it if full. */
static void
darwin_cons_deliver_locked(void)
{
	uint32_t	i;
	uint32_t	next;

	for (i = 0; i < darwin_cons_line_len; i++) {
		next = darwin_cons_head + 1u;
		if (next - darwin_cons_tail > DARWIN_CONS_BUF)
			break;		/* reader is not keeping up; drop */
		darwin_cons_buf[darwin_cons_head & DARWIN_CONS_MASK] =
		    darwin_cons_line[i];
		darwin_cons_head = next;
	}
	darwin_cons_line_len = 0;
}

/*
 * One character arriving at the terminal: the line discipline.
 *
 * Echo happens HERE, as the key arrives, not where the line is consumed --
 * that is the difference between a terminal and a queue, and it is why a
 * shell that is busy running a command still shows what you type at it.
 *
 * WHICH OF THESE THINGS HAPPEN IS NOW ASKED, not assumed.  This used to be
 * canonical mode because it was written that way; every branch below that
 * edits, echoes or signals is now conditional on the flag that governs it, so
 * a program that turns ICANON and ECHO off gets what it asked for: raw bytes,
 * one at a time, nothing printed.  The three questions are separate on purpose
 * -- programs use all four combinations, and a "raw mode" boolean would have
 * tied echo to editing to signals and served none of them exactly.
 *
 * Returns with the wake, if one is owed, left to the caller: a reader is
 * woken outside the lock, never under it.
 */
static struct thread *
darwin_cons_input_locked(char c, bool *intr_out)
{
	struct darwin_termios	*tio;
	struct thread		*w;

	*intr_out = false;
	tio = darwin_cons_tio_locked();

	/*
	 * Not canonical: the byte is the message.  No editing, no line to
	 * accumulate, and the reader is owed a wake for every single character
	 * rather than for every line -- which is the whole point, since a
	 * program in this mode is waiting on one keystroke.
	 *
	 * ISIG survives ICANON going away; they are independent flags and a
	 * pager that wants raw keys usually still wants Ctrl-C to work.  So the
	 * signal characters are tested first, out of c_cc rather than from the
	 * constants, because a program is allowed to move them.
	 */
	if ((tio->c_lflag & DARWIN_ICANON) == 0) {
		if ((tio->c_lflag & DARWIN_ISIG) != 0 &&
		    (uint8_t)c == tio->c_cc[DARWIN_VINTR]) {
			darwin_cons_line_len = 0;
			*intr_out = true;
			w = darwin_cons_waiter;
			darwin_cons_waiter = NULL;
			return (w);
		}
		if (darwin_cons_line_len >= DARWIN_CONS_LINE)
			darwin_cons_deliver_locked();
		darwin_cons_line[darwin_cons_line_len++] = c;
		darwin_cons_deliver_locked();
		if ((tio->c_lflag & DARWIN_ECHO) != 0)
			tty_putc(c);
		w = darwin_cons_waiter;
		darwin_cons_waiter = NULL;
		return (w);
	}

	/*
	 * Canonical mode.  What follows was a switch on constants; it is a
	 * chain of comparisons against c_cc because those are VARIABLES now --
	 * a program may move its interrupt character, and one that does would
	 * be answered by a switch that still knew only 0x03.
	 *
	 * Input mapping comes first: with ICRNL set -- the default, and what
	 * every one of these programs is built for -- the Return key's carriage
	 * return IS a newline by the time anything else looks at it.  With it
	 * cleared, a bare CR is an ordinary byte and only LF ends a line, which
	 * is what a program that cleared it asked for.
	 */
	if (c == '\r' && (tio->c_iflag & DARWIN_ICRNL) != 0)
		c = '\n';

	if ((tio->c_lflag & DARWIN_ISIG) != 0 &&
	    (uint8_t)c == tio->c_cc[DARWIN_VINTR]) {
		/*
		 * Ctrl-C discards what was typed and signals.  Discarding is
		 * the part that is easy to leave out and wrong to: the line
		 * you abandoned must not arrive at the next prompt.
		 *
		 * It also WAKES the reader, which is not decoration.  Posting
		 * a signal in this kernel only sets a bit -- it does not
		 * disturb a thread already asleep in a syscall -- so a reader
		 * parked at a prompt would sleep through its own interrupt
		 * and collect it at the exit of whatever syscall woke it
		 * next.  Measured, not reasoned: the first version of this
		 * printed ^C, discarded the line correctly, and then ate the
		 * NEXT command the user typed, because that command's read(2)
		 * was the one that carried the stale SIGINT out to dash.
		 */
		darwin_cons_line_len = 0;
		tty_putc('^');
		tty_putc('C');
		tty_putc('\n');
		*intr_out = true;
	} else if ((uint8_t)c == tio->c_cc[DARWIN_VEOF]) {
		/*
		 * Ctrl-D delivers what is typed so far WITHOUT a newline; on
		 * an empty line that is a zero-byte read, which is exactly
		 * how a Unix terminal spells end-of-file.  A shell exits on
		 * it, which is how the console gets handed back.
		 */
		if (darwin_cons_line_len == 0)
			darwin_cons_eof = true;
		else
			darwin_cons_deliver_locked();
	} else if ((uint8_t)c == tio->c_cc[DARWIN_VERASE] ||
	    c == DARWIN_CONS_ERASE) {
		/*
		 * Two keys, one meaning.  VERASE is one character and it is
		 * DEL by default, but this keyboard driver sends backspace for
		 * the key labelled backspace, and a terminal that rubbed out
		 * on only one of them would be wrong for half the hardware
		 * that reaches it.
		 */
		if (darwin_cons_line_len == 0)
			return (NULL);		/* nothing to rub out */
		darwin_cons_line_len--;
		if ((tio->c_lflag & DARWIN_ECHOE) != 0) {
			tty_putc('\b');
			tty_putc(' ');
			tty_putc('\b');
		}
		return (NULL);
	} else if ((uint8_t)c == tio->c_cc[DARWIN_VKILL]) {
		/* Ctrl-U: the whole line goes, and is seen to go. */
		while (darwin_cons_line_len > 0) {
			darwin_cons_line_len--;
			if ((tio->c_lflag & DARWIN_ECHOKE) != 0) {
				tty_putc('\b');
				tty_putc(' ');
				tty_putc('\b');
			}
		}
		return (NULL);
	} else if (c == '\n') {
		if (darwin_cons_line_len < DARWIN_CONS_LINE)
			darwin_cons_line[darwin_cons_line_len++] = '\n';
		if ((tio->c_lflag & (DARWIN_ECHO | DARWIN_ECHONL)) != 0)
			tty_putc('\n');
		darwin_cons_deliver_locked();
	} else {
		if (c < 0x20 && c != '\t')
			return (NULL);		/* not a key we render */
		if (darwin_cons_line_len >= DARWIN_CONS_LINE) {
			/*
			 * A line longer than the buffer is delivered rather
			 * than truncated: losing the tail silently would be
			 * worse than handing the reader a line it did not
			 * ask to be split.
			 */
			darwin_cons_deliver_locked();
		}
		darwin_cons_line[darwin_cons_line_len++] = c;
		if ((tio->c_lflag & DARWIN_ECHO) != 0)
			tty_putc(c);
		return (NULL);
	}

	w = darwin_cons_waiter;
	darwin_cons_waiter = NULL;
	return (w);
}

/*
 * Push one character in from a producer.  Returns true if the console took
 * it -- which is also the answer to "does a Darwin task want this key".
 */
bool
darwin_cons_input(char c)
{
	struct thread	*w;
	bool		 intr;

	spin_lock(&darwin_cons_lock);
	darwin_cons_n_key++;
	w = darwin_cons_input_locked(c, &intr);
	spin_unlock(&darwin_cons_lock);

	/* Both of these can sleep or take other locks; neither may run above. */
	if (intr)
		darwin_cons_signal_fg(DARWIN_SIGINT);
	if (w != NULL)
		thread_wake(w);
	return (true);
}

/*
 * The keyboard driver's sink.  Answers "is a Darwin task holding the
 * console" and, if so, swallows the key; otherwise the driver sends it to
 * the Mach input port and the native shell gets it as before.  One decision,
 * one place: nothing else in the system arbitrates the keyboard.
 */
bool
darwin_cons_sink(char c)
{
	struct task	*fg;

	if (darwin_cons_fg_id == 0)
		return (false);
	fg = task_lookup_ref(darwin_cons_fg_id);
	if (fg == NULL) {
		/*
		 * The claimant died without releasing -- teardown normally
		 * clears this, but a claim that outlives its task would wedge
		 * the keyboard, so drop it here too rather than trust one path.
		 */
		darwin_cons_fg_id = 0;
		return (false);
	}
	task_deref(fg);
	return (darwin_cons_input(c));
}

/*
 * Release the console if `t` was holding it.  Called when a task dies: a
 * claim that outlived its owner would route every keystroke into a ring
 * nobody drains, which from the keyboard's end is indistinguishable from a
 * dead machine.
 */
void
darwin_cons_release(struct task *t)
{

	if (t == NULL || t->t_id != darwin_cons_fg_id)
		return;
	darwin_cons_fg_id = 0;
	spin_lock(&darwin_cons_lock);
	/*
	 * A parked reader belonging to a dying task cannot be woken -- and
	 * must not be, since the thread is on its way out.  Dropping the
	 * pointer here is what keeps a later keystroke from calling
	 * thread_wake on freed memory.
	 */
	if (darwin_cons_waiter != NULL &&
	    darwin_cons_waiter->th_task == t)
		darwin_cons_waiter = NULL;
	darwin_cons_line_len   = 0;
	darwin_cons_tail       = darwin_cons_head;  /* discard unread input */
	darwin_cons_eof        = false;	/* the next claimant starts fresh */
	darwin_cons_script_len = 0;	/* a script belongs to its session */
	darwin_cons_script_off = 0;
	/*
	 * And the SETTINGS go back with everything else.  A program that dies
	 * holding the terminal in raw mode is not an unlikely case -- it is the
	 * ordinary way a full-screen program ends when it crashes -- and on a
	 * machine whose console has one claimant and no `reset` to run, leaving
	 * it that way costs a reboot.  See darwin_cons_tio_default.
	 */
	darwin_cons_tio_default(darwin_cons_tio_locked());
	spin_unlock(&darwin_cons_lock);

	/*
	 * A session ending is the natural moment to say what the terminal
	 * cost, and the only one available while the machine is still up: the
	 * boot-time stats block has long since printed by the time anybody
	 * types.  The waits figure is the interesting one -- see the counters.
	 */
	darwin_cons_stats();
}

/*
 * Load a scripted session, driven by the SYS_CONS_FEED native syscall.  The
 * bytes are not delivered here -- they are held until a reader asks, and then
 * released one line at a time through the same discipline a keystroke uses
 * (see darwin_cons_script_locked).  Running out of script is end-of-input,
 * which is what lets a scripted shell exit without anybody typing Ctrl-D.
 */
void
darwin_cons_feed(const char *buf, size_t n)
{
	size_t	i;

	spin_lock(&darwin_cons_lock);
	if (n > sizeof(darwin_cons_script))
		n = sizeof(darwin_cons_script);
	for (i = 0; i < n; i++)
		darwin_cons_script[i] = buf[i];
	darwin_cons_script_len = (uint32_t)n;
	darwin_cons_script_off = 0;
	darwin_cons_eof = false;
	spin_unlock(&darwin_cons_lock);
}

/*
 * Release the next scripted line into the discipline, or declare end-of-input
 * once the script is spent.  Returns false when there was nothing left to
 * type and the caller should wait for a real key instead; *intr_out reports a
 * Ctrl-C in the script, which the caller signals after dropping the lock
 * exactly as it would for a typed one.
 */
static bool
darwin_cons_script_locked(bool *intr_out)
{
	bool	intr;
	char	c;

	*intr_out = false;
	if (darwin_cons_script_off >= darwin_cons_script_len) {
		if (darwin_cons_script_len != 0)
			darwin_cons_eof = true;	/* the script ran out */
		return (false);
	}
	do {
		c = darwin_cons_script[darwin_cons_script_off++];
		darwin_cons_n_script++;
		/*
		 * The wake this returns is discarded on purpose: the only
		 * thread that could be parked here is the one running this,
		 * and it is about to loop round and find the line itself.
		 */
		(void)darwin_cons_input_locked(c, &intr);
		if (intr)
			*intr_out = true;
	} while (c != '\n' && darwin_cons_script_off < darwin_cons_script_len);
	return (true);
}

/*
 * read(2) on a console fd (implicit stdin or an explicit CONSOLE slot):
 * serve one line of console input.  The discipline above has already echoed
 * it and already decided where the line ends, so this only moves bytes and
 * waits.
 *
 * It waits by SLEEPING.  This used to spin on thread_yield(), which works and
 * costs the whole machine: an interactive shell sitting at a prompt would
 * take every timeslice offered to it, forever, to discover the ring was
 * still empty.  Now the reader parks on the ring and the producer wakes it.
 */
static long
darwin_cons_read(struct syscall_frame *f, void *ubuf, size_t n)
{
	struct darwin_termios	*tio;
	char			 line[256];
	size_t			 got;
	size_t			 least;
	char			 c;
	bool			 eof;
	bool			 intr;

	got = 0;
	darwin_cons_fg_id = current_thread->th_task->t_id;
	if (n > sizeof(line))
		n = sizeof(line);

	for (;;) {
		spin_lock(&darwin_cons_lock);
		tio = darwin_cons_tio_locked();
		/*
		 * HOW LITTLE WILL DO.  In canonical mode the ring only ever
		 * holds whole lines, so anything in it is a complete answer
		 * and one byte is enough to return on.  Out of it, VMIN is the
		 * program's own statement of how little it will settle for --
		 * a pager waiting on a keystroke sets 1 and means it, and a
		 * program setting 0 is asking not to be made to wait at all.
		 *
		 * VTIME is stored and reported back faithfully and is NOT
		 * honoured: a timed wait wants a timer per reader, and there
		 * is nothing here that needs one yet.  Said out loud rather
		 * than left for someone to discover, since a program setting
		 * VMIN=0 VTIME=5 would get a poll instead of a half-second.
		 */
		least = 1;
		if ((tio->c_lflag & DARWIN_ICANON) == 0)
			least = tio->c_cc[DARWIN_VMIN];
		while (got < n && darwin_cons_tail != darwin_cons_head) {
			c = darwin_cons_buf[darwin_cons_tail & DARWIN_CONS_MASK];
			darwin_cons_tail++;
			line[got++] = c;
			if (c == '\n' && (tio->c_lflag & DARWIN_ICANON) != 0)
				break;
		}
		eof = darwin_cons_eof;
		if (got >= least || eof) {
			spin_unlock(&darwin_cons_lock);
			break;
		}

		/*
		 * Nothing queued.  If a script is loaded, this is the moment
		 * it gets to type its next line -- the reader asking is what
		 * paces it, so the transcript interleaves commands with their
		 * output instead of listing them all up front.
		 */
		if (darwin_cons_script_locked(&intr)) {
			spin_unlock(&darwin_cons_lock);
			if (intr)
				darwin_cons_signal_fg(DARWIN_SIGINT);
			continue;
		}

		/*
		 * A pending, unblocked signal (e.g. SIGINT from Ctrl-C) breaks
		 * the blocking read with EINTR; the syscall-exit path then
		 * delivers it -- a caught handler runs, an uncaught SIGINT
		 * terminates.  Mirrors the pipe read's interrupt check, and
		 * has to be tested before parking or a signal that arrived
		 * while we were awake would be slept through.
		 */
		if (darwin_signal_pending(current_thread->th_task)) {
			spin_unlock(&darwin_cons_lock);
			return (darwin_err(f, DARWIN_EINTR));
		}

		/*
		 * Register, then park with the lock dropped ATOMICALLY with
		 * respect to a producer: thread_block_release does the drop
		 * under the scheduler lock, so a wake fired between the two
		 * cannot be lost.  Doing it by hand -- unlock, then block --
		 * is the classic missed-wakeup, and here it would hang a
		 * shell at its prompt until the next keystroke.
		 */
		darwin_cons_n_wait++;
		darwin_cons_waiter = current_thread;
		thread_block_release(THREAD_BLOCK_SLEEP,
		    (void *)&darwin_cons_head, &darwin_cons_lock);
	}

	darwin_cons_n_read++;
	if (got == 0)
		return (darwin_ok(f, 0));		/* EOF */
	if (syscall_copyout(ubuf, line, got) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, (long)got));
}

/*
 * What a create actually gets: what was asked for, less what this task's umask
 * takes away.
 *
 * Every program asks for the most it could possibly want -- 0666 for a file,
 * 0777 for a directory -- and every Unix hands back less.  That is not a
 * courtesy, it is where the number in `ls -l` comes from, and a system without
 * it has to either ignore the argument (which this did, out loud) or produce
 * world-writable files nobody asked for.
 *
 * The argument arrives as a long because that is how a syscall carries one;
 * only the low twelve bits mean anything, and the type bits a caller may have
 * folded in are the filesystem's business rather than the caller's.
 */
static uint16_t
darwin_mode_arg(long raw)
{
	struct task	*t;

	t = current_thread->th_task;
	return ((uint16_t)((uint32_t)raw & 07777u & ~(uint32_t)t->t_darwin_umask));
}

/*
 * ioctl(2) on a terminal, which is the only kind of device a program here can
 * be holding: everything else it can open is a file, a pipe, or nothing.
 *
 * The refusals matter as much as the answers.  ENOTTY on a file and on a pipe
 * is not a failure to implement something -- it is the answer, and it is the
 * one isatty(3) is built out of.  gls asks TIOCGWINSZ before deciding whether
 * to print in columns, and a kernel that answered it for a pipe would have gls
 * writing columns into a file.
 *
 * `arg` is a pointer for every request here; a caller that passes a bad one
 * gets EFAULT from the copy rather than a fault in the kernel.
 */
static long
darwin_cons_ioctl(struct syscall_frame *f, struct darwin_ofile *of,
    int fd, unsigned long req, void *arg)
{
	struct darwin_termios	 tio;
	struct darwin_winsize	 ws;
	uint32_t		 avail;
	int			 n;

	if (of->of_type != DARWIN_OF_CONSOLE &&
	    !(of->of_type == DARWIN_OF_FREE && fd <= 2))
		return (darwin_err(f, of->of_type == DARWIN_OF_FREE ?
		    DARWIN_EBADF : DARWIN_ENOTTY));

	switch (req) {
	case DARWIN_TIOCGETA:
		spin_lock(&darwin_cons_lock);
		tio = *darwin_cons_tio_locked();
		spin_unlock(&darwin_cons_lock);
		if (syscall_copyout(arg, &tio, sizeof(tio)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		return (darwin_ok(f, 0));

	case DARWIN_TIOCSETA:
	case DARWIN_TIOCSETAW:
	case DARWIN_TIOCSETAF:
		if (syscall_copyin(&tio, arg, sizeof(tio)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		spin_lock(&darwin_cons_lock);
		*darwin_cons_tio_locked() = tio;
		/*
		 * The three spellings differ only in what happens to input
		 * that has already arrived.  SETA takes effect now and leaves
		 * it; SETAW would wait for output to drain, and there is
		 * nothing here that buffers output to drain; SETAF discards
		 * what is queued, which is what a program changing modes
		 * between two of its own reads is asking for -- keystrokes
		 * typed under the OLD rules must not arrive under the new.
		 */
		if (req == DARWIN_TIOCSETAF) {
			darwin_cons_tail     = darwin_cons_head;
			darwin_cons_line_len = 0;
		}
		spin_unlock(&darwin_cons_lock);
		kprintf("darwin: the terminal is now %s with echo %s, and "
		    "signals %s\n",
		    (tio.c_lflag & DARWIN_ICANON) != 0 ? "canonical" : "RAW",
		    (tio.c_lflag & DARWIN_ECHO) != 0 ? "on" : "OFF",
		    (tio.c_lflag & DARWIN_ISIG) != 0 ? "on" : "off");
		return (darwin_ok(f, 0));

	case DARWIN_TIOCGWINSZ:
		/*
		 * The console's real size, not a fabricated 80x24: this is a
		 * text-mode display and TTY_COLS x TTY_ROWS is the hardware.
		 * The pixel fields are zero, which is what every terminal that
		 * is not a graphics window reports.
		 */
		ws.ws_row    = TTY_ROWS;
		ws.ws_col    = TTY_COLS;
		ws.ws_xpixel = 0;
		ws.ws_ypixel = 0;
		if (syscall_copyout(arg, &ws, sizeof(ws)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		return (darwin_ok(f, 0));

	case DARWIN_TIOCSWINSZ:
		/*
		 * Refused, and refused honestly.  On a pty the size is a
		 * property of the window and the program that owns it says
		 * what it is; here it is a property of the CRT controller, and
		 * accepting a number we would then keep contradicting -- every
		 * TIOCGWINSZ after it would answer 25x80 again -- is worse
		 * than saying no.
		 */
		return (darwin_err(f, DARWIN_EINVAL));

	case DARWIN_FIONREAD:
		spin_lock(&darwin_cons_lock);
		avail = darwin_cons_head - darwin_cons_tail;
		spin_unlock(&darwin_cons_lock);
		n = (int)avail;
		if (syscall_copyout(arg, &n, sizeof(n)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		return (darwin_ok(f, 0));

	default:
		/*
		 * ENOTTY for a request this terminal does not know, which is
		 * what Unix answers and what the callers are written for --
		 * "inappropriate ioctl for device" is about the REQUEST, not
		 * about the device, however much the wording suggests
		 * otherwise.
		 */
		kprintf("darwin: ioctl(%#lx) on the terminal is not one we "
		    "answer\n", req);
		return (darwin_err(f, DARWIN_ENOTTY));
	}
}

/*
 * One line about the terminal, printed beside the other subsystem counters.
 * The waits-per-read ratio is the whole point: at one wait per read the
 * reader sleeps until something happens, which is what a terminal should
 * cost when nobody is typing.
 */
void
darwin_cons_stats(void)
{

	if (darwin_cons_n_read == 0 && darwin_cons_n_key == 0)
		return;
	kprintf("cons: %llu reads, %llu waits -- %llu keys typed, "
	    "%llu scripted\n",
	    (unsigned long long)darwin_cons_n_read,
	    (unsigned long long)darwin_cons_n_wait,
	    (unsigned long long)darwin_cons_n_key,
	    (unsigned long long)darwin_cons_n_script);
}

/* ---- the working directory ----------------------------------------------- */

/*
 * Resolve a user-supplied path against the calling task's working directory,
 * producing an absolute, normalised path in `out`.
 *
 * Normalisation is not decoration.  A cwd that only ever gets longer is not a
 * working directory -- `cd ..` has to work, and the only place that can
 * happen is here, because the filesystem below resolves components literally
 * and has no notion of a parent link.  So "." is dropped, ".." pops the last
 * component (and does nothing at the root, exactly as a real Unix root
 * behaves), and repeated slashes collapse.
 *
 * Returns 0, or a negative errno-ish for a path that will not fit.  Writing
 * into `out` only on success would be tidier but costs a second buffer; every
 * caller treats a failure as fatal to the syscall and never looks at `out`.
 */
static int
darwin_path_resolve(const struct task *t, const char *in, char *out,
    size_t cap)
{
	size_t	n;
	size_t	i;

	if (in == NULL || cap < 2)
		return (-1);

	n = 0;
	if (in[0] != '/') {
		/* Relative: start from the working directory. */
		for (i = 0; t->t_darwin_cwd[i] != '\0'; i++) {
			if (n + 1 >= cap)
				return (-1);
			out[n++] = t->t_darwin_cwd[i];
		}
	}
	if (n == 0)
		out[n++] = '/';

	i = 0;
	while (in[i] != '\0') {
		size_t	start;
		size_t	len;

		while (in[i] == '/')
			i++;
		if (in[i] == '\0')
			break;
		start = i;
		while (in[i] != '\0' && in[i] != '/')
			i++;
		len = i - start;

		if (len == 1 && in[start] == '.')
			continue;
		if (len == 2 && in[start] == '.' && in[start + 1] == '.') {
			/* Pop one component; at the root there is none. */
			while (n > 1 && out[n - 1] != '/')
				n--;
			if (n > 1)
				n--;		/* drop the separator too */
			if (n == 0)
				out[n++] = '/';
			continue;
		}
		if (out[n - 1] != '/') {
			if (n + 1 >= cap)
				return (-1);
			out[n++] = '/';
		}
		if (n + len >= cap)
			return (-1);
		for (start = i - len; start < i; start++)
			out[n++] = in[start];
	}

	/*
	 * A trailing separator is stripped so the result can be pasted onto
	 * unconditionally, but the root is only a separator and keeping it is
	 * the difference between "/" and the empty string.
	 */
	if (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = '\0';
	return (0);
}

void
darwin_files_teardown(struct task *t)
{
	size_t	i;

	for (i = 0; i < DARWIN_NOFILE; i++) {
		if (t->t_darwin_files[i].of_type != DARWIN_OF_FREE)
			darwin_ofile_clear(&t->t_darwin_files[i]);
	}
	/*
	 * The console is a file this task may have been holding too, and the
	 * one whose loss is not local: a claim outliving its owner would send
	 * every keystroke into a ring nobody drains.
	 */
	darwin_cons_release(t);
}

int
darwin_files_fork_copy(struct task *parent, struct task *child)
{
	struct darwin_ofile	*dst;
	struct darwin_ofile	*src;
	uint8_t			*buf;
	size_t			 i;
	size_t			 k;

	for (i = 0; i < DARWIN_NOFILE; i++) {
		src = &parent->t_darwin_files[i];
		dst = &child->t_darwin_files[i];
		switch (src->of_type) {
		case DARWIN_OF_CONSOLE:
			dst->of_type = DARWIN_OF_CONSOLE;
			break;
		case DARWIN_OF_DIR:
		case DARWIN_OF_FILE:
			/*
			 * Private cursor.  POSIX shares the offset through the
			 * open-file description; for the read-only files this
			 * serves, cursor divergence after fork is unobservable.
			 * A disk-backed fd copies its handle -- three words --
			 * where it used to copy the file's entire contents.
			 */
			buf = NULL;
			if (src->of_buf != NULL) {
				buf = kmalloc(src->of_size != 0 ?
				    src->of_size : 1);
				if (buf == NULL)
					return (-1);
				for (k = 0; k < src->of_size; k++)
					buf[k] = src->of_buf[k];
			}
			dst->of_buf    = buf;
			dst->of_handle = src->of_handle;
			dst->of_path   = darwin_path_dup(src->of_path);
			dst->of_size   = src->of_size;
			dst->of_off    = src->of_off;
			dst->of_flags  = src->of_flags;
			/*
			 * The SOURCE's type, not a constant: a directory
			 * descriptor copies exactly like a file one -- a
			 * duplicated name and nothing else, since it has no
			 * buffer and no handle -- but it must stay a
			 * directory on the other side, and a line that said
			 * FILE here would have quietly turned it into one.
			 */
			dst->of_type   = src->of_type;
			break;
		case DARWIN_OF_PIPE_R:
			spin_lock(&src->of_pipe->p_lock);
			src->of_pipe->p_readers++;
			spin_unlock(&src->of_pipe->p_lock);
			dst->of_pipe = src->of_pipe;
			dst->of_type = DARWIN_OF_PIPE_R;
			break;
		case DARWIN_OF_PIPE_W:
			spin_lock(&src->of_pipe->p_lock);
			src->of_pipe->p_writers++;
			spin_unlock(&src->of_pipe->p_lock);
			dst->of_pipe = src->of_pipe;
			dst->of_type = DARWIN_OF_PIPE_W;
			break;
		default:
			break;
		}
	}
	return (0);
}

/*
 * Copy `oldfd`'s effective slot onto `newfd` -- the shared core of
 * dup(2) and dup2(2).  A FREE slot at fd 0..2 duplicates as an explicit
 * CONSOLE binding (the implicit std stream made concrete).  The target
 * slot is released first.  Returns 0 or a negative Darwin errno.
 */
static int
darwin_dup_install(struct task *t, int oldfd, int newfd)
{
	struct darwin_ofile	*dst;
	struct darwin_ofile	*src;
	uint8_t			*buf;
	uint32_t		 k;
	uint8_t			 type;

	src  = &t->t_darwin_files[oldfd];
	type = src->of_type;
	if (type == DARWIN_OF_FREE) {
		if (oldfd > 2)
			return (-DARWIN_EBADF);
		type = DARWIN_OF_CONSOLE;
	}
	dst = &t->t_darwin_files[newfd];
	if (dst->of_type != DARWIN_OF_FREE)
		darwin_ofile_clear(dst);

	switch (type) {
	case DARWIN_OF_CONSOLE:
		dst->of_type = DARWIN_OF_CONSOLE;
		return (0);
	case DARWIN_OF_DIR:
	case DARWIN_OF_FILE:
		buf = NULL;
		if (src->of_buf != NULL) {
			buf = kmalloc(src->of_size != 0 ?
			    src->of_size : 1);
			if (buf == NULL)
				return (-DARWIN_ENOMEM);
			for (k = 0; k < src->of_size; k++)
				buf[k] = src->of_buf[k];
		}
		dst->of_buf    = buf;
		dst->of_handle = src->of_handle;
		dst->of_path   = darwin_path_dup(src->of_path);
		dst->of_size   = src->of_size;
		dst->of_off    = src->of_off;
		dst->of_flags  = src->of_flags;
		dst->of_type   = src->of_type;
		return (0);
	case DARWIN_OF_PIPE_R:
		spin_lock(&src->of_pipe->p_lock);
		src->of_pipe->p_readers++;
		spin_unlock(&src->of_pipe->p_lock);
		dst->of_pipe = src->of_pipe;
		dst->of_type = DARWIN_OF_PIPE_R;
		return (0);
	case DARWIN_OF_PIPE_W:
		spin_lock(&src->of_pipe->p_lock);
		src->of_pipe->p_writers++;
		spin_unlock(&src->of_pipe->p_lock);
		dst->of_pipe = src->of_pipe;
		dst->of_type = DARWIN_OF_PIPE_W;
		return (0);
	default:
		return (-DARWIN_EBADF);
	}
}

/* ---- zombies (exit status for wait4) -------------------------------------- */

/*
 * The wait4 channel: a dying Darwin task's {pid, ppid, status} parked
 * until the parent reaps it.  Deliberately a flat table, not a proc
 * tree -- style9 has no struct proc, and 32 unreaped children is
 * already a pathological pipeline.  A task that dies by exception
 * (not exit/kill) records nothing; wait4 then reports ECHILD once the
 * child leaves the live task list, which a shell treats as "died
 * weirdly" rather than hanging.
 */
#define	DARWIN_NZOMBIE	32

struct darwin_zombie {
	uint64_t	z_pid;
	uint64_t	z_ppid;
	int		z_status;	/* wait4 format            */
	bool		z_used;
};

static struct darwin_zombie	darwin_zombies[DARWIN_NZOMBIE];	/* (z) */
static struct spinlock		darwin_zombie_lock =
    SPINLOCK_INIT("dzombie");					/* (z) */

/* ---- signals -------------------------------------------------------------- */

/*
 * Bit for signal `signo` in the pending / mask words.  Valid signals are
 * 1..DARWIN_NSIG-1; signal 0 (the kill(2) existence probe) and anything out
 * of range map to no bit, so posting them is a silent no-op.
 */
static inline uint32_t
darwin_sigbit(int signo)
{

	if (signo <= 0 || signo >= DARWIN_NSIG)
		return (0);
	return ((uint32_t)1 << signo);
}

/*
 * Default action for a SIG_DFL signal.  Only SIGCHLD defaults to ignore;
 * every other signal we can post (SIGINT, SIGPIPE, SIGTERM, SIGKILL)
 * defaults to terminating the task.
 */
static bool
darwin_sig_default_is_ignore(int signo)
{

	return (signo == DARWIN_SIGCHLD);
}

void
darwin_signal_post(struct task *t, int signo)
{
	uint32_t	bit;

	bit = darwin_sigbit(signo);
	if (t == NULL || bit == 0)
		return;
	__atomic_fetch_or(&t->t_sig_pending, bit, __ATOMIC_RELEASE);
}

bool
darwin_signal_pending(struct task *t)
{
	uint32_t	deliverable;
	int		signo;

	if (t == NULL)
		return (false);
	/*
	 * One load of the pending set, not one per use: it is written from
	 * another task's thread, and reading it twice could see two different
	 * answers inside a single decision.  t_sig_mask is written only by the
	 * owning task on its own thread, so a plain read of it is right here --
	 * this is that thread.
	 */
	deliverable = __atomic_load_n(&t->t_sig_pending, __ATOMIC_ACQUIRE) &
	    ~t->t_sig_mask;
	if (deliverable == 0)
		return (false);

	/*
	 * Posted and unblocked is not enough.  A signal whose disposition is
	 * to do NOTHING must not end a wait: it will be consumed on the way
	 * back to ring 3 and the caller would have returned EINTR for an event
	 * that left no trace.
	 *
	 * SIGCHLD is the whole reason this matters and it is not a corner
	 * case.  It is default-ignore, and it arrives precisely when a parent
	 * is sitting in wait4 -- so a predicate that counted it made wait4
	 * return EINTR the instant its child died, every time, instead of
	 * reaping it.  That is not a hypothetical: it is what the first
	 * version of this did, and pipefork caught it on the first boot.
	 */
	for (signo = 1; signo < DARWIN_NSIG; signo++) {
		if ((deliverable & darwin_sigbit(signo)) == 0)
			continue;
		if (t->t_sig_handler[signo] == DARWIN_SIG_IGN)
			continue;
		if (t->t_sig_handler[signo] == DARWIN_SIG_DFL &&
		    darwin_sig_default_is_ignore(signo))
			continue;
		return (true);		/* caught, or fatal -- either acts */
	}
	return (false);
}

/*
 * Notify a Darwin parent that a child changed state: post SIGCHLD to the
 * task whose id is `ppid`.  Best-effort -- a parent that already exited is a
 * silent no-op.  Kept out of darwin_zombie_lock: task_lookup_ref takes
 * tasks_lock, and no path holds a task lock under darwin_zombie_lock.
 */
static void
darwin_signal_notify_parent(uint64_t ppid)
{
	struct task	*parent;

	if (ppid == 0)
		return;
	parent = task_lookup_ref(ppid);
	if (parent == NULL)
		return;
	darwin_signal_post(parent, DARWIN_SIGCHLD);
	task_deref(parent);
}

/*
 * Pick the next signal to act on out of `t`'s deliverable set
 * (pending & ~mask), lowest number first, reporting its disposition through
 * *disp_out.  IGN and default-ignore signals are consumed here and skipped;
 * a SIG_DFL-terminate signal is consumed and returned; a caught signal
 * (handler VA) is left pending -- returned so phase 2 can deliver it
 * on-stack -- which also stops the scan, so a terminate queued behind a
 * caught signal waits until the handler is serviced.  Returns 0 when
 * nothing remains deliverable.
 */
static int
darwin_signal_next(struct task *t, uint64_t *disp_out)
{
	uint32_t	deliverable;
	uint32_t	bit;
	uint64_t	disp;
	int		signo;

	for (;;) {
		deliverable = __atomic_load_n(&t->t_sig_pending,
		    __ATOMIC_ACQUIRE) & ~t->t_sig_mask;
		if (deliverable == 0)
			return (0);
		signo = __builtin_ctz(deliverable);
		bit   = (uint32_t)1 << signo;
		disp  = t->t_sig_handler[signo];
		if (disp != DARWIN_SIG_DFL && disp != DARWIN_SIG_IGN) {
			*disp_out = disp;
			return (signo);		/* caught: leave pending */
		}
		__atomic_fetch_and(&t->t_sig_pending, ~bit, __ATOMIC_RELEASE);
		if (disp == DARWIN_SIG_IGN)
			continue;		/* explicit ignore -- discard */
		if (darwin_sig_default_is_ignore(signo))
			continue;		/* default ignore -- discard */
		*disp_out = DARWIN_SIG_DFL;
		return (signo);			/* default terminate */
	}
}

void
darwin_signal_deliver(struct task *t)
{
	uint64_t	disp;
	int		signo;

	disp = DARWIN_SIG_DFL;
	if (t == NULL)
		return;
	signo = darwin_signal_next(t, &disp);
	if (signo == 0)
		return;
	if (disp != DARWIN_SIG_DFL)
		return;		/* caught: phase 2 delivers on-stack */
	/*
	 * SIG_DFL, terminate.  A signalled task never reaches its own exit(2),
	 * so this is the sole writer of its wait4 status (termsig in the low 7
	 * bits -- WIFSIGNALED for the parent's wait4).
	 */
	darwin_zombie_record(t->t_id, t->t_darwin_ppid, signo & 0x7F);
	thread_exit();
	/* NOTREACHED */
}

/*
 * RFLAGS to resume ring 3 with: the arithmetic flags and DF as the sigframe
 * left them, IF and the must-be-set bit 1 forced on, everything else dropped.
 * The frame lives on the user's own stack, so sf_rflags is attacker-writable
 * in the limit -- unmasked, a forged frame could hand ring 3 IOPL 3 (raw I/O
 * ports plus CLI/STI) through either resume path.
 */
static uint64_t
darwin_signal_rflags(uint64_t saved)
{

	return ((saved & DARWIN_SIGRETURN_RFLAGS_MASK) | 0x202ULL);
}

/*
 * Build the on-stack signal frame for a caught signal and reshape `f` so the
 * syscall-exit sysret enters the task's _sigtramp.  Returns 0 on success, -1
 * if the frame could not be written (no trampoline registered, or a bad user
 * stack) -- the caller then falls back to termination.  Stack layout below
 * f's user rsp:
 *
 *	[ 128-byte red zone -- preserved ]
 *	[ darwin_sigframe (64 B, 16-aligned) ]   <- ucontext + saved context
 *	[ 8-byte pad ]                            <- new rsp (%16 == 8 at entry)
 *
 * The exit asm reloads rdi/rsi/rdx/r10 from f->sf_arg0..3, so the trampoline
 * enters with (signo, siginfo=0, ucontext, handler).
 */
static int
darwin_signal_setup_frame(struct syscall_frame *f, int signo, uint64_t handler,
    long rv)
{
	struct darwin_sigframe	frame;
	struct task		*t;
	uint64_t		 base;
	uint64_t		 fa;

	t = current_thread->th_task;
	if (t->t_sig_tramp == 0)
		return (-1);

	frame.sf_magic  = DARWIN_SIGFRAME_MAGIC;
	frame.sf_signo  = (uint64_t)signo;
	frame.sf_rip    = f->sf_user_rip;
	frame.sf_rsp    = f->sf_user_rsp;
	frame.sf_rflags = f->sf_user_rflags;
	frame.sf_rax    = (uint64_t)rv;
	frame.sf_mask   = (uint64_t)t->t_sig_mask;
	frame.sf_pad    = 0;

	base = (f->sf_user_rsp - 128) & ~(uint64_t)15;
	fa   = base - sizeof(frame);
	if (syscall_copyout((void *)fa, &frame, sizeof(frame)) != 0)
		return (-1);

	t->t_sig_mask |= darwin_sigbit(signo);		/* blocked in handler */
	f->sf_user_rip = t->t_sig_tramp;
	f->sf_user_rsp = fa - 8;			/* %16 == 8 at entry */
	f->sf_arg0 = (uint64_t)signo;			/* rdi: signo        */
	f->sf_arg1 = 0;					/* rsi: siginfo      */
	f->sf_arg2 = fa;				/* rdx: ucontext     */
	f->sf_arg3 = handler;				/* r10: handler      */
	return (0);
}

void
darwin_signal_deliver_syscall(struct syscall_frame *f, long rv)
{
	struct task	*t;
	uint64_t	 disp;
	int		 signo;

	t = current_thread->th_task;
	disp = DARWIN_SIG_DFL;
	signo = darwin_signal_next(t, &disp);
	if (signo == 0)
		return;
	if (disp != DARWIN_SIG_DFL) {
		/*
		 * Caught: consume the pending bit and deliver to the ring-3
		 * handler on the user stack.  If the frame cannot be built
		 * (no trampoline / bad stack) fall through to terminate.
		 */
		__atomic_fetch_and(&t->t_sig_pending,
		    ~((uint32_t)1 << signo), __ATOMIC_RELEASE);
		if (darwin_signal_setup_frame(f, signo, disp, rv) == 0)
			return;
	}
	darwin_zombie_record(t->t_id, t->t_darwin_ppid, signo & 0x7F);
	thread_exit();
	/* NOTREACHED */
}

/*
 * The asynchronous twin of darwin_signal_setup_frame.  Same stack layout and
 * the same trampoline entry protocol; what differs is how much has to be
 * saved.  A signal taken at a syscall boundary can rely on the ABI -- the
 * argument and scratch registers were already dead, and the FPU file is
 * caller-saved across a call.  A signal taken from the IRQ path interrupted
 * an arbitrary instruction, so every GPR and the x87/SSE file are live and
 * all of it goes in the frame.
 *
 * The handler VA travels in %r10 here too, even though nothing forces it on
 * this path -- matching the syscall flavour (where SYSCALL owns %rcx) lets
 * one _sigtramp serve both.
 */
static int
darwin_signal_setup_frame_trap(struct trapframe *tf, int signo,
    uint64_t handler)
{
	struct darwin_sigframe_full	frame;
	struct task			*t;
	uint64_t			 base;
	uint64_t			 fa;

	t = current_thread->th_task;
	if (t->t_sig_tramp == 0)
		return (-1);

	frame.sf_magic = DARWIN_SIGFRAME_MAGIC_FULL;
	frame.sf_signo = (uint64_t)signo;
	frame.sf_mask  = (uint64_t)t->t_sig_mask;
	frame.sf_r15   = tf->tf_r15;
	frame.sf_r14   = tf->tf_r14;
	frame.sf_r13   = tf->tf_r13;
	frame.sf_r12   = tf->tf_r12;
	frame.sf_r11   = tf->tf_r11;
	frame.sf_r10   = tf->tf_r10;
	frame.sf_r9    = tf->tf_r9;
	frame.sf_r8    = tf->tf_r8;
	frame.sf_rdi   = tf->tf_rdi;
	frame.sf_rsi   = tf->tf_rsi;
	frame.sf_rbp   = tf->tf_rbp;
	frame.sf_rbx   = tf->tf_rbx;
	frame.sf_rdx   = tf->tf_rdx;
	frame.sf_rcx   = tf->tf_rcx;
	frame.sf_rax   = tf->tf_rax;
	frame.sf_rip   = tf->tf_rip;
	frame.sf_rsp   = tf->tf_rsp;
	frame.sf_rflags = tf->tf_rflags;

	/*
	 * The interrupted thread's x87/SSE file is still live in the register
	 * file -- the trap did not switch threads, and thread_switch_asm is
	 * the only thing that spills th_fpu -- so FXSAVE here captures exactly
	 * the state the handler is about to clobber.
	 */
	__asm__ __volatile__ ("fxsave (%0)" : : "r"(frame.sf_fpu) : "memory");

	base = (tf->tf_rsp - 128) & ~(uint64_t)15;
	fa   = base - sizeof(frame);
	if (syscall_copyout((void *)fa, &frame, sizeof(frame)) != 0)
		return (-1);

	t->t_sig_mask |= darwin_sigbit(signo);		/* blocked in handler */
	tf->tf_rip = t->t_sig_tramp;
	tf->tf_rsp = fa - 8;				/* %16 == 8 at entry */
	tf->tf_rdi = (uint64_t)signo;
	tf->tf_rsi = 0;
	tf->tf_rdx = fa;
	tf->tf_r10 = handler;
	return (0);
}

void
darwin_signal_deliver_trap(struct trapframe *tf)
{
	struct task	*t;
	uint64_t	 disp;
	int		 signo;

	t = current_thread->th_task;
	disp = DARWIN_SIG_DFL;
	signo = darwin_signal_next(t, &disp);
	if (signo == 0)
		return;
	if (disp != DARWIN_SIG_DFL) {
		__atomic_fetch_and(&t->t_sig_pending,
		    ~((uint32_t)1 << signo), __ATOMIC_RELEASE);
		if (darwin_signal_setup_frame_trap(tf, signo, disp) == 0)
			return;
	}
	darwin_zombie_record(t->t_id, t->t_darwin_ppid, signo & 0x7F);
	thread_exit();
	/* NOTREACHED */
}

/*
 * Resume from an SGFR2 frame.  Rebuilds the interrupted machine state as a
 * trapframe and leaves through IRETQ, the one exit that can restore %rcx and
 * %r11 -- SYSRET architecturally destroys both, which is why the async path
 * cannot ride the ordinary syscall return the way the SGFR1 path does.
 *
 * Never returns: on a good frame it lands back in ring 3 where the signal
 * struck, on a corrupt one it retires the task.
 */
static void
darwin_sigreturn_full(uint64_t uctx)
{
	struct darwin_sigframe_full	frame;
	struct trapframe		tf;
	struct task			*t;

	t = current_thread->th_task;
	if (syscall_copyin(&frame, (const void *)uctx, sizeof(frame)) != 0 ||
	    frame.sf_magic != DARWIN_SIGFRAME_MAGIC_FULL) {
		kprintf("darwin: bad async sigreturn frame @0x%llx\n",
		    (unsigned long long)uctx);
		darwin_zombie_record(t->t_id, t->t_darwin_ppid, DARWIN_SIGKILL);
		thread_exit();
		/* NOTREACHED */
	}

	t->t_sig_mask = (uint32_t)frame.sf_mask;

	tf.tf_r15 = frame.sf_r15;
	tf.tf_r14 = frame.sf_r14;
	tf.tf_r13 = frame.sf_r13;
	tf.tf_r12 = frame.sf_r12;
	tf.tf_r11 = frame.sf_r11;
	tf.tf_r10 = frame.sf_r10;
	tf.tf_r9  = frame.sf_r9;
	tf.tf_r8  = frame.sf_r8;
	tf.tf_rdi = frame.sf_rdi;
	tf.tf_rsi = frame.sf_rsi;
	tf.tf_rbp = frame.sf_rbp;
	tf.tf_rbx = frame.sf_rbx;
	tf.tf_rdx = frame.sf_rdx;
	tf.tf_rcx = frame.sf_rcx;
	tf.tf_rax = frame.sf_rax;

	tf.tf_trapno = 0;
	tf.tf_err    = 0;
	tf.tf_rip    = frame.sf_rip;
	tf.tf_cs     = GDT_UCODE | GDT_RPL3;
	tf.tf_rflags = darwin_signal_rflags(frame.sf_rflags);
	tf.tf_rsp    = frame.sf_rsp;
	tf.tf_ss     = GDT_UDATA | GDT_RPL3;

	/*
	 * Reload the interrupted FPU file last: the kernel is built -mno-sse
	 * and touches neither x87 nor XMM between here and the IRETQ, and a
	 * context switch in that window would spill and reload the restored
	 * state intact.
	 */
	__asm__ __volatile__ ("fxrstor (%0)" : : "r"(frame.sf_fpu) : "memory");

	trapframe_iretq(&tf);
	/* NOTREACHED */
}

/* ---- zombies -------------------------------------------------------------- */

void
darwin_zombie_record(unsigned long long pid, unsigned long long ppid,
    int status)
{
	size_t	i;
	int	slot;

	spin_lock(&darwin_zombie_lock);

	/*
	 * Orphan sweep: zombies whose parent is the task dying right now
	 * will never be reaped -- nobody else may wait for them.
	 */
	for (i = 0; i < DARWIN_NZOMBIE; i++) {
		if (darwin_zombies[i].z_used &&
		    darwin_zombies[i].z_ppid == pid)
			darwin_zombies[i].z_used = false;
	}

	if (ppid == 0) {		/* no Darwin parent -- nobody waits */
		spin_unlock(&darwin_zombie_lock);
		return;
	}

	slot = -1;
	for (i = 0; i < DARWIN_NZOMBIE; i++) {
		if (!darwin_zombies[i].z_used) {
			slot = (int)i;
			break;
		}
	}
	if (slot < 0) {
		spin_unlock(&darwin_zombie_lock);
		kprintf("darwin: zombie table full, status of pid %llu "
		    "dropped\n", pid);
		darwin_signal_notify_parent(ppid);
		return;
	}
	darwin_zombies[slot].z_pid    = pid;
	darwin_zombies[slot].z_ppid   = ppid;
	darwin_zombies[slot].z_status = status;
	darwin_zombies[slot].z_used   = true;
	spin_unlock(&darwin_zombie_lock);
	darwin_signal_notify_parent(ppid);
}

/*
 * Reap one zombie of `ppid` (any child when pid == 0, exactly `pid`
 * otherwise).  True with the slot freed and the result written through
 * the out parameters, false when nothing matches.
 */
static bool
darwin_zombie_reap(uint64_t ppid, uint64_t pid, int *status_out,
    uint64_t *pid_out)
{
	size_t	i;

	spin_lock(&darwin_zombie_lock);
	for (i = 0; i < DARWIN_NZOMBIE; i++) {
		if (!darwin_zombies[i].z_used)
			continue;
		if (darwin_zombies[i].z_ppid != ppid)
			continue;
		if (pid != 0 && darwin_zombies[i].z_pid != pid)
			continue;
		*status_out = darwin_zombies[i].z_status;
		*pid_out    = darwin_zombies[i].z_pid;
		darwin_zombies[i].z_used = false;
		spin_unlock(&darwin_zombie_lock);
		return (true);
	}
	spin_unlock(&darwin_zombie_lock);
	return (false);
}

/*
 * Class 2: the BSD/Unix call gate.  Arguments are already in sf_arg0..5 in
 * the right order, so each case just hands them to the style9 primitive.
 */
static long
darwin_unix(struct syscall_frame *f, uint32_t nr)
{

	switch (nr) {
	case DARWIN_SYS_write: {
		struct darwin_ofile	*of;
		struct task		*t;
		long			 rv;
		int			 fd;

		fd = (int)f->sf_arg0;
		t  = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		switch (of->of_type) {
		case DARWIN_OF_FREE:
			if (fd != 1 && fd != 2)
				return (darwin_err(f, DARWIN_EBADF));
			/* FALLTHROUGH -- legacy std stream */
		case DARWIN_OF_CONSOLE:
			rv = syscall_console_write(
			    (const char *)f->sf_arg1, (size_t)f->sf_arg2);
			if (rv < 0)
				return (darwin_err(f, DARWIN_EFAULT));
			return (darwin_ok(f, rv));
		case DARWIN_OF_PIPE_W:
			return (darwin_pipe_write(f, of->of_pipe,
			    (const void *)f->sf_arg1, (size_t)f->sf_arg2));
		case DARWIN_OF_FILE:
			return (darwin_file_write(f, of,
			    (const void *)f->sf_arg1, (size_t)f->sf_arg2));
		default:
			return (darwin_err(f, DARWIN_EBADF));
		}
	}
	case DARWIN_SYS_gettimeofday: {
		struct darwin_timeval	tv;
		int64_t			us;

		/*
		 * The only wall-clock source a program has.  A machine with no
		 * usable RTC reports EPERM rather than handing back 1970: a
		 * program that knows the time is unavailable can say so, while
		 * one told it is the epoch will print a date and be believed.
		 */
		if (!clock_walltime_valid())
			return (darwin_err(f, DARWIN_EPERM));
		us = clock_walltime_us();
		tv.tv_sec  = us / 1000000LL;
		tv.tv_usec = (int32_t)(us % 1000000LL);
		tv.tv_pad  = 0;

		/* NULL is legal: a caller may want only the return value. */
		if (f->sf_arg0 != 0 &&
		    syscall_copyout((void *)f->sf_arg0, &tv, sizeof(tv)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		/* arg1 is the timezone pointer; ignored, as everywhere else. */
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_chdir: {
		char			 raw[DARWIN_PATH_MAX];
		char			 want[DARWIN_PATH_MAX];
		struct fs_statbuf	 sb;
		struct task		*t;
		size_t			 i;
		long			 len;

		t = current_thread->th_task;
		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(t, raw, want, sizeof(want)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));

		/*
		 * Checked before it is adopted, and checked for being a
		 * DIRECTORY rather than merely existing.  A cwd that names a
		 * regular file would make every later relative path resolve
		 * under it and fail one component deeper, where the error has
		 * nothing to do with the mistake that caused it.  The root is
		 * accepted without asking the volume, since it is the one
		 * directory that exists by construction.
		 */
		if (!(want[0] == '/' && want[1] == '\0')) {
			if (fs_stat(want, &sb) != FS_E_OK)
				return (darwin_err(f, DARWIN_ENOENT));
			if (!FS_ISDIR(sb.fs_mode))
				return (darwin_err(f, DARWIN_ENOTDIR));
		}
		for (i = 0; i < DARWIN_PATH_MAX; i++) {
			t->t_darwin_cwd[i] = want[i];
			if (want[i] == '\0')
				break;
		}
		kprintf("darwin: UNIX chdir(\"%s\") -> %s\n", raw,
		    t->t_darwin_cwd);
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS___getcwd: {
		struct task	*t;
		size_t		 n;

		t = current_thread->th_task;
		for (n = 0; t->t_darwin_cwd[n] != '\0'; n++)
			continue;
		n++;				/* the NUL is part of it */
		/*
		 * ERANGE, not a truncated answer.  A caller handed a partial
		 * path would open the wrong thing rather than fail, which is
		 * exactly what getcwd(3) exists to prevent.
		 */
		if (f->sf_arg1 < n)
			return (darwin_err(f, DARWIN_ERANGE));
		if (syscall_copyout((void *)f->sf_arg0, t->t_darwin_cwd,
		    n) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_getpid: {
		uint64_t	id;

		id = current_thread->th_task->t_id;
		kprintf("darwin: UNIX getpid() -> %llu\n",
		    (unsigned long long)id);
		return (darwin_ok(f, (long)id));
	}
	case DARWIN_SYS_getppid:
		return (darwin_ok(f,
		    (long)current_thread->th_task->t_darwin_ppid));
	case DARWIN_SYS_exit: {
		struct task	*t;

		t = current_thread->th_task;
		kprintf("darwin: UNIX exit(%d)\n", (int)f->sf_arg0);
		darwin_zombie_record(t->t_id, t->t_darwin_ppid,
		    ((int)f->sf_arg0 & 0xFF) << 8);
		thread_exit();
		/* NOTREACHED */
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_open: {
		char				 path[DARWIN_PATH_MAX];
		char				 raw[DARWIN_PATH_MAX];
		const struct progreg_entry	*pe;
		struct fs_handle		 handle;
		struct task			*t;
		uint8_t				*buf;
		uint32_t			 size;
		uint32_t			 k;
		uint32_t			 flags;
		long				 len;
		int				 fd;
		int				 rv;
		bool				 writing;
		bool				 on_disk;

		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0) {
			kprintf("darwin: open: bad path pointer\n");
			return (darwin_err(f, DARWIN_EFAULT));
		}
		if (darwin_path_resolve(current_thread->th_task, raw, path,
		    sizeof(path)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));

		/*
		 * WHAT AN OPEN FOR WRITING NOW MEANS
		 *
		 * This used to answer EROFS to every write mode, because the
		 * volume underneath could only be read and an fd that accepted
		 * writes would have swallowed them.  The volume can be written
		 * now, so the flags are honoured -- and the three that MAKE or
		 * UNMAKE bytes are answered here rather than at the first
		 * write, because that is what open(2) promises: after it
		 * returns, the file exists and is the length the flags say.
		 */
		flags   = (uint32_t)f->sf_arg1;
		writing = (flags & DARWIN_O_ACCMODE) != DARWIN_O_RDONLY;

		/*
		 * A DIRECTORY CAN BE OPENED, and that is not a technicality.
		 *
		 * fs_open answers about bytes, so it says "not found" for a
		 * directory -- which is what a program asking for one got, and
		 * the reason GNU mkdir reported that the directory it had just
		 * successfully created did not exist: it opens what it makes.
		 * Measured, not guessed; the kernel said so in one line once
		 * the failing open was made to print.
		 *
		 * What comes back is a descriptor that names a PLACE.  There
		 * is nothing to read through it -- read(2) answers EISDIR --
		 * and writing to one is refused before it starts.  It carries
		 * the path, which is what every call that takes a directory fd
		 * actually wants.
		 */
		{
			struct fs_statbuf	dst;

			if (fs_stat(path, &dst) == FS_E_OK && dst.fs_is_dir) {
				if (writing || (flags & DARWIN_O_TRUNC) != 0)
					return (darwin_err(f, DARWIN_EISDIR));
				fd = darwin_fd_alloc(current_thread->th_task);
				if (fd < 0)
					return (darwin_err(f, DARWIN_EMFILE));
				t = current_thread->th_task;
				t->t_darwin_files[fd].of_path =
				    darwin_path_dup(path);
				t->t_darwin_files[fd].of_size  = 0;
				t->t_darwin_files[fd].of_off   = 0;
				t->t_darwin_files[fd].of_flags = flags;
				t->t_darwin_files[fd].of_type  = DARWIN_OF_DIR;
				kprintf("darwin: UNIX open('%s') -> fd=%d "
				    "(a directory, which names a place and "
				    "not bytes)\n", path, fd);
				return (darwin_ok(f, fd));
			}
		}

		buf = NULL;
		on_disk = true;
		rv = fs_open(path, &handle);
		if (rv == FS_E_NOTFOUND && (flags & DARWIN_O_CREAT) != 0) {
			uint64_t	ino;

			/*
			 * open(2)'s third argument, at last taken seriously.
			 * It is a REQUEST, not the answer: what a file is
			 * created with is the request less this task's umask,
			 * which is why every program asks for 0666 and every
			 * Unix produces 0644.
			 */
			rv = fs_create(path, darwin_mode_arg(f->sf_arg2), &ino);
			if (rv == FS_E_OK)
				rv = fs_open(path, &handle);
			else if (rv == FS_E_ROFS || rv == FS_E_NOMOUNT)
				return (darwin_err(f, DARWIN_EROFS));
			else if (rv != FS_E_EXIST)
				return (darwin_err(f, darwin_fs_errno(rv)));
		} else if (rv == FS_E_OK && (flags & DARWIN_O_CREAT) != 0 &&
		    (flags & DARWIN_O_EXCL) != 0) {
			return (darwin_err(f, DARWIN_EEXIST));
		}
		/*
		 * Resolve, do not read.  What an fd needs is the answer to
		 * "which file"; the bytes come later and only the ones asked
		 * for.  This used to slurp the whole file into the kernel
		 * heap on every open, which cost the file's length per open
		 * and put a ceiling on how large a file could be opened at
		 * all.
		 */
		if (rv == FS_E_NOTFOUND || rv == FS_E_NOMOUNT) {
			/* Not on the disk: try the synthetic /bin (progreg). */
			pe = darwin_bin_lookup(path);
			if (pe == NULL) {
				kprintf("darwin: open('%s') -- nothing of that "
				    "name on the volume or in /bin\n", path);
				return (darwin_err(f, DARWIN_ENOENT));
			}
			/*
			 * A built-in is an image in the kernel's own text.
			 * There is nowhere for a write to it to go, and
			 * saying so is the whole of what EROFS is for.
			 */
			if (writing || (flags & DARWIN_O_TRUNC) != 0)
				return (darwin_err(f, DARWIN_EROFS));
			if (pe->pr_size > 0x7FFFFFFF)
				return (darwin_err(f, DARWIN_ENOMEM));
			buf = kmalloc(pe->pr_size != 0 ?
			    pe->pr_size : 1);
			if (buf == NULL)
				return (darwin_err(f, DARWIN_ENOMEM));
			for (k = 0; k < (uint32_t)pe->pr_size; k++)
				buf[k] = pe->pr_image[k];
			size = (uint32_t)pe->pr_size;
			handle.fh_kind = FS_HANDLE_NONE;
			on_disk = false;
		} else if (rv != FS_E_OK) {
			kprintf("darwin: open('%s') -> %s rv=%d\n",
			    path, fs_kind(), rv);
			if (rv == FS_E_NOMEM || rv == FS_E_TOOBIG)
				return (darwin_err(f, DARWIN_ENOMEM));
			return (darwin_err(f, DARWIN_EIO));
		} else {
			/*
			 * O_TRUNC means the file is empty when open returns,
			 * not when something first writes: a shell that
			 * redirects into a file and then produces no output
			 * has still emptied it, and that is the difference
			 * between `> f` and nothing at all.
			 */
			if ((flags & DARWIN_O_TRUNC) != 0 &&
			    handle.fh_size != 0) {
				rv = fs_truncate(&handle, 0);
				if (rv != FS_E_OK) {
					kprintf("darwin: open('%s'): O_TRUNC "
					    "refused (rv=%d)\n", path, rv);
					return (darwin_err(f,
					    darwin_fs_errno(rv)));
				}
			}
			if (handle.fh_size > 0xFFFFFFFFULL)
				return (darwin_err(f, DARWIN_ENOMEM));
			size = (uint32_t)handle.fh_size;
		}

		t  = current_thread->th_task;
		fd = darwin_fd_alloc(t);
		if (fd < 0) {
			if (buf != NULL)
				kfree(buf);
			return (darwin_err(f, DARWIN_EMFILE));
		}
		t->t_darwin_files[fd].of_buf    = buf;
		t->t_darwin_files[fd].of_handle = handle;
		/* Kept for diagnostics; the handle is what gets read. */
		t->t_darwin_files[fd].of_path =
		    on_disk ? darwin_path_dup(path) : NULL;
		t->t_darwin_files[fd].of_size  = size;
		t->t_darwin_files[fd].of_off   = 0;
		t->t_darwin_files[fd].of_flags = flags;
		t->t_darwin_files[fd].of_type  = DARWIN_OF_FILE;
		kprintf("darwin: UNIX open('%s') -> fd=%d (%u bytes, %s%s)\n",
		    path, fd, (unsigned)size,
		    on_disk ? "on the volume" : "built in",
		    writing ? ", for writing" : "");
		return (darwin_ok(f, fd));
	}
	case DARWIN_SYS_read: {
		struct darwin_ofile	*of;
		struct task		*t;
		uint32_t		 avail;
		size_t			 n;
		long			 rv;
		int			 fd;

		fd = (int)f->sf_arg0;
		n  = (size_t)f->sf_arg2;
		t  = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		switch (of->of_type) {
		case DARWIN_OF_FREE:
			if (fd == 0)	/* implicit stdin == console */
				return (darwin_cons_read(f,
				    (void *)f->sf_arg1, n));
			return (darwin_err(f, DARWIN_EBADF));
		case DARWIN_OF_CONSOLE:
			return (darwin_cons_read(f, (void *)f->sf_arg1, n));
		case DARWIN_OF_DIR:
			/*
			 * A directory is not a stream of bytes to anything
			 * above this kernel: a walker calls readdir(3), which
			 * goes down the fs_readdir backchannel and never
			 * touches this path.  EISDIR is what a modern Unix
			 * answers and what makes the difference visible.
			 */
			return (darwin_err(f, DARWIN_EISDIR));
		case DARWIN_OF_FILE:
			avail = of->of_size - of->of_off;
			if (n > (size_t)avail)
				n = avail;
			if (n == 0)
				return (darwin_ok(f, 0));
			if (of->of_buf != NULL) {
				/* Built-in image: the bytes are already here. */
				rv = syscall_copyout((void *)f->sf_arg1,
				    of->of_buf + of->of_off, n);
				if (rv != 0)
					return (darwin_err(f, DARWIN_EFAULT));
				of->of_off += (uint32_t)n;
				return (darwin_ok(f, (long)n));
			}
			return (darwin_file_read(f, of, (void *)f->sf_arg1, n));
		case DARWIN_OF_PIPE_R:
			return (darwin_pipe_read(f, of->of_pipe,
			    (void *)f->sf_arg1, n));
		default:
			return (darwin_err(f, DARWIN_EBADF));
		}
	}
	case DARWIN_SYS_unlink: {
		char	path[DARWIN_PATH_MAX];
		char	raw[DARWIN_PATH_MAX];
		long	len;
		int	rv;

		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(current_thread->th_task, raw, path,
		    sizeof(path)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));

		/*
		 * An open fd is NOT kept alive by this.  Unix says a file lives
		 * until its last name and its last descriptor are gone, and
		 * that promise needs a reference count on the inode, which
		 * needs a vnode layer this kernel does not have.  So the bytes
		 * go now and a descriptor still open on them reads what is no
		 * longer there.  Said out loud rather than discovered: no
		 * program here holds a file it has unlinked, and one that did
		 * would be relying on something never implemented.
		 */
		rv = fs_unlink(path);
		if (rv != FS_E_OK) {
			if (rv != FS_E_NOTFOUND)
				kprintf("darwin: unlink('%s') refused "
				    "(rv=%d)\n", path, rv);
			return (darwin_err(f, darwin_fs_errno(rv)));
		}
		kprintf("darwin: UNIX unlink('%s') -- the name is gone\n",
		    path);
		return (darwin_ok(f, 0));
	}
	/*
	 * mkdir(2) and rmdir(2), which share everything with unlink above
	 * except which call they end in.
	 *
	 * THE MODE IS HONOURED NOW.  It used to be taken and dropped, said out
	 * loud right here: the writer stamped 0755 on every directory because
	 * there was no umask to subtract and no chmod to correct it with
	 * afterwards.  There are both, so `mkdir foo` -- which asks for 0777,
	 * as every program does -- comes out 0755 the way it does on a Mac, and
	 * `mkdir -m 700` comes out 0700 because it was asked for.
	 */
	case DARWIN_SYS_mkdir:
	case DARWIN_SYS_rmdir: {
		char		path[DARWIN_PATH_MAX];
		char		raw[DARWIN_PATH_MAX];
		const char	*what;
		long		len;
		int		rv;
		bool		make;

		make = nr == DARWIN_SYS_mkdir;
		what = make ? "mkdir" : "rmdir";
		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(current_thread->th_task, raw, path,
		    sizeof(path)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));

		rv = make ? fs_mkdir(path, darwin_mode_arg(f->sf_arg1), NULL) :
		    fs_rmdir(path);
		if (rv != FS_E_OK) {
			/*
			 * "Already there" is to a mkdir what "not there" is
			 * to an unlink: the ordinary answer to a program that
			 * asked rather than looked first, and not worth a
			 * line.  Everything else is.
			 */
			if (rv != FS_E_NOTFOUND &&
			    !(make && rv == FS_E_EXIST))
				kprintf("darwin: %s('%s') refused (rv=%d)\n",
				    what, path, rv);
			return (darwin_err(f, darwin_fs_errno(rv)));
		}
		kprintf("darwin: UNIX %s('%s') -- the directory %s\n", what,
		    path, make ? "is there" : "is gone");
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_close: {
		struct darwin_ofile	*of;
		struct task		*t;
		int			 fd;

		fd = (int)f->sf_arg0;
		t  = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		if (of->of_type == DARWIN_OF_FREE) {
			if (fd <= 2)	/* legacy std streams: no backing */
				return (darwin_ok(f, 0));
			return (darwin_err(f, DARWIN_EBADF));
		}
		darwin_ofile_clear(of);
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_lseek: {
		struct darwin_ofile	*of;
		struct task		*t;
		int64_t			 base;
		int64_t			 off;
		int64_t			 pos;
		int			 fd;
		int			 whence;

		fd     = (int)f->sf_arg0;
		off    = (int64_t)f->sf_arg1;
		whence = (int)f->sf_arg2;
		t  = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		if (of->of_type == DARWIN_OF_PIPE_R ||
		    of->of_type == DARWIN_OF_PIPE_W)
			return (darwin_err(f, DARWIN_ESPIPE));
		if (of->of_type != DARWIN_OF_FILE)
			return (darwin_err(f, DARWIN_EBADF));

		switch (whence) {
		case 0:	base = 0;				break; /* SET */
		case 1:	base = (int64_t)of->of_off;		break; /* CUR */
		case 2:	base = (int64_t)of->of_size;		break; /* END */
		default:
			return (darwin_err(f, DARWIN_EINVAL));
		}
		pos = base + off;
		if (pos < 0 || pos > (int64_t)of->of_size)
			return (darwin_err(f, DARWIN_EINVAL));
		of->of_off = (uint32_t)pos;
		return (darwin_ok(f, (long)pos));
	}
	case DARWIN_SYS_fork: {
		long	rv;

		rv = arch_darwin_fork(f);
		if (rv < 0) {
			kprintf("darwin: UNIX fork() failed rv=%ld\n", rv);
			return (darwin_err(f, DARWIN_ENOMEM));
		}
		kprintf("darwin: UNIX fork() -> child %ld\n", rv);
		return (darwin_ok(f, rv));
	}
	case DARWIN_SYS_wait4: {
		struct task	*t;
		uint64_t	 got;
		long		 pid;
		int		 options;
		int		 status;

		t       = current_thread->th_task;
		pid     = (long)f->sf_arg0;
		options = (int)f->sf_arg2;
		for (;;) {
			if (darwin_zombie_reap(t->t_id,
			    pid > 0 ? (uint64_t)pid : 0, &status, &got)) {
				if (f->sf_arg1 != 0 &&
				    syscall_copyout((void *)f->sf_arg1,
				    &status, sizeof(status)) != 0)
					return (darwin_err(f, DARWIN_EFAULT));
				kprintf("darwin: UNIX wait4 -> pid %llu "
				    "status 0x%x\n",
				    (unsigned long long)got,
				    (unsigned)status);
				return (darwin_ok(f, (long)got));
			}
			/*
			 * No zombie.  If no live child could still make
			 * one, the wait can never succeed.  The zombie is
			 * recorded before the child leaves the live list,
			 * so re-checking the table first means a child
			 * that exits between these two samples is caught
			 * next iteration, never lost.
			 */
			if (task_count_darwin_children(t->t_id,
			    pid > 0 ? (uint64_t)pid : 0) == 0)
				return (darwin_err(f, DARWIN_ECHILD));
			if (options & DARWIN_WNOHANG)
				return (darwin_ok(f, 0));
			/*
			 * A parent waiting on a child that will not exit is
			 * the wait most worth interrupting -- it is where a
			 * shell spends its time, and where Ctrl-C has to
			 * land.  SIGCHLD is in the deliverable set too, so a
			 * caught SIGCHLD breaks the wait and the handler runs
			 * before the loop is re-entered.
			 */
			if (task_kill_pending(t) || darwin_signal_pending(t))
				return (darwin_err(f, DARWIN_EINTR));
			thread_yield();
		}
	}
	case DARWIN_SYS_pipe: {
		struct darwin_pipe	*p;
		struct task		*t;
		int			 rfd;
		int			 wfd;

		t = current_thread->th_task;
		p = darwin_pipe_create();
		if (p == NULL)
			return (darwin_err(f, DARWIN_ENOMEM));
		rfd = darwin_fd_alloc(t);
		wfd = -1;
		if (rfd >= 0) {
			t->t_darwin_files[rfd].of_pipe = p;
			t->t_darwin_files[rfd].of_type = DARWIN_OF_PIPE_R;
			wfd = darwin_fd_alloc(t);
		}
		if (wfd < 0) {
			if (rfd >= 0)
				darwin_ofile_clear(&t->t_darwin_files[rfd]);
			else
				darwin_pipe_drop(p, false);
			darwin_pipe_drop(p, true);
			return (darwin_err(f, DARWIN_EMFILE));
		}
		t->t_darwin_files[wfd].of_pipe = p;
		t->t_darwin_files[wfd].of_type = DARWIN_OF_PIPE_W;
		kprintf("darwin: UNIX pipe() -> r=%d w=%d\n", rfd, wfd);
		/*
		 * Both fds in one %rax: read end low, write end high.
		 * Darwin's native convention is %rax/%rdx; our clean-room
		 * libSystem is the only caller of this number and unpacks
		 * the packed form (the entry stub hands back one register).
		 */
		return (darwin_ok(f,
		    (long)(((uint64_t)(uint32_t)wfd << 32) |
		    (uint32_t)rfd)));
	}
	case DARWIN_SYS_dup: {
		struct task	*t;
		int		 newfd;
		int		 oldfd;
		int		 rv;

		oldfd = (int)f->sf_arg0;
		t     = current_thread->th_task;
		if (oldfd < 0 || oldfd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		newfd = darwin_fd_alloc(t);
		if (newfd < 0)
			return (darwin_err(f, DARWIN_EMFILE));
		rv = darwin_dup_install(t, oldfd, newfd);
		if (rv < 0)
			return (darwin_err(f, -rv));
		return (darwin_ok(f, newfd));
	}
	case DARWIN_SYS_dup2: {
		struct task	*t;
		int		 newfd;
		int		 oldfd;
		int		 rv;

		oldfd = (int)f->sf_arg0;
		newfd = (int)f->sf_arg1;
		t     = current_thread->th_task;
		if (oldfd < 0 || oldfd >= DARWIN_NOFILE ||
		    newfd < 0 || newfd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		if (oldfd == newfd)
			return (darwin_ok(f, newfd));
		rv = darwin_dup_install(t, oldfd, newfd);
		if (rv < 0)
			return (darwin_err(f, -rv));
		return (darwin_ok(f, newfd));
	}
	/*
	 * umask(2): the bits a create may not grant, and the OLD value back.
	 *
	 * Returning the previous mask is not a nicety -- it is the only way to
	 * read the thing, since there is no getumask, and a program that wants
	 * to know sets it twice.
	 */
	case DARWIN_SYS_umask: {
		struct task	*t;
		uint16_t	 was;

		t   = current_thread->th_task;
		was = t->t_darwin_umask;
		t->t_darwin_umask = (uint16_t)((uint32_t)f->sf_arg0 & 07777u);
		return (darwin_ok(f, (long)was));
	}
	/*
	 * chmod(2) and fchmod(2): the permission bits of something that is
	 * already there.
	 *
	 * There is no ownership check because there are no owners -- one user,
	 * root, and a volume whose inodes all say uid 0.  What a real kernel
	 * would refuse here it would refuse on grounds this system does not
	 * have, so the check is absent rather than faked.
	 */
	case DARWIN_SYS_chmod:
	case DARWIN_SYS_fchmod: {
		char			 path[DARWIN_PATH_MAX];
		char			 raw[DARWIN_PATH_MAX];
		struct darwin_ofile	*of;
		struct task		*t;
		long			 len;
		uint16_t		 mode;
		int			 rv;
		int			 fd;

		t = current_thread->th_task;
		if (nr == DARWIN_SYS_fchmod) {
			/*
			 * An fd is a path here, because this kernel has no
			 * vnodes: what it remembers about an open file is the
			 * name it was opened by.  A descriptor onto something
			 * with no name -- a pipe, the console, a built-in
			 * image -- has nothing to chmod, and says so.
			 */
			fd = (int)f->sf_arg0;
			if (fd < 0 || fd >= DARWIN_NOFILE)
				return (darwin_err(f, DARWIN_EBADF));
			of = &t->t_darwin_files[fd];
			if ((of->of_type != DARWIN_OF_FILE &&
			    of->of_type != DARWIN_OF_DIR) ||
			    of->of_path == NULL)
				return (darwin_err(f, DARWIN_EINVAL));
			for (len = 0; of->of_path[len] != '\0'; len++) {
				if (len >= (long)sizeof(path) - 1)
					return (darwin_err(f,
					    DARWIN_ENAMETOOLONG));
				path[len] = of->of_path[len];
			}
			path[len] = '\0';
			mode = (uint16_t)((uint32_t)f->sf_arg1 & 07777u);
		} else {
			len = syscall_copyin_str((const char *)f->sf_arg0, raw,
			    sizeof(raw));
			if (len < 0)
				return (darwin_err(f, DARWIN_EFAULT));
			if (darwin_path_resolve(t, raw, path,
			    sizeof(path)) != 0)
				return (darwin_err(f, DARWIN_ENAMETOOLONG));
			mode = (uint16_t)((uint32_t)f->sf_arg1 & 07777u);
		}

		rv = fs_chmod(path, mode);
		if (rv != FS_E_OK) {
			kprintf("darwin: chmod('%s', %04o) refused (rv=%d)\n",
			    path, (unsigned)mode, rv);
			return (darwin_err(f, darwin_fs_errno(rv)));
		}
		kprintf("darwin: UNIX chmod('%s') -- the mode is %04o now\n",
		    path, (unsigned)mode);
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_ioctl: {
		struct task	*t;
		int		 fd;

		fd = (int)f->sf_arg0;
		t  = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		return (darwin_cons_ioctl(f, &t->t_darwin_files[fd], fd,
		    (unsigned long)f->sf_arg1, (void *)f->sf_arg2));
	}
	case DARWIN_SYS_fcntl: {
		struct task	*t;
		int		 cmd;
		int		 newfd;
		int		 oldfd;
		int		 rv;

		oldfd = (int)f->sf_arg0;
		cmd   = (int)f->sf_arg1;
		t     = current_thread->th_task;
		if (oldfd < 0 || oldfd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		switch (cmd) {
		case DARWIN_F_DUPFD:
		case DARWIN_F_DUPFD_CLOEXEC:
			/*
			 * Close-on-exec is moot here (exec preserves fds by
			 * design and the table has no flag bits), so the
			 * CLOEXEC flavor degenerates to plain F_DUPFD.
			 */
			newfd = darwin_fd_alloc_from(t, (int)f->sf_arg2);
			if (newfd < 0)
				return (darwin_err(f, DARWIN_EMFILE));
			rv = darwin_dup_install(t, oldfd, newfd);
			if (rv < 0)
				return (darwin_err(f, -rv));
			return (darwin_ok(f, newfd));
		case DARWIN_F_GETFD:
		case DARWIN_F_SETFD:
		case DARWIN_F_GETFL:
		case DARWIN_F_SETFL:
			return (darwin_ok(f, 0));
		default:
			kprintf("darwin: fcntl(%d, cmd=%d) unsupported\n",
			    oldfd, cmd);
			return (darwin_err(f, DARWIN_EINVAL));
		}
	}
	case DARWIN_SYS_execve: {
		char				  path[256];
		const struct progreg_entry	 *e;
		char				**kargv;
		const char			 *base;
		size_t				  i;
		long				  n;
		long				  rv;
		uint32_t			  magic;
		int				  argc;

		n = syscall_copyin_str((const char *)f->sf_arg0, path,
		    sizeof(path));
		if (n < 0)
			return (darwin_err(f, DARWIN_EFAULT));

		/*
		 * The program registry is flat: resolve by final path
		 * component, so "/bin/gfactor" and "gfactor" both land on
		 * the registered image.
		 */
		base = path;
		for (i = 0; path[i] != '\0'; i++) {
			if (path[i] == '/')
				base = &path[i + 1];
		}
		e = progreg_find(base);
		if (e == NULL) {
			kprintf("darwin: UNIX execve('%s') -> "
			    "not registered\n", path);
			return (darwin_err(f, DARWIN_ENOENT));
		}
		magic = e->pr_size >= sizeof(uint32_t) ?
		    *(const uint32_t *)(const void *)e->pr_image : 0;
		if (magic != MACHO_MAGIC_64 && magic != MACHO_FAT_MAGIC &&
		    magic != MACHO_FAT_CIGAM)
			return (darwin_err(f, DARWIN_ENOEXEC));

		kargv = NULL;
		argc  = 0;
		rv = syscall_copyin_argv((char *const *)f->sf_arg1, &kargv,
		    &argc);
		if (rv < 0)
			return (darwin_err(f, rv == SYS_E_NOMEM ?
			    DARWIN_ENOMEM : DARWIN_EFAULT));

		kprintf("darwin: UNIX execve('%s') argc=%d\n", path, argc);
		rv = arch_darwin_execve(e->pr_image, e->pr_size, argc,
		    kargv, f);
		if (kargv != NULL)
			kfree(kargv);
		if (rv < 0)
			return (darwin_err(f, DARWIN_ENOMEM));
		/* Frame rewritten; the sysret enters the new image. */
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_kill: {
		struct task	*target;
		uint64_t	 disp;
		long		 pid;
		int		 sig;

		pid = (long)f->sf_arg0;
		sig = (int)f->sf_arg1;
		if (pid <= 0)
			return (darwin_err(f, DARWIN_EINVAL));
		target = task_lookup_ref((uint64_t)pid);
		if (target == NULL)
			return (darwin_err(f, DARWIN_ESRCH));
		/*
		 * The target's disposition decides the mechanism.  Reading it
		 * unlocked is exact on this uniprocessor: sigaction(2) is the
		 * only writer, it only ever writes its OWN task, and no thread
		 * runs between our read and the post below (syscalls execute
		 * with IF clear).
		 */
		disp = (sig > 0 && sig < DARWIN_NSIG)
		    ? target->t_sig_handler[sig] : DARWIN_SIG_DFL;
		if (sig != 0 && (disp != DARWIN_SIG_DFL ||
		    darwin_sig_default_is_ignore(sig) ||
		    target == current_thread->th_task)) {
			/*
			 * Post and let return-to-user delivery decide.  That
			 * covers every signal the target does not simply die
			 * from: one it catches (its handler runs at its next
			 * return to ring 3 -- a timer IRQ is enough, it need
			 * never syscall), one it ignores, a default-ignore
			 * signal (SIGCHLD), and ANY self-signal, which is
			 * applied at THIS kill syscall's own exit.
			 */
			darwin_signal_post(target, sig);
		} else if (sig != 0) {
			/*
			 * Cross-task default-terminate.  The target may be
			 * blocked in a syscall and there is no signal-wake to
			 * pull it out yet, so termination stays synchronous
			 * here.  Record the wait4 status (termsig in the low
			 * bits -- a terminated task never reaches its own
			 * exit(2), so this is the only writer) and request the
			 * async kill.
			 */
			kprintf("darwin: UNIX kill(%ld, %d) -> terminate\n",
			    pid, sig);
			darwin_zombie_record(target->t_id,
			    target->t_darwin_ppid, sig & 0x7F);
			task_request_terminate((uint64_t)pid);
		}
		task_deref(target);
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_sigaction: {
		int	signo;

		/*
		 * libSystem's sigaction/signal marshal (signo, handler) into
		 * arg0/arg1: arg1 is the ring-3 handler VA -- DARWIN_SIG_DFL
		 * (0), DARWIN_SIG_IGN (1), or a function pointer.  We record the
		 * disposition here; on-stack invocation of a caught handler is
		 * phase 2 (until then a caught signal simply stays pending and
		 * never terminates).  SIGKILL is uncatchable.
		 */
		signo = (int)f->sf_arg0;
		if (signo <= 0 || signo >= DARWIN_NSIG ||
		    signo == DARWIN_SIGKILL)
			return (darwin_err(f, DARWIN_EINVAL));
		current_thread->th_task->t_sig_handler[signo] = f->sf_arg1;
		/* arg2 carries libSystem's _sigtramp VA (same for every sig). */
		if (f->sf_arg2 != 0)
			current_thread->th_task->t_sig_tramp = f->sf_arg2;
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_sigprocmask: {
		struct task	*t;
		uint32_t	 old;
		uint32_t	 set;
		int		 how;

		/*
		 * (how, newmask) arrive in arg0/arg1; how == 0 means "query
		 * only, no change" (libSystem sends it for a NULL set).  The
		 * old mask returns in %rax so libSystem can store *oset.
		 * SIGKILL can never be blocked.
		 */
		t   = current_thread->th_task;
		old = t->t_sig_mask;
		how = (int)f->sf_arg0;
		set = (uint32_t)f->sf_arg1;
		switch (how) {
		case DARWIN_SIG_BLOCK:
			t->t_sig_mask |= set;
			break;
		case DARWIN_SIG_UNBLOCK:
			t->t_sig_mask &= ~set;
			break;
		case DARWIN_SIG_SETMASK:
			t->t_sig_mask = set;
			break;
		default:
			break;			/* how == 0: no change */
		}
		t->t_sig_mask &= ~darwin_sigbit(DARWIN_SIGKILL);
		return (darwin_ok(f, (long)old));
	}
	case DARWIN_SYS_sigreturn: {
		struct darwin_sigframe	frame;
		struct task		*t;
		uint64_t		 uctx;

		/*
		 * Restore the context saved at delivery.  _sigtramp passes the
		 * ucontext in arg0; the magic at offset 0 says which flavour it
		 * is.  An SGFR2 (asynchronous) frame carries a whole machine
		 * state and can only be resumed by IRETQ, so it leaves through
		 * darwin_sigreturn_full and never comes back here.  An SGFR1
		 * frame reshapes THIS syscall frame so the sysret lands back at
		 * the interrupted rip/rsp/rflags with the original %rax --
		 * sigreturn does not "return" normally.  A bad pointer or an
		 * unknown magic means a corrupt/forged frame; kill the task
		 * rather than resume into nonsense.
		 *
		 * Reading 64 bytes is safe for either flavour: SGFR2 is the
		 * larger struct and shares the magic's placement.
		 */
		t    = current_thread->th_task;
		uctx = f->sf_arg0;
		if (syscall_copyin(&frame, (const void *)uctx,
		    sizeof(frame)) != 0 ||
		    (frame.sf_magic != DARWIN_SIGFRAME_MAGIC &&
		    frame.sf_magic != DARWIN_SIGFRAME_MAGIC_FULL)) {
			kprintf("darwin: bad sigreturn frame @0x%llx\n",
			    (unsigned long long)uctx);
			darwin_zombie_record(t->t_id, t->t_darwin_ppid,
			    DARWIN_SIGKILL);
			thread_exit();
			/* NOTREACHED */
		}
		if (frame.sf_magic == DARWIN_SIGFRAME_MAGIC_FULL) {
			darwin_sigreturn_full(uctx);
			/* NOTREACHED */
		}
		t->t_sig_mask     = (uint32_t)frame.sf_mask;
		f->sf_user_rip    = frame.sf_rip;
		f->sf_user_rsp    = frame.sf_rsp;
		f->sf_user_rflags = darwin_signal_rflags(frame.sf_rflags);
		return ((long)frame.sf_rax);		/* becomes user %rax */
	}
	case DARWIN_SYS_mmap: {
		struct darwin_ofile	*of;
		struct vm_object	*obj;
		struct task		*t;
		uint64_t		 size;
		uint64_t		 off;
		uint64_t		 va;
		uint32_t		 uprot;
		uint32_t		 flags;
		uint8_t			 prot;
		int			 fd;

		size  = f->sf_arg1;
		uprot = (uint32_t)f->sf_arg2;
		flags = (uint32_t)f->sf_arg3;
		fd    = (int)f->sf_arg4;
		off   = f->sf_arg5;
		t     = current_thread->th_task;

		if (size == 0)
			return (darwin_err(f, DARWIN_EINVAL));
		/*
		 * The address argument is a hint and this takes it as one: the
		 * map picks the range.  MAP_FIXED is the case where the caller
		 * is not asking but telling, and honouring it means splitting
		 * or replacing whatever already lives there -- say no rather
		 * than quietly place the mapping somewhere else, which is the
		 * one answer a MAP_FIXED caller cannot cope with.
		 */
		if ((flags & DARWIN_MAP_FIXED) != 0)
			return (darwin_err(f, DARWIN_EINVAL));
		if ((flags & (DARWIN_MAP_SHARED | DARWIN_MAP_PRIVATE)) == 0)
			return (darwin_err(f, DARWIN_EINVAL));

		size = (size + 0xFFFull) & ~0xFFFull;
		if (size == 0)			/* rounded past 64 bits */
			return (darwin_err(f, DARWIN_ENOMEM));

		prot = VM_PROT_USER;
		if ((uprot & DARWIN_PROT_READ) != 0)
			prot |= VM_PROT_READ;
		if ((uprot & DARWIN_PROT_WRITE) != 0)
			prot |= VM_PROT_WRITE;
		if ((uprot & DARWIN_PROT_EXEC) != 0)
			prot |= VM_PROT_EXEC;

		obj = NULL;
		if ((flags & DARWIN_MAP_ANON) != 0) {
			if (off != 0)
				return (darwin_err(f, DARWIN_EINVAL));
		} else {
			if (fd < 0 || fd >= DARWIN_NOFILE)
				return (darwin_err(f, DARWIN_EBADF));
			of = &t->t_darwin_files[fd];
			if (of->of_type != DARWIN_OF_FILE)
				return (darwin_err(f, DARWIN_EBADF));
			/*
			 * Only a file that lives on the volume can be paged
			 * in.  The synthetic /bin entries are built into the
			 * kernel image and have no handle to read through.
			 */
			if (of->of_handle.fh_kind == FS_HANDLE_NONE)
				return (darwin_err(f, DARWIN_ENODEV));
			if ((off & 0xFFFull) != 0)
				return (darwin_err(f, DARWIN_EINVAL));
			obj = vm_object_file(&of->of_handle, of->of_path);
			if (obj == NULL)
				return (darwin_err(f, DARWIN_ENOMEM));
		}

		if (!vm_map_find_space(t->t_map, size, &va)) {
			vm_object_deref(obj);
			return (darwin_err(f, DARWIN_ENOMEM));
		}
		/*
		 * No frames are allocated here and no page tables are touched:
		 * the entry is the whole mapping until something reads or
		 * writes it.  That is the difference between this and
		 * vm_allocate, and it is why mapping a 4 MiB file costs a
		 * kmalloc rather than 4 MiB.
		 */
		if (!vm_map_enter_backed(t->t_map, va, size, prot,
		    VME_F_ANON | VME_F_LAZY, obj, off)) {
			vm_object_deref(obj);
			return (darwin_err(f, DARWIN_ENOMEM));
		}
		kprintf("darwin: UNIX mmap(%llu KiB, prot=%u, %s) -> 0x%llx\n",
		    (unsigned long long)(size >> 10), (unsigned)uprot,
		    (obj != NULL) ? obj->vo_path : "anon",
		    (unsigned long long)va);
		return (darwin_ok(f, (long)va));
	}
	case DARWIN_SYS_munmap: {
		struct task	*t;
		uint64_t	 va;
		uint64_t	 size;

		va   = f->sf_arg0;
		size = f->sf_arg1;
		t    = current_thread->th_task;

		if (size == 0 || (va & 0xFFFull) != 0)
			return (darwin_err(f, DARWIN_EINVAL));
		size = (size + 0xFFFull) & ~0xFFFull;
		if (size == 0)
			return (darwin_err(f, DARWIN_EINVAL));
		/*
		 * Any sub-range of a mapping, including its middle: vm_map
		 * cuts the entries at the edges of the request.  What is still
		 * refused is a range with a hole in it, which POSIX would let
		 * pass but which here means the caller has lost track of what
		 * it owns.
		 */
		if (!vm_map_release(t->t_map, t->t_pmap, va, size))
			return (darwin_err(f, DARWIN_EINVAL));
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_setitimer:
		/*
		 * No interval timers yet; succeed so a defensive disarm
		 * (setitimer with a zero value) is a clean no-op.
		 */
		return (darwin_ok(f, 0));
	default:
		kprintf("darwin: unimplemented BSD syscall %u\n",
		    (unsigned)nr);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/*
 * Class 1: the Mach trap gate.  These return a value directly in %rax with no
 * carry convention; we still clear carry so a Mach trap never leaves it set
 * from a prior BSD error on the same thread.
 */
static long
darwin_mach(struct syscall_frame *f, uint32_t trap)
{

	switch (trap) {
	case DARWIN_MACH_task_self_trap:
		kprintf("darwin: MACH task_self_trap() -> name=%u\n",
		    (unsigned)MACH_PORT_TASK_SELF);
		return (darwin_ok(f, (long)MACH_PORT_TASK_SELF));
	case DARWIN_MACH_host_self_trap: {
		mach_port_name_t	n;

		/*
		 * Install a fresh SEND right to the kernel's host port in the
		 * caller's space and hand back the name.  MACH_PORT_NULL on
		 * failure (host_init has not run / table full), matching the
		 * port-returning-trap convention task_self_trap uses.
		 */
		n = MACH_PORT_NULL;
		(void)host_self_acquire(current_thread->th_task->t_port_space,
		    &n);
		kprintf("darwin: MACH host_self_trap() -> name=%u\n",
		    (unsigned)n);
		return (darwin_ok(f, (long)n));
	}
	case DARWIN_MACH_mach_reply_port: {
		mach_port_name_t	n;

		n = port_allocate(current_thread->th_task->t_port_space,
		    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
		kprintf("darwin: MACH mach_reply_port() -> name=%u\n",
		    (unsigned)n);
		return (darwin_ok(f, (long)n));	/* MACH_PORT_NULL on failure */
	}
	case DARWIN_MACH_mach_msg_trap:
		return (darwin_mach_msg(f));
	default:
		/* Port-returning traps signal failure with a null name. */
		kprintf("darwin: unhandled mach trap %u\n", (unsigned)trap);
		return (darwin_ok(f, (long)MACH_PORT_NULL));
	}
}

/*
 * Map a style9 mach_msg result -- MACH_MSG_OK / a positive MACH_E_*, or the
 * negative SYS_E_FAULT a user-range check returns -- onto the Darwin
 * mach_msg_return_t the caller reads.  `sending` selects the SEND_* vs RCV_*
 * code family.
 */
static long
darwin_mach_msg_err(long rv, bool sending)
{

	switch (rv) {
	case MACH_E_NAME:
	case MACH_E_RIGHT:
	case MACH_E_DEAD:
		return (sending ? DARWIN_MACH_SEND_INVALID_DEST :
		    DARWIN_MACH_RCV_INVALID_NAME);
	case MACH_E_TOOSMALL:
		return (DARWIN_MACH_RCV_TOO_LARGE);
	case MACH_E_TIMEOUT:
		return (sending ? DARWIN_MACH_SEND_TIMED_OUT :
		    DARWIN_MACH_RCV_TIMED_OUT);
	default:
		return (sending ? DARWIN_MACH_SEND_INVALID_DATA :
		    DARWIN_MACH_RCV_INVALID_DATA);
	}
}

/*
 * mach_msg_trap (Mach class 1, trap 31): the classic combined send/receive.
 * Darwin's mach_msg(3) packs its arguments into the syscall registers in the
 * usual order; we read msg/option/rcv_size/rcv_name/timeout.  The 7th arg
 * (notify) is unsupported -- a classic mach_msg passes MACH_PORT_NULL there.
 * send_size (arg2) is implicit: the kernel honours msg->msgh_size.  A combined
 * SEND|RCV sends then receives into the same buffer, exactly as mach_msg(3)
 * does; the shared syscall_msg_* helpers do the user-range check + SMAP
 * bracket + drive the kernel's existing message path.  Returns a
 * mach_msg_return_t in %rax with carry clear (Mach convention).
 */
static long
darwin_mach_msg(struct syscall_frame *f)
{
	struct mach_msg_header	*msg;
	uint64_t		 timeout;
	uint32_t		 option;
	uint32_t		 rcv_size;
	mach_port_name_t	 rcv_name;
	long			 rv;

	msg      = (struct mach_msg_header *)f->sf_arg0;
	option   = (uint32_t)f->sf_arg1;
	rcv_size = (uint32_t)f->sf_arg3;
	rcv_name = (mach_port_name_t)f->sf_arg4;
	timeout  = f->sf_arg5;

	if (option & DARWIN_MACH_SEND_MSG) {
		rv = syscall_msg_send(msg);
		if (rv != MACH_MSG_OK) {
			kprintf("darwin: MACH mach_msg send -> rv=%ld\n", rv);
			return (darwin_ok(f, darwin_mach_msg_err(rv, true)));
		}
	}
	if (option & DARWIN_MACH_RCV_MSG) {
		if (option & DARWIN_MACH_RCV_TIMEOUT)
			rv = syscall_msg_recv_timed(rcv_name, msg, rcv_size,
			    timeout);
		else
			rv = syscall_msg_recv(rcv_name, msg, rcv_size);
		if (rv != MACH_MSG_OK) {
			kprintf("darwin: MACH mach_msg recv -> rv=%ld\n", rv);
			return (darwin_ok(f, darwin_mach_msg_err(rv, false)));
		}
	}

	kprintf("darwin: MACH mach_msg(option=0x%x) -> KERN_SUCCESS\n",
	    (unsigned)option);
	return (darwin_ok(f, DARWIN_MACH_MSG_SUCCESS));
}

/*
 * style9-private call gate (class DARWIN_SYSCALL_CLASS_STYLE9), reached only
 * from our own dyld -- never from a genuine Apple binary.  See darwin.h.
 */
static long
darwin_style9(struct syscall_frame *f, uint32_t num)
{

	switch (num) {
	case DARWIN_S9_dyld_map_image:
		return (darwin_s9_map_image(f));
	case DARWIN_S9_fs_stat:
		return (darwin_s9_fs_stat(f));
	case DARWIN_S9_fs_readdir:
		return (darwin_s9_fs_readdir(f));
	case DARWIN_S9_uname:
		return (darwin_s9_uname(f));
	case DARWIN_S9_fs_fstat:
		return (darwin_s9_fs_fstat(f));
	case DARWIN_S9_fs_fdpath:
		return (darwin_s9_fs_fdpath(f));
	default:
		kprintf("darwin: unimplemented style9 call %u\n",
		    (unsigned)num);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/*
 * map_image(const char *path): map the embedded dylib registered under `path`
 * into the calling task at its next dylib base, and return that base in %rax.
 * dyld reads the dependency name out of the main image's LC_LOAD_DYLIB and
 * hands it here; the kernel owns the actual mapping (it holds the blob + the
 * VM machinery), which keeps the user/kernel SMAP boundary clean.  Carry set
 * with 0 in %rax on any failure (unknown path, fault, OOM) so dyld can branch.
 */
static long
darwin_s9_map_image(struct syscall_frame *f)
{
	char		path[DARWIN_DYLIB_PATH_MAX];
	struct task	*t;
	uint64_t	bias;
	uint64_t	span;
	size_t		i;
	long		n;
	int		rv;

	t = current_thread->th_task;

	n = syscall_copyin_str((const char *)f->sf_arg0, path, sizeof(path));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));

	for (i = 0; i < DARWIN_NDYLIBS; i++)
		if (darwin_streq(path, darwin_dylibs[i].dy_path))
			break;
	if (i == DARWIN_NDYLIBS) {
		kprintf("darwin: s9 map_image '%s' -> not registered\n", path);
		return (darwin_err(f, DARWIN_ENOENT));
	}

	if (t->t_darwin_dylib_next == 0)
		t->t_darwin_dylib_next = DARWIN_DYLIB_BASE;
	bias = t->t_darwin_dylib_next;

	rv = macho_map_dylib(t, darwin_dylibs[i].dy_start,
	    (size_t)(darwin_dylibs[i].dy_end - darwin_dylibs[i].dy_start),
	    bias, &span);
	if (rv != MACHO_E_OK) {
		kprintf("darwin: s9 map_image '%s' map rv=%d\n", path, rv);
		return (darwin_err(f, DARWIN_ENOMEM));
	}
	t->t_darwin_dylib_next = bias + span;

	kprintf("darwin: s9 map_image '%s' -> base=0x%llx span=0x%llx\n",
	    path, (unsigned long long)bias, (unsigned long long)span);
	return (darwin_ok(f, (long)bias));
}

/*
 * Metadata for something in the program registry, which is not on any volume:
 * these files are part of the kernel image, so no filesystem has an opinion
 * about when they were written or who owns them.
 *
 * The timestamp is the one true thing available -- the moment this kernel
 * started running, which is when these files came into existence as far as
 * anything can observe.  It is computed rather than sampled (wall time minus
 * uptime is exactly the anchor clock_init took from the RTC), so repeated
 * stats of /bin/hello agree with each other instead of drifting a second per
 * second the way reporting "now" would.  A machine with no usable RTC reports
 * zero, which is the same "unrecorded" a volume without timestamps reports.
 *
 * The mode says read-only and executable because that is precisely what a
 * program baked into the kernel image is.
 */
static void
darwin_bin_statbuf(struct fs_statbuf *sb, int is_dir)
{
	uint64_t	ns;
	size_t		i;
	uint8_t		*p;

	p = (uint8_t *)sb;
	for (i = 0; i < sizeof(*sb); i++)
		p[i] = 0;

	ns = 0;
	if (clock_walltime_valid()) {
		ns = (uint64_t)(clock_walltime_us() -
		    (int64_t)clock_uptime_us()) * 1000ULL;
	}
	sb->fs_mtime_ns = ns;
	sb->fs_atime_ns = ns;
	sb->fs_ctime_ns = ns;
	sb->fs_btime_ns = ns;
	sb->fs_nlink    = 1;
	sb->fs_mode     = is_dir ? (FS_S_IFDIR | 0555) : (FS_S_IFREG | 0555);
	sb->fs_is_dir   = is_dir ? 1 : 0;
}

/*
 * fs_stat(const char *path, struct fs_statbuf *out): existence + size + type +
 * inode probe behind libSystem's stat$INODE64.  Copies the small fs_statbuf
 * out to the caller and returns 0 (carry clear) if the file is present, carry
 * set otherwise.  The kernel reports only this neutral struct; libSystem turns
 * it into Apple's struct stat, so the macOS ABI layout stays out of the kernel
 * -- and which filesystem answered stays out of libSystem.
 */
static long
darwin_s9_fs_stat(struct syscall_frame *f)
{
	char				 path[DARWIN_PATH_MAX];
	char				 raw[DARWIN_PATH_MAX];
	struct fs_statbuf		 sb;
	const struct progreg_entry	*pe;
	long				 n;
	int				 rv;

	n = syscall_copyin_str((const char *)f->sf_arg0, raw, sizeof(raw));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));
	if (darwin_path_resolve(current_thread->th_task, raw, path,
	    sizeof(path)) != 0)
		return (darwin_err(f, DARWIN_ENAMETOOLONG));

	/*
	 * /bin answers as the overlay it is (see fs_readdir below): the
	 * directory itself is neither the volume's nor the registry's alone,
	 * so it keeps a stable synthetic inode even when the volume has a
	 * /bin.  Everything under it resolves normally, registry first.
	 */
	if (darwin_streq(path, DARWIN_BIN_DIR)) {
		darwin_bin_statbuf(&sb, 1);
		sb.fs_ino    = DARWIN_BIN_INO_BASE;
	} else if ((pe = darwin_bin_lookup(path)) != NULL) {
		darwin_bin_statbuf(&sb, 0);
		sb.fs_size   = pe->pr_size;
		sb.fs_ino    = DARWIN_BIN_INO_BASE + 1 +
		    (uint32_t)(pe - progreg_at(0));
	} else {
		rv = fs_stat(path, &sb);
		if (rv != FS_E_OK)
			return (darwin_err(f, DARWIN_ENOENT));
	}
	if (syscall_copyout((void *)f->sf_arg1, &sb, sizeof(sb)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 0));
}

/*
 * fs_readdir(const char *path, uint32_t index, struct fs_dirent *out):
 * fill *out with the index-th entry of the directory at `path`, behind
 * libSystem's opendir/readdir.  Returns 1 in %rax when an entry was written, 0
 * at end-of-directory (carry clear either way), carry set on error.  The
 * kernel keeps no per-fd cursor: each call re-resolves and re-scans to
 * `index`, which is cheap for the small read-only directories this serves.
 */
static long
darwin_s9_fs_readdir(struct syscall_frame *f)
{
	char				 path[DARWIN_PATH_MAX];
	char				 raw[DARWIN_PATH_MAX];
	struct fs_dirent		 de;
	struct fs_statbuf		 sb;
	const struct progreg_entry	*pe;
	uint32_t			 index;
	uint32_t			 nreal;
	long				 n;
	int				 i;
	int				 rv;

	n = syscall_copyin_str((const char *)f->sf_arg0, raw, sizeof(raw));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));
	if (darwin_path_resolve(current_thread->th_task, raw, path,
	    sizeof(path)) != 0)
		return (darwin_err(f, DARWIN_ENAMETOOLONG));
	index = (uint32_t)f->sf_arg1;

	if (darwin_streq(path, DARWIN_BIN_DIR)) {
		/*
		 * /bin is an OVERLAY: whatever the volume has there, with the
		 * program registry appended.  open() and stat() already see
		 * both -- they try the disk and fall back to the registry --
		 * so a listing that showed only the registry was the one
		 * operation disagreeing with the other two, and a file you can
		 * open but cannot see is worse than either answer alone.
		 */
		if (fs_readdir(path, index, &de) != 1) {
			/*
			 * Past the volume's own entries.  How many there were
			 * has to be counted, because the registry's numbering
			 * starts where the disk's stops and neither side knows
			 * about the other.
			 */
			for (nreal = 0; fs_readdir(path, nreal, &de) == 1;
			    nreal++)
				continue;
			pe = progreg_at(index - nreal);
			if (pe == NULL)
				return (darwin_ok(f, 0));  /* end of directory */
			de.fde_ino    = DARWIN_BIN_INO_BASE + 1 +
			    (index - nreal);
			de.fde_size   = pe->pr_size;
			de.fde_is_dir = 0;
			for (i = 0; i + 1 < FS_NAME_MAX &&
			    pe->pr_name[i] != '\0'; i++)
				de.fde_name[i] = pe->pr_name[i];
			de.fde_name[i] = '\0';
		}
	} else {
		rv = fs_readdir(path, index, &de);
		if (rv < 0)
			return (darwin_err(f, DARWIN_ENOENT));
		if (rv == 0) {
			/*
			 * End of the on-disk listing.  The root grows one
			 * synthetic entry -- "bin" -- at exactly the first
			 * end index (a probe at index-1 still yielding an
			 * entry proves this is that index), so a directory
			 * walker discovers the program registry.
			 */
			if (!darwin_streq(path, "/"))
				return (darwin_ok(f, 0));
			if (index > 0 &&
			    fs_readdir(path, index - 1, &de) != 1)
				return (darwin_ok(f, 0));
			/*
			 * Unless the volume already has a /bin of its own, in
			 * which case the overlay above has merged the registry
			 * into it and naming it again here would list one
			 * directory twice.
			 */
			if (fs_stat(DARWIN_BIN_DIR, &sb) == FS_E_OK)
				return (darwin_ok(f, 0));
			de.fde_ino    = DARWIN_BIN_INO_BASE;
			de.fde_size   = 0;
			de.fde_is_dir = 1;
			de.fde_name[0] = 'b';
			de.fde_name[1] = 'i';
			de.fde_name[2] = 'n';
			de.fde_name[3] = '\0';
		}
	}
	if (syscall_copyout((void *)f->sf_arg2, &de, sizeof(de)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 1));
}

/*
 * fs_fstat(int fd, struct darwin_fdstat *out): classify what an open fd
 * holds for libSystem's fstat64.  The neutral kinds map onto S_IFREG /
 * S_IFCHR / S_IFIFO in the clean-room library; the implicit std streams
 * (FREE at 0..2) classify as the console they reach.
 */
static long
darwin_s9_fs_fstat(struct syscall_frame *f)
{
	struct darwin_fdstat	 ds;
	struct darwin_ofile	*of;
	struct task		*t;
	int			 fd;

	fd = (int)f->sf_arg0;
	t  = current_thread->th_task;
	if (fd < 0 || fd >= DARWIN_NOFILE)
		return (darwin_err(f, DARWIN_EBADF));
	of = &t->t_darwin_files[fd];
	ds.fds_size = 0;
	switch (of->of_type) {
	case DARWIN_OF_FREE:
		if (fd > 2)
			return (darwin_err(f, DARWIN_EBADF));
		ds.fds_kind = DARWIN_FDSTAT_CHR;
		break;
	case DARWIN_OF_CONSOLE:
		ds.fds_kind = DARWIN_FDSTAT_CHR;
		break;
	case DARWIN_OF_FILE:
		ds.fds_size = of->of_size;
		ds.fds_kind = DARWIN_FDSTAT_REG;
		break;
	case DARWIN_OF_DIR:
		ds.fds_kind = DARWIN_FDSTAT_DIR;
		break;
	case DARWIN_OF_PIPE_R:
	case DARWIN_OF_PIPE_W:
		ds.fds_kind = DARWIN_FDSTAT_FIFO;
		break;
	default:
		return (darwin_err(f, DARWIN_EBADF));
	}
	if (syscall_copyout((void *)f->sf_arg1, &ds, sizeof(ds)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 0));
}

/*
 * fs_fdpath(int fd, char *buf, size_t cap): what path an fd was opened by.
 *
 * ONE CALL, FIVE SYMBOLS.  The *at family, fdopendir, fchdir and fchmod all
 * ask the same question in different words -- "the thing this descriptor is
 * on, by name" -- and a kernel with vnodes would answer none of them this way.
 * This one has no vnodes: what it keeps about an open file is the path it was
 * opened by, which was being kept for diagnostics and turns out to be exactly
 * what libSystem needs to build the rest of the family on top of the calls
 * that already work.
 *
 * The limit of that honesty is worth stating: a file RENAMED after it was
 * opened would answer with the name it no longer has.  Nothing here can rename
 * yet -- that is a later rung -- and when it can, this becomes a lie that has
 * to be replaced by an inode-keyed answer rather than patched.
 *
 * Returns the length, or fails: EBADF for a descriptor that is not open,
 * EINVAL for one with no name at all (a pipe, the console, a built-in image).
 */
static long
darwin_s9_fs_fdpath(struct syscall_frame *f)
{
	struct darwin_ofile	*of;
	struct task		*t;
	size_t			 cap;
	size_t			 n;
	int			 fd;

	fd  = (int)f->sf_arg0;
	cap = (size_t)f->sf_arg2;
	t   = current_thread->th_task;
	if (fd < 0 || fd >= DARWIN_NOFILE)
		return (darwin_err(f, DARWIN_EBADF));
	of = &t->t_darwin_files[fd];
	if (of->of_type == DARWIN_OF_FREE)
		return (darwin_err(f, DARWIN_EBADF));
	if (of->of_path == NULL)
		return (darwin_err(f, DARWIN_EINVAL));

	for (n = 0; of->of_path[n] != '\0'; n++)
		continue;
	if (cap < n + 1)
		return (darwin_err(f, DARWIN_ERANGE));
	if (syscall_copyout((void *)f->sf_arg1, of->of_path, n + 1) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, (long)n));
}

/*
 * The fabricated Darwin identity this kernel reports through uname().  None of
 * it is "real" -- style9 is not XNU -- but a libSystem-only CLI tool cannot
 * tell: it calls uname(2) and prints whatever comes back, never validating it.
 * The release (23.x == macOS 14 Sonoma) and machine name a plausible x86-64
 * Mac; the version banner is branded style9 so the lie is at least honest about
 * its provenance.  The hostname matches libSystem's gethostname() ("style9").
 */
static const struct darwin_uname	darwin_uname_id = {
	"Darwin",
	"style9",
	"23.6.0",
	"Darwin Kernel Version 23.6.0: style9 clean-room; "
	    "root:xnu-style9/RELEASE_X86_64",
	"x86_64",
};

/*
 * uname(struct darwin_uname *out): copy the identity card out to the caller.
 * libSystem reshapes it into Apple's struct utsname.  Returns 0 (carry clear);
 * carry set only if the destination pointer faults.
 */
static long
darwin_s9_uname(struct syscall_frame *f)
{

	if (syscall_copyout((void *)f->sf_arg0, &darwin_uname_id,
	    sizeof(darwin_uname_id)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 0));
}

/* Tiny NUL-terminated string compare for the dylib registry lookup. */
static bool
darwin_streq(const char *a, const char *b)
{
	size_t	i;

	for (i = 0; ; i++) {
		if (a[i] != b[i])
			return (false);
		if (a[i] == '\0')
			return (true);
	}
}

/*
 * The synthetic /bin: the program registry presented as a directory.  A
 * shell's PATH machinery stat(2)s each candidate before committing to an
 * execve, so the registry the execve resolves against must also be visible
 * to the path calls -- otherwise every registered program is runnable yet
 * "not found".  This helper answers "/bin/<name>" lookups for the FS-shaped
 * services (open / fs_stat / fs_readdir above); execve keeps its own
 * basename resolution, and the FAT volume keeps every other path.
 */
static const struct progreg_entry *
darwin_bin_lookup(const char *path)
{
	size_t	i;

	for (i = 0; DARWIN_BIN_DIR[i] != '\0'; i++)
		if (path[i] != DARWIN_BIN_DIR[i])
			return (NULL);
	if (path[i] != '/')
		return (NULL);
	return (progreg_find(path + i + 1));
}
