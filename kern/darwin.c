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
#include "cpu.h"
#include "darwin.h"
#include "fs.h"
#include "gdt.h"
#include "host.h"
#include "intr.h"
#include "kmem.h"
#include "kprintf.h"
#include "lapic.h"
#include "macho.h"
#include "mutex.h"
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
 * Darwin (XNU) syscall personality.  syscall_dispatch routes a
 * TASK_PERSONALITY_DARWIN task here; we decode the Apple class/number from
 * %rax and map each call onto the style9 primitive that implements it.
 * Arguments arrive as on style9 (rdi/rsi/rdx/r10/r8/r9); results differ:
 *	- Unix/BSD (class 2): CF clear means %rax is the result; CF set means
 *	  %rax is a positive Darwin errno, mapped from the internal failure.
 *	- Mach (class 1): a port name or kern_return_t in %rax, no carry
 *	  convention; we clear carry.
 *
 * The carry flag lives in the saved user RFLAGS (syscall_entry.S sysrets
 * with %r11 = sf_user_rflags), so every return goes through darwin_ok() or
 * darwin_err() and never inherits stale carry from user code.
 */

#define	RFLAGS_CF	(1u << 0)

/*
 * The synthetic /bin: where darwin_bin_lookup (below) presents the program
 * registry as a directory.  Its inode numbers start at DARWIN_BIN_INO_BASE,
 * meant to be well clear of any the volume hands out.
 */
#define	DARWIN_BIN_DIR		"/bin"
#define	DARWIN_BIN_INO_BASE	0xB1000000u

/*
 * Darwin console input.  The DARWIN_OF_CONSOLE / implicit-stdin read path
 * drains this ring; producers push one character at a time through
 * darwin_cons_input(), the line discipline: echo, erase, Ctrl-C to a
 * signal, Ctrl-D to end-of-file, and only completed lines to the reader.
 * Editing and signals happen when a key arrives, whether or not anyone is
 * reading, which is why they live on the producer side.
 *
 * Two producers: the keyboard driver thread, while a Darwin task has
 * claimed the console (darwin_cons_sink), and the SYS_CONS_FEED native
 * syscall, which loads a canned script for the boot demo.  Both go through
 * the same discipline.
 *
 * The ring is separate from the kbd/uart Mach ports the native shell
 * reads: nothing leaks between the two, and the claim decides at the
 * driver who gets each keystroke.
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
static long	darwin_mach_msg_err(long rv, bool sending, uint32_t option);
static long	darwin_style9(struct syscall_frame *f, uint32_t num);
static long	darwin_s9_map_image(struct syscall_frame *f);
static long	darwin_s9_fs_stat(struct syscall_frame *f);
static long	darwin_s9_fs_readdir(struct syscall_frame *f);
static long	darwin_s9_uname(struct syscall_frame *f);
static long	darwin_s9_fs_fstat(struct syscall_frame *f);
static long	darwin_s9_fs_fdpath(struct syscall_frame *f);
static long	darwin_s9_pselect(struct syscall_frame *f);
static inline uint32_t darwin_sigbit(int signo);
static bool	darwin_streq(const char *a, const char *b);
static const struct progreg_entry *darwin_bin_find(const char *name);
static const struct progreg_entry *darwin_bin_lookup(const char *path);
static void	darwin_select_news(void);
static void	darwin_select_stats(void);

static struct darwin_pipe *darwin_pipe_create(void);
static void	darwin_pipe_drop(struct darwin_pipe *p, bool writer);
static long	darwin_pipe_read(struct syscall_frame *f,
		    struct darwin_pipe *p, void *ubuf, size_t n);
static long	darwin_pipe_write(struct syscall_frame *f,
		    struct darwin_pipe *p, const void *ubuf, size_t n);
static void	darwin_ofile_clear(struct task *t, struct darwin_ofile *of);
static uint64_t	darwin_lk_file(const struct darwin_ofile *of);
static void	darwin_lk_drop(uint64_t owner, uint64_t file);
static void	darwin_lk_stats(void);
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
 * The two producers and the consumer run in different threads.  Nothing
 * sleeps under the lock; wakes are posted after it is dropped.
 */
static struct spinlock	darwin_cons_lock = SPINLOCK_INIT("darwin-cons");

static char	darwin_cons_buf[DARWIN_CONS_BUF];  /* (c) cooked, deliverable */
static uint32_t	darwin_cons_head;	/* (c) producer end             */
static uint32_t	darwin_cons_tail;	/* (c) consumer end             */
static bool	darwin_cons_eof;	/* (c) no more input is coming  */

static char	darwin_cons_line[DARWIN_CONS_LINE];	/* (c) being typed */
static uint32_t	darwin_cons_line_len;			/* (c) */

/*
 * A scripted session waiting to be "typed".  SYS_CONS_FEED leaves the
 * script here, and the reader releases one line of it through the line
 * discipline whenever it would otherwise wait.  Pushing it all at once
 * would echo every command before the program had started; a line at a
 * time interleaves the echo with the output, as typing would.
 */
static char	darwin_cons_script[DARWIN_CONS_BUF];	/* (c) */
static uint32_t	darwin_cons_script_len;			/* (c) */
static uint32_t	darwin_cons_script_off;			/* (c) */

/*
 * The terminal settings (tcgetattr/tcsetattr).  A full-screen program
 * starts by turning ICANON and ECHO off, and the discipline honours these
 * flags.  One setting for the device, not per descriptor, as on Unix.
 * Under darwin_cons_lock: the discipline reads them on the producer's
 * thread while an ioctl may write them on the consumer's.
 */
static struct darwin_termios	darwin_cons_tio;	/* (c) */

/*
 * Default settings: the state a session starts in and, unlike Unix, the
 * state darwin_cons_release restores when it ends.  The console has one
 * claimant at a time and no `reset', so a program killed in raw mode would
 * otherwise leave it wedged until reboot.
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
 * The settings, defaulted on first use rather than by an init hook that a
 * caller could arrive before.  Caller holds darwin_cons_lock.
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
 * Console counters.  darwin_cons_n_wait counts trips round the read wait
 * loop that found nothing: one per wake for a sleeping reader, one per
 * timeslice for a spinning one.
 */
static uint64_t	darwin_cons_n_read;	/* (c) read(2) calls served      */
static uint64_t	darwin_cons_n_wait;	/* (c) fruitless trips round it  */
static uint64_t	darwin_cons_n_key;	/* (c) characters typed          */
static uint64_t	darwin_cons_n_script;	/* (c) characters scripted       */

/*
 * Embedded dylibs, mapped by path on demand through the dyld backchannel
 * (darwin_s9_map_image).  objcopy derives the symbols from the input file
 * name (every non-alphanumeric byte -> '_').  A new dylib is a row in
 * darwin_dylibs[] plus the embedded blob.
 *
 * libSystem.B.dylib: the clean-room one, user/libsystem.c.
 */
extern uint8_t	_binary_libSystem_B_dylib_start[];
extern uint8_t	_binary_libSystem_B_dylib_end[];

/*
 * libgmp (GMP 6.3.0, a real Homebrew x86-64 bottle), gfactor's second
 * dependency.  Keyed on its literal install name: a bottle leaves the
 * @@HOMEBREW_PREFIX@@ placeholder unrelocated, and that is the string
 * gfactor's LC_LOAD_DYLIB names, so it matches verbatim.
 */
extern uint8_t	_binary_libgmp_10_dylib_start[];
extern uint8_t	_binary_libgmp_10_dylib_end[];

/*
 * libedit (clean-room stub, user/libedit_stub.c).  dash links Apple's
 * /usr/lib/libedit.3.dylib and calls it only when stdin is a tty; the
 * stub's el_init returns NULL, which dash treats as "no editor".
 */
extern uint8_t	_binary_libedit_3_dylib_start[];
extern uint8_t	_binary_libedit_3_dylib_end[];

/*
 * sqlite3's two others, both clean-room: libz without a codec
 * (user/libz_stub.c) and a plain line reader under readline's bottle name
 * (user/libreadline_stub.c).
 */
extern uint8_t	_binary_libz_1_dylib_start[];
extern uint8_t	_binary_libz_1_dylib_end[];
extern uint8_t	_binary_libreadline_8_dylib_start[];
extern uint8_t	_binary_libreadline_8_dylib_end[];

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
	{ "/usr/lib/libz.1.dylib",
	    _binary_libz_1_dylib_start, _binary_libz_1_dylib_end },
	{ "@@HOMEBREW_PREFIX@@/opt/readline/lib/libreadline.8.dylib",
	    _binary_libreadline_8_dylib_start,
	    _binary_libreadline_8_dylib_end },
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
 * A kernel pipe: one fixed ring shared by every fd cloned from either end
 * (dup2 within a task, fork across tasks).  p_readers/p_writers count the
 * live fds per end; the pipe frees itself when both reach zero.  A read of
 * an empty ring with no writers is EOF, a write with no readers is EPIPE.
 * All fields under p_lock, held only for ring arithmetic (user copies go
 * through a bounce buffer outside it).
 *
 * A reader sleeps on &p_count, a writer facing a full ring on &p_rpos;
 * either returns EINTR when its task has a kill or an unblocked signal
 * pending.
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
	if (dead) {
		kfree(p);
		return;
	}
	/*
	 * An end going away must wake the other side: a reader with no
	 * writers left is at EOF, a writer with no readers faces EPIPE, and
	 * neither learns it by waiting.  Both channels are woken; each side
	 * re-tests everything anyway.
	 */
	(void)sched_wakeup(&p->p_count);
	(void)sched_wakeup(&p->p_rpos);
	darwin_select_news();
}

/*
 * Pipe and wait4 counters.  As on the console, a "fruitless trip" is a pass
 * round a wait loop that found nothing: one per wake for a parked waiter.
 */
static uint64_t	darwin_pipe_n_read;	/* pipe read(2) calls served    */
static uint64_t	darwin_pipe_n_rwait;	/* fruitless trips, empty ring  */
static uint64_t	darwin_pipe_n_write;	/* pipe write(2) calls served   */
static uint64_t	darwin_pipe_n_wwait;	/* fruitless trips, full ring   */
static uint64_t	darwin_wait_n_call;	/* wait4(2) calls that reaped   */
static uint64_t	darwin_wait_n_wait;	/* fruitless trips, no zombie   */
static uint64_t	darwin_wait_n_net;	/* trips the safety net woke    */
static uint64_t	darwin_wait_n_lost;	/* ...that a lost wake explains */
static uint64_t	darwin_wait_n_told;	/* child-news calls that woke   */
static uint64_t	darwin_wait_n_woke;	/* ...and threads they woke     */

/*
 * How long a parked wait4 sleeps before re-checking on its own.  The wakes
 * for this wait come from every route out of a Darwin task, not all in this
 * file; if one is ever missed, the net keeps the parent from hanging.
 *
 * The net firing is normal for a slow child.  A lost wake is when the
 * re-check finds a zombie already there (darwin_wait_n_lost).
 */
#define	DARWIN_WAIT_NET_MS	500

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
			/* Room in the ring now: a blocked writer wants it. */
			(void)sched_wakeup(&p->p_rpos);
			darwin_select_news();
			darwin_pipe_n_read++;
			if (syscall_copyout(ubuf, bounce, take) != 0)
				return (darwin_err(f, DARWIN_EFAULT));
			return (darwin_ok(f, (long)take));
		}
		if (p->p_writers == 0) {
			spin_unlock(&p->p_lock);
			return (darwin_ok(f, 0));	/* EOF */
		}
		/*
		 * Stop waiting if the task is being killed or has an unblocked
		 * signal pending (else SIGINT would sit until the read ends).
		 * Tested under p_lock, which the park below releases: a signal
		 * posted in between wakes the thread, so it is not missed.
		 */
		if (task_kill_pending(current_thread->th_task) ||
		    darwin_signal_pending(current_thread->th_task)) {
			spin_unlock(&p->p_lock);
			return (darwin_err(f, DARWIN_EINTR));
		}
		/* Sleep until a writer says there are bytes. */
		darwin_pipe_n_rwait++;
		thread_block_release(THREAD_BLOCK_SLEEP, &p->p_count,
		    &p->p_lock);
	}
}

/*
 * Write the whole buffer, blocking on a full ring: a libc that does not
 * retry short writes needs full-length completion.  A reader-less pipe
 * returns the bytes already moved, or posts SIGPIPE and fails with EPIPE
 * if nothing was.
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
				 * Ignored or caught, SIGPIPE leaves the write
				 * failing with EPIPE; by default it terminates
				 * the writer as this syscall returns.
				 */
				darwin_signal_post(current_thread->th_task,
				    DARWIN_SIGPIPE);
				return (darwin_err(f, DARWIN_EPIPE));
			}
			space = DARWIN_PIPE_BUF - p->p_count;
			if (space == 0) {
				/* Same pair of reasons as the read side. */
				if (task_kill_pending(
				    current_thread->th_task) ||
				    darwin_signal_pending(
				    current_thread->th_task)) {
					spin_unlock(&p->p_lock);
					return (darwin_err(f, DARWIN_EINTR));
				}
				/*
				 * Sleep until a reader makes room, on &p_rpos
				 * so readers and writers do not wake each
				 * other for nothing.
				 */
				darwin_pipe_n_wwait++;
				thread_block_release(THREAD_BLOCK_SLEEP,
				    &p->p_rpos, &p->p_lock);
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
			/* Bytes in the ring: a blocked reader wants them. */
			(void)sched_wakeup(&p->p_count);
			darwin_select_news();
			off  += put;
			done += put;
		}
	}
	darwin_pipe_n_write++;
	return (darwin_ok(f, (long)n));
}

/* ---- open-file table ----------------------------------------------------- */

/*
 * darwin_fd_alloc_from returns the lowest FREE slot at `min` or above, or
 * -1 when the table is full above it; fcntl(F_DUPFD) uses the floor (a
 * shell saves its std fds at 10+).  darwin_fd_alloc is the floor-3 form:
 * 0..2 keep their implicit std-stream meaning until dup2 retargets them.
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
 * Keep a copy of the path a file was opened by.  mmap's pager reads pages
 * long after the open, and with no vnode layer the path is the only durable
 * handle.  NULL on allocation failure is not fatal: the fd still works, it
 * just cannot be mapped.
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
 * The cursor of an open file description.  POSIX gives a descriptor and
 * every copy of it -- dup(2), dup2(2), F_DUPFD, fork(2) -- one offset, so
 * what a copy writes lands after what the original wrote (`2>&1`, a
 * subshell writing into its parent's redirection).  The cell is shared by
 * reference, freed with its last descriptor.  Its mutex is held across a
 * read(2), write(2) or lseek(2) through the description, since a parent
 * and its child may use it on two CPUs at once; the fs lock nests inside.
 */
struct darwin_foff {
	struct mutex	fo_lock;
	uint64_t	fo_off;		/* (l) */
	uint32_t	fo_refs;	/* atomic */
};

static struct darwin_foff *
darwin_foff_new(void)
{
	struct darwin_foff	*fo;

	fo = kmalloc(sizeof(*fo));
	if (fo == NULL)
		return (NULL);
	mutex_init(&fo->fo_lock, "dfoff");
	fo->fo_off  = 0;
	fo->fo_refs = 1;
	return (fo);
}

static void
darwin_foff_hold(struct darwin_foff *fo)
{

	(void)__atomic_add_fetch(&fo->fo_refs, 1, __ATOMIC_RELAXED);
}

static void
darwin_foff_drop(struct darwin_foff *fo)
{

	if (__atomic_sub_fetch(&fo->fo_refs, 1, __ATOMIC_ACQ_REL) == 0)
		kfree(fo);
}

/*
 * read(2) from a disk-backed fd, through a kernel bounce buffer: fs_pread
 * writes kernel memory, the user copy needs an SMAP bracket, and no user
 * page should be held while the disk read sleeps.  The bounce is capped at
 * DARWIN_READ_CHUNK so a huge read cannot ask for a huge allocation; the
 * loop still delivers the whole length.  With `atp` NULL the read is at
 * the description's cursor and moves it (read(2)); otherwise it is at
 * *atp and moves nothing (pread(2)).
 */
#define	DARWIN_READ_CHUNK	(64u * 1024u)

static long
darwin_file_read(struct syscall_frame *f, struct darwin_ofile *of, void *ubuf,
    size_t n, const uint64_t *atp)
{
	struct darwin_foff	*fo;
	uint8_t			*bounce;
	uint64_t		 at;
	size_t			 done;
	size_t			 chunk;
	uint32_t		 got;
	int			 err;
	int			 rv;

	if (n == 0)
		return (darwin_ok(f, 0));
	bounce = kmalloc(n < DARWIN_READ_CHUNK ? n : DARWIN_READ_CHUNK);
	if (bounce == NULL)
		return (darwin_err(f, DARWIN_ENOMEM));

	fo = NULL;
	if (atp != NULL)
		at = *atp;
	else {
		fo = of->of_foff;
		mutex_lock(&fo->fo_lock);
		at = fo->fo_off;
	}
	err = 0;
	for (done = 0; done < n; done += chunk) {
		chunk = n - done;
		if (chunk > DARWIN_READ_CHUNK)
			chunk = DARWIN_READ_CHUNK;
		got = 0;
		rv = fs_pread(&of->of_handle, at + done, bounce,
		    (uint32_t)chunk, &got);
		if (rv != FS_E_OK) {
			/*
			 * EIO, except for a /.xid descriptor whose checkpoint
			 * has left the window: ESTALE.
			 */
			err = rv == FS_E_GONE ? DARWIN_ESTALE : DARWIN_EIO;
			break;
		}
		if (got == 0)			/* end of file, short read */
			break;
		if (syscall_copyout((uint8_t *)ubuf + done, bounce,
		    got) != 0) {
			err = DARWIN_EFAULT;
			break;
		}
		if (got < chunk) {
			done += got;
			break;
		}
	}
	if (fo != NULL) {
		if (err == 0)
			fo->fo_off = at + done;
		mutex_unlock(&fo->fo_lock);
	}
	kfree(bounce);
	if (err != 0)
		return (darwin_err(f, err));
	of->of_size = (uint32_t)of->of_handle.fh_size;	/* fs_pread's refresh */
	return (darwin_ok(f, (long)done));
}

/*
 * A file's length now.  of_size is what this descriptor last saw; another
 * descriptor or process may have grown or cut the file since, so one on
 * the volume asks it (fs_length refreshes a stale handle).  A built-in
 * image never changes.
 */
static uint64_t
darwin_file_len(struct darwin_ofile *of)
{
	uint64_t	len;

	if (of->of_buf != NULL || of->of_handle.fh_kind == FS_HANDLE_NONE)
		return (of->of_size);
	if (fs_length(&of->of_handle, &len) != FS_E_OK)
		return (of->of_size);
	of->of_size = (uint32_t)len;
	return (len);
}

/*
 * Map an FS_E_* code to a Darwin errno.  FS_E_SPREAD (a file's records
 * split across two nodes, which the writer refuses to move) is not a full
 * disk, but ENOSPC -- room could not be found -- is the nearest true
 * answer; EIO would claim the volume is damaged.
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
	case FS_E_INVAL:	return (DARWIN_EINVAL);
	/* A /.xid checkpoint the free queue has since let go of. */
	case FS_E_GONE:		return (DARWIN_ESTALE);
	default:		return (DARWIN_EIO);
	}
}

/*
 * write(2) to a disk-backed fd, through a bounce buffer as in
 * darwin_file_read.  O_APPEND is resolved per call against the handle's
 * current length, not the open-time one, so two appenders do not overwrite
 * each other.  A write is short only if the filesystem shortened it.
 * pwrite(2) passes its offset in `atp`: the write lands there, O_APPEND or
 * not, as on Darwin, and the cursor stays put.
 */
#define	DARWIN_WRITE_CHUNK	(64u * 1024u)

static long
darwin_file_write(struct syscall_frame *f, struct darwin_ofile *of,
    const void *ubuf, size_t n, const uint64_t *atp)
{
	struct darwin_foff	*fo;
	uint8_t			*bounce;
	uint64_t		 at;
	size_t			 done;
	size_t			 chunk;
	uint32_t		 put;
	int			 err;
	int			 rv;

	if ((of->of_flags & DARWIN_O_ACCMODE) == DARWIN_O_RDONLY)
		return (darwin_err(f, DARWIN_EBADF));
	if (of->of_handle.fh_kind == FS_HANDLE_NONE)
		return (darwin_err(f, DARWIN_EROFS));
	if (n == 0)
		return (darwin_ok(f, 0));

	bounce = kmalloc(n < DARWIN_WRITE_CHUNK ? n : DARWIN_WRITE_CHUNK);
	if (bounce == NULL)
		return (darwin_err(f, DARWIN_ENOMEM));

	fo = NULL;
	if (atp != NULL)
		at = *atp;
	else {
		fo = of->of_foff;
		mutex_lock(&fo->fo_lock);
		/* An append's end is found under the fs lock, not here. */
		if ((of->of_flags & DARWIN_O_APPEND) != 0)
			at = FS_OFF_APPEND;
		else
			at = fo->fo_off;
	}
	err = 0;
	for (done = 0; done < n; done += put) {
		chunk = n - done;
		if (chunk > DARWIN_WRITE_CHUNK)
			chunk = DARWIN_WRITE_CHUNK;
		if (syscall_copyin(bounce, (const uint8_t *)ubuf + done,
		    chunk) != 0) {
			err = DARWIN_EFAULT;
			break;
		}
		/*
		 * An append puts each chunk at the end as it then stands: a
		 * write longer than a chunk may interleave with another
		 * appender's, but never lands on its bytes.
		 */
		put = 0;
		rv = fs_pwrite(&of->of_handle,
		    at == FS_OFF_APPEND ? FS_OFF_APPEND : at + done, bounce,
		    (uint32_t)chunk, &put);
		if (rv != FS_E_OK) {
			if (done != 0)
				break;		/* a short write, not an error */
			kprintf("darwin: write to '%s' refused (rv=%d)\n",
			    of->of_path != NULL ? of->of_path : "?", rv);
			err = darwin_fs_errno(rv);
			break;
		}
		if (put == 0)
			break;
	}
	if (err != 0 && done != 0)
		err = 0;			/* short, not failed */

	/*
	 * fs_pwrite has already grown fh_size if the write ran off the end;
	 * an append leaves the cursor there.
	 */
	if (fo != NULL) {
		if (err == 0)
			fo->fo_off = at == FS_OFF_APPEND ?
			    of->of_handle.fh_size : at + done;
		mutex_unlock(&fo->fo_lock);
	}
	kfree(bounce);
	if (err != 0)
		return (darwin_err(f, err));
	of->of_size = (uint32_t)of->of_handle.fh_size;
	return (darwin_ok(f, (long)done));
}

/* Copy `src` into `dst[cap]`, cut short and always terminated. */
static void
darwin_strfill(char *dst, const char *src, size_t cap)
{
	size_t	i;

	for (i = 0; i + 1 < cap && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/*
 * statfs(2) and fstatfs(2), 64-bit-inode form: the one mounted volume, seen
 * from `path` -- read-only under /.xid.  Built in kernel memory, as the
 * struct is too big for this stack, and copied out whole.
 */
static long
darwin_statfs_out(struct syscall_frame *f, const char *path, void *ubuf)
{
	struct darwin_statfs64	*sf;
	uint64_t		 blocks;
	uint64_t		 bfree;
	uint32_t		 bsize;
	long			 rv;

	sf = kcalloc(1, sizeof(*sf));
	if (sf == NULL)
		return (darwin_err(f, DARWIN_ENOMEM));
	(void)fs_space(&bsize, &blocks, &bfree);
	sf->f_bsize     = bsize != 0 ? bsize : 4096;
	sf->f_iosize    = (int32_t)sf->f_bsize;
	sf->f_blocks    = blocks;
	sf->f_bfree     = bfree;
	sf->f_bavail    = bfree;
	sf->f_fsid[0]   = 1;
	if (path != NULL && fs_readonly(path))
		sf->f_flags = DARWIN_MNT_RDONLY;
	darwin_strfill(sf->f_fstypename, fs_kind(), sizeof(sf->f_fstypename));
	darwin_strfill(sf->f_mntonname, "/", sizeof(sf->f_mntonname));
	darwin_strfill(sf->f_mntfromname, "/dev/disk0s1",
	    sizeof(sf->f_mntfromname));

	rv = syscall_copyout(ubuf, sf, sizeof(*sf));
	kfree(sf);
	if (rv != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 0));
}

/*
 * Release whatever one slot of task t holds and return it to FREE.  A file
 * takes t's record locks on it along.
 */
static void
darwin_ofile_clear(struct task *t, struct darwin_ofile *of)
{

	switch (of->of_type) {
	case DARWIN_OF_DIR:
	case DARWIN_OF_FILE:
		/*
		 * The one place a descriptor stops existing, so the file is
		 * closed here rather than in close(2): dup2 over a live slot,
		 * task teardown and exec end descriptors too.  A directory's
		 * handle is FS_HANDLE_NONE, which fs_close ignores.
		 */
		if (of->of_type == DARWIN_OF_FILE && darwin_lk_file(of) != 0)
			darwin_lk_drop(t->t_id, darwin_lk_file(of));
		(void)fs_close(&of->of_handle);
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
	if (of->of_foff != NULL)
		darwin_foff_drop(of->of_foff);
	of->of_pipe          = NULL;
	of->of_foff          = NULL;
	of->of_buf           = NULL;
	of->of_path          = NULL;
	of->of_handle.fh_kind = FS_HANDLE_NONE;
	of->of_handle.fh_id   = 0;
	of->of_handle.fh_size = 0;
	of->of_size          = 0;
	of->of_flags         = 0;
	of->of_type          = DARWIN_OF_FREE;
}

/*
 * Foreground task for console input: the id of the Darwin task that last
 * read(2) the console.  It is also the claim: while it names a live task,
 * the keyboard driver routes keys here instead of to the native shell's
 * Mach input port.  A stand-in for process groups.  Read without the lock;
 * a stale id only makes the following lookup fail.
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
 * One character arriving at the terminal: the line discipline.  Echo
 * happens here, as the key arrives, so a busy shell still shows what is
 * typed.  Editing (ICANON), echo (ECHO) and signals (ISIG) are governed
 * separately by their own flags; programs use every combination.
 *
 * Returns whether a wake is owed; the caller wakes the reader channel
 * (&darwin_cons_head) outside the lock.  The console keeps no thread
 * pointer, so a dying reader leaves nothing to clean up.
 */
static bool
darwin_cons_input_locked(char c, bool *intr_out)
{
	struct darwin_termios	*tio;

	*intr_out = false;
	tio = darwin_cons_tio_locked();

	/*
	 * Non-canonical: no editing, and a wake per character.  ISIG is
	 * independent of ICANON (a pager wants raw keys and a working Ctrl-C),
	 * so VINTR is tested first, from c_cc since a program may move it.
	 */
	if ((tio->c_lflag & DARWIN_ICANON) == 0) {
		if ((tio->c_lflag & DARWIN_ISIG) != 0 &&
		    (uint8_t)c == tio->c_cc[DARWIN_VINTR]) {
			darwin_cons_line_len = 0;
			*intr_out = true;
			return (true);
		}
		if (darwin_cons_line_len >= DARWIN_CONS_LINE)
			darwin_cons_deliver_locked();
		darwin_cons_line[darwin_cons_line_len++] = c;
		darwin_cons_deliver_locked();
		if ((tio->c_lflag & DARWIN_ECHO) != 0)
			tty_putc(c);
		return (true);
	}

	/*
	 * Canonical mode.  An if-chain rather than a switch because the
	 * special characters come from c_cc and may be moved.  ICRNL (the
	 * default) maps CR to newline first; with it clear, only LF ends a
	 * line.
	 */
	if (c == '\r' && (tio->c_iflag & DARWIN_ICRNL) != 0)
		c = '\n';

	if ((tio->c_lflag & DARWIN_ISIG) != 0 &&
	    (uint8_t)c == tio->c_cc[DARWIN_VINTR]) {
		/*
		 * Ctrl-C discards the typed line, so it cannot reach the next
		 * prompt, and signals.  It also returns true to wake the
		 * reader: posting a signal only sets a bit, and a reader
		 * parked at a prompt would otherwise carry the SIGINT out
		 * with the next command's read(2) and lose that command.
		 */
		darwin_cons_line_len = 0;
		tty_putc('^');
		tty_putc('C');
		tty_putc('\n');
		*intr_out = true;
	} else if ((uint8_t)c == tio->c_cc[DARWIN_VEOF]) {
		/*
		 * Ctrl-D delivers the line so far without a newline; on an
		 * empty line that is a zero-byte read, i.e. end-of-file.
		 */
		if (darwin_cons_line_len == 0)
			darwin_cons_eof = true;
		else
			darwin_cons_deliver_locked();
	} else if ((uint8_t)c == tio->c_cc[DARWIN_VERASE] ||
	    c == DARWIN_CONS_ERASE) {
		/*
		 * VERASE is DEL by default, but the keyboard driver sends
		 * backspace for the Backspace key; both erase.
		 */
		if (darwin_cons_line_len == 0)
			return (false);		/* nothing to rub out */
		darwin_cons_line_len--;
		if ((tio->c_lflag & DARWIN_ECHOE) != 0) {
			tty_putc('\b');
			tty_putc(' ');
			tty_putc('\b');
		}
		return (false);
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
		return (false);
	} else if (c == '\n') {
		if (darwin_cons_line_len < DARWIN_CONS_LINE)
			darwin_cons_line[darwin_cons_line_len++] = '\n';
		if ((tio->c_lflag & (DARWIN_ECHO | DARWIN_ECHONL)) != 0)
			tty_putc('\n');
		darwin_cons_deliver_locked();
	} else {
		if (c < 0x20 && c != '\t')
			return (false);		/* not a key we render */
		if (darwin_cons_line_len >= DARWIN_CONS_LINE) {
			/* Deliver an overlong line in pieces, not truncated. */
			darwin_cons_deliver_locked();
		}
		darwin_cons_line[darwin_cons_line_len++] = c;
		if ((tio->c_lflag & DARWIN_ECHO) != 0)
			tty_putc(c);
		return (false);
	}

	return (true);
}

/*
 * Push one character in from a producer.  Returns true if the console took
 * it (currently always).
 */
bool
darwin_cons_input(char c)
{
	bool	owed;
	bool	intr;

	spin_lock(&darwin_cons_lock);
	darwin_cons_n_key++;
	owed = darwin_cons_input_locked(c, &intr);
	spin_unlock(&darwin_cons_lock);

	/* Both of these can sleep or take other locks; neither may run above. */
	if (intr)
		darwin_cons_signal_fg(DARWIN_SIGINT);
	if (owed) {
		(void)sched_wakeup(&darwin_cons_head);
		darwin_select_news();
	}
	return (true);
}

/*
 * The keyboard driver's sink: if a Darwin task holds the console, take the
 * key; otherwise return false and the driver sends it to the Mach input
 * port.  Nothing else arbitrates the keyboard.
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
		 * The claimant died unreleased.  Teardown normally clears the
		 * claim; dropping it here too keeps the keyboard from wedging.
		 */
		darwin_cons_fg_id = 0;
		return (false);
	}
	task_deref(fg);
	return (darwin_cons_input(c));
}

/*
 * Release the console if `t` was holding it.  Called when a task dies; a
 * claim that outlived its owner would route every key into a ring nobody
 * drains.
 */
void
darwin_cons_release(struct task *t)
{

	if (t == NULL || t->t_id != darwin_cons_fg_id)
		return;
	darwin_cons_fg_id = 0;
	spin_lock(&darwin_cons_lock);
	darwin_cons_line_len   = 0;
	darwin_cons_tail       = darwin_cons_head;  /* discard unread input */
	darwin_cons_eof        = false;	/* the next claimant starts fresh */
	darwin_cons_script_len = 0;	/* a script belongs to its session */
	darwin_cons_script_off = 0;
	/* Settings too: see darwin_cons_tio_default. */
	darwin_cons_tio_default(darwin_cons_tio_locked());
	spin_unlock(&darwin_cons_lock);

	/*
	 * Session end is the moment to print what the session cost; the
	 * boot-time stats block printed long before anyone typed.
	 */
	darwin_cons_stats();
	darwin_wait_stats();
	sched_wake_latency_print();
	cpu_dump();
	lapic_timer_report();
}

/*
 * Load a scripted session (SYS_CONS_FEED).  The bytes are held until a
 * reader asks, then released a line at a time (darwin_cons_script_locked).
 * Running out of script is end-of-input, so a scripted shell exits.
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
	darwin_select_news();
}

/*
 * Release the next scripted line into the discipline, or declare
 * end-of-input once the script is spent.  Returns false when there was
 * nothing to type and the caller should wait for a real key.  *intr_out
 * reports a scripted Ctrl-C, which the caller signals after unlocking.
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
		 * The owed wake is dropped: the reader is the thread running
		 * this, and it will find the line itself.
		 */
		(void)darwin_cons_input_locked(c, &intr);
		if (intr)
			*intr_out = true;
	} while (c != '\n' && darwin_cons_script_off < darwin_cons_script_len);
	return (true);
}

/*
 * read(2) on a console fd (implicit stdin or a CONSOLE slot).  The
 * discipline has already echoed and split lines, so this only moves bytes
 * and waits, parked on &darwin_cons_head until a producer wakes it.
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
		 * How little will do.  In canonical mode the ring holds only
		 * whole lines, so one byte is enough; otherwise VMIN says (0
		 * means do not wait).  VTIME is stored and reported but not
		 * honoured: VMIN=0 VTIME=5 polls instead of waiting 0.5 s.
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

		/* Nothing queued: a loaded script types its next line now. */
		if (darwin_cons_script_locked(&intr)) {
			spin_unlock(&darwin_cons_lock);
			if (intr)
				darwin_cons_signal_fg(DARWIN_SIGINT);
			continue;
		}

		/*
		 * A pending, unblocked signal breaks the read with EINTR and
		 * the syscall-exit path delivers it.  Tested before parking, or
		 * a signal that arrived while awake would be slept through.
		 */
		if (darwin_signal_pending(current_thread->th_task)) {
			spin_unlock(&darwin_cons_lock);
			return (darwin_err(f, DARWIN_EINTR));
		}

		/*
		 * Park on the ring's head.  thread_block_release drops the
		 * lock under the scheduler lock, so a producer's wake cannot
		 * fall between the unlock and the block.
		 */
		darwin_cons_n_wait++;
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
 * The mode a create actually gets: the requested permission bits (low
 * twelve; any type bits are the filesystem's business) less this task's
 * umask.
 */
static uint16_t
darwin_mode_arg(long raw)
{
	struct task	*t;

	t = current_thread->th_task;
	return ((uint16_t)((uint32_t)raw & 07777u & ~(uint32_t)t->t_darwin_umask));
}

/*
 * ioctl(2) on the terminal, the only device a program here can hold.
 * ENOTTY on a file or pipe is the answer isatty(3) is built on (gls asks
 * TIOCGWINSZ to decide whether to print in columns).  `arg` is a user
 * pointer for every request; a bad one gets EFAULT from the copy.
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
		 * SETA applies now; SETAW would first drain output, of which
		 * none is buffered; SETAF also discards queued input, typed
		 * under the old rules.
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
		 * The text-mode console's real size, TTY_COLS x TTY_ROWS;
		 * pixel fields zero, as for any non-graphical terminal.
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
		 * Refused: the size is the CRT controller's, and every later
		 * TIOCGWINSZ would contradict an accepted value.
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
		/* ENOTTY for an unknown request, as Unix answers. */
		kprintf("darwin: ioctl(%#lx) on the terminal is not one we "
		    "answer\n", req);
		return (darwin_err(f, DARWIN_ENOTTY));
	}
}

/*
 * One line of terminal counters.  About one wait per read means the reader
 * sleeps until something happens.
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

/*
 * The same for select, pipes and wait4.  Waits far above calls means a
 * waiter is spinning, not sleeping.
 */
void
darwin_wait_stats(void)
{

	darwin_select_stats();
	darwin_lk_stats();
	if (darwin_pipe_n_read == 0 && darwin_pipe_n_write == 0 &&
	    darwin_wait_n_call == 0)
		return;
	kprintf("pipe: %llu reads (%llu waits), %llu writes (%llu waits)\n",
	    (unsigned long long)darwin_pipe_n_read,
	    (unsigned long long)darwin_pipe_n_rwait,
	    (unsigned long long)darwin_pipe_n_write,
	    (unsigned long long)darwin_pipe_n_wwait);
	kprintf("wait: %llu wait4(2) reaped, %llu waits, %llu on the deadline "
	    "(a slow child, not a fault), %llu explained by a lost wake "
	    "(want 0)\n",
	    (unsigned long long)darwin_wait_n_call,
	    (unsigned long long)darwin_wait_n_wait,
	    (unsigned long long)darwin_wait_n_net,
	    (unsigned long long)darwin_wait_n_lost);
	kprintf("wait: %llu news told, %llu parent(s) actually woken by it\n",
	    (unsigned long long)darwin_wait_n_told,
	    (unsigned long long)darwin_wait_n_woke);
	/*
	 * A FAIL, since the count is exact and load-independent: some path
	 * out of a Darwin task does not call darwin_child_news.
	 */
	if (darwin_wait_n_lost != 0)
		kprintf("wait: FAIL %llu answer(s) reached a parent only "
		    "because of the deadline -- a wake is missing\n",
		    (unsigned long long)darwin_wait_n_lost);
}

/* ---- readiness: select(2), pselect, poll(2) ------------------------------ */

/*
 * Readiness for select(2), pselect and poll(2).  Rather than a wait queue
 * per file (BSD's selrecord), there is one channel, darwin_select_gen:
 * every producer rings it after its own wake, and a selector that finds
 * nothing ready parks there and rescans.  A spurious wake costs one scan
 * of at most DARWIN_NOFILE slots.
 *
 * The generation count makes it race-free.  A producer bumps it under
 * darwin_select_lock; a selector reads it, scans without the lock (the
 * scan takes pipe and console locks, which must not nest under it), then
 * locks and compares.  A bump means rescan; otherwise it parks with the
 * lock held and thread_block_release drops it under the scheduler lock,
 * so no bump-and-wake can fall in the gap.
 *
 * Answers: a disk file, directory or /dev/null is always ready both ways.
 * A pipe's read end is readable with bytes or no writers left; its write
 * end writable with room or no readers left.  The console is always
 * writable, and readable with input queued, at end-of-input, or with a
 * script loaded (a read releases its next line).  No exceptional
 * conditions are ever reported.  A raw-mode VMIN above one reads as ready
 * from the first byte, as BSD's ttnread does; the read may still wait.
 */
#define	DARWIN_RD	0x1u		/* would not block reading   */
#define	DARWIN_WR	0x2u		/* would not block writing   */
#define	DARWIN_HUP	0x4u		/* a reader's writers are gone */
#define	DARWIN_ERR	0x8u		/* a writer's readers are gone */

#define	DARWIN_POLL_MAX	64		/* pollfds one call may carry */

static struct spinlock	darwin_select_lock = SPINLOCK_INIT("dselect");
static uint64_t		darwin_select_gen;	  /* (s) bumped per event   */
static uint64_t		darwin_select_n_call;	  /* select/pselect/poll    */
static uint64_t		darwin_select_n_wait;	  /* parks                  */
static uint64_t		darwin_select_n_timeout;  /* parks the clock ended  */
static uint64_t		darwin_select_n_intr;	  /* waits a signal ended   */

/* A producer's duty: bump the generation, then wake any selector. */
static void
darwin_select_news(void)
{

	spin_lock(&darwin_select_lock);
	darwin_select_gen++;
	spin_unlock(&darwin_select_lock);
	(void)sched_wakeup(&darwin_select_gen);
}

static void
darwin_select_stats(void)
{

	if (darwin_select_n_call == 0)
		return;
	kprintf("select: %llu call(s), %llu park(s), %llu ended by the clock, "
	    "%llu by a signal\n",
	    (unsigned long long)darwin_select_n_call,
	    (unsigned long long)darwin_select_n_wait,
	    (unsigned long long)darwin_select_n_timeout,
	    (unsigned long long)darwin_select_n_intr);
}

/*
 * What one descriptor would answer now.  `want` is DARWIN_RD/WR; the return
 * is the subset that would not block, plus DARWIN_HUP/ERR for a pipe end
 * whose other side is gone (poll(2) reports those; select(2) folds them
 * into readable/writable).  A slot with nothing open sets *bad (EBADF) and
 * returns 0.
 */
static uint32_t
darwin_fd_ready(struct task *t, int fd, uint32_t want, bool *bad)
{
	struct darwin_ofile	*of;
	struct darwin_pipe	*p;
	uint32_t		 got;
	uint8_t			 type;

	*bad = false;
	if (fd < 0 || fd >= DARWIN_NOFILE) {
		*bad = true;
		return (0);
	}
	of   = &t->t_darwin_files[fd];
	type = of->of_type;
	if (type == DARWIN_OF_FREE) {
		if (fd > 2) {
			*bad = true;
			return (0);
		}
		type = DARWIN_OF_CONSOLE;	/* the implicit std streams */
	}
	got = 0;
	switch (type) {
	case DARWIN_OF_FILE:
	case DARWIN_OF_DIR:
	case DARWIN_OF_NULL:
		got = want;
		break;
	case DARWIN_OF_CONSOLE:
		if ((want & DARWIN_WR) != 0)
			got |= DARWIN_WR;
		if ((want & DARWIN_RD) != 0) {
			spin_lock(&darwin_cons_lock);
			if (darwin_cons_head != darwin_cons_tail ||
			    darwin_cons_eof || darwin_cons_script_len != 0)
				got |= DARWIN_RD;
			spin_unlock(&darwin_cons_lock);
		}
		break;
	case DARWIN_OF_PIPE_R:
		p = of->of_pipe;
		spin_lock(&p->p_lock);
		if (p->p_writers == 0)
			got |= DARWIN_HUP;
		if ((want & DARWIN_RD) != 0 &&
		    (p->p_count > 0 || p->p_writers == 0))
			got |= DARWIN_RD;
		spin_unlock(&p->p_lock);
		break;
	case DARWIN_OF_PIPE_W:
		p = of->of_pipe;
		spin_lock(&p->p_lock);
		if (p->p_readers == 0)
			got |= DARWIN_ERR;
		if ((want & DARWIN_WR) != 0 &&
		    (p->p_count < DARWIN_PIPE_BUF || p->p_readers == 0))
			got |= DARWIN_WR;
		spin_unlock(&p->p_lock);
		break;
	default:
		*bad = true;
		break;
	}
	return (got);
}

/*
 * An absolute deadline `ms` from now, in the tick clock the timed waiter is
 * checked against, plus one tick (Unix's tvtohz rule): "now" in whole
 * ticks is up to a tick stale, and a timeout must never return early.
 *
 * Not from the microsecond clock: that is ticks plus a TSC delta, and
 * under a hypervisor lost ticks make it drift from the tick clock by
 * seconds.  A deadline must be in the clock that judges it.  Never 0,
 * which callers use for "no deadline".
 */
static uint64_t
darwin_deadline_ms(uint64_t ms)
{
	uint64_t	d;
	uint64_t	tick;

	tick = 1000u / clock_hz();
	if (tick == 0)
		tick = 1;
	d = clock_uptime_ms() + ms + tick;
	return (d == 0 ? 1 : d);
}

/*
 * Wait until something is ready, the deadline passes, or a signal arrives.
 * `scan` is the caller's readiness pass over its own list; it returns how
 * many answers it found (or a negative errno) and is run again after every
 * wake.  `deadline_ms` is an absolute uptime, 0 for none; `once` is a
 * zero timeout -- one scan, no park.  Returns the scan's count, 0 on the
 * deadline, or -EINTR.
 *
 * The signal test runs with nothing held; that is safe because posting a
 * signal wakes the target, and a wake that finds nobody asleep leaves
 * th_wake_pending for the next park (kern/sched.c).
 */
static long
darwin_readiness_wait(long (*scan)(void *), void *arg, uint64_t deadline_ms,
    bool once)
{
	struct task	*t;
	uint64_t	 gen;
	long		 n;

	t = current_thread->th_task;
	for (;;) {
		spin_lock(&darwin_select_lock);
		gen = darwin_select_gen;
		spin_unlock(&darwin_select_lock);
		n = scan(arg);
		if (n != 0)
			return (n);		/* ready, or an error */
		if (once)
			return (0);
		if (deadline_ms != 0 && clock_uptime_ms() >= deadline_ms) {
			darwin_select_n_timeout++;
			return (0);
		}
		if (task_kill_pending(t) || darwin_signal_pending(t)) {
			darwin_select_n_intr++;
			return (-DARWIN_EINTR);
		}
		spin_lock(&darwin_select_lock);
		if (darwin_select_gen != gen) {
			spin_unlock(&darwin_select_lock);
			continue;		/* news since the scan */
		}
		if (deadline_ms != 0) {
			current_thread->th_wake_deadline_ms = deadline_ms;
			sched_add_timed_waiter(current_thread);
		}
		darwin_select_n_wait++;
		thread_block_release(THREAD_BLOCK_SLEEP, &darwin_select_gen,
		    &darwin_select_lock);
		if (deadline_ms != 0) {
			sched_remove_timed_waiter(current_thread);
			current_thread->th_wake_deadline_ms = 0;
		}
	}
}

/* ---- record locks: fcntl(2) F_GETLK, F_SETLK, F_SETLKW ------------------- */

/*
 * POSIX record locks.  One list for the machine, under darwin_lk_lock:
 * each entry is one process's lock on one byte range of one file, the
 * file named by its inode.  A process's own locks never conflict; a new
 * one replaces whatever of its own it overlaps, splitting what sticks out,
 * so one owner's entries on a file never overlap each other.  Closing any
 * descriptor on a file drops all of the closer's locks on it, and exit
 * drops the rest, as POSIX has it.
 *
 * F_SETLKW waits on the readiness channel, which every release rings.
 * There is no deadlock detection: two processes waiting on each other
 * wait until a signal.
 */
#define	DARWIN_LK_MAX	256			/* entries, machine-wide */
#define	DARWIN_LK_EOF	UINT64_MAX		/* "however far" */

struct darwin_lk {
	struct darwin_lk	*lk_next;
	uint64_t		 lk_file;	/* inode */
	uint64_t		 lk_owner;	/* task id */
	uint64_t		 lk_start;
	uint64_t		 lk_end;	/* inclusive */
	int			 lk_type;	/* DARWIN_F_RDLCK or _WRLCK */
};

/* One request, in absolute bytes, with the entries it may need. */
struct darwin_lk_req {
	struct darwin_lk	*lr_split;	/* spare: a range cut in two */
	struct darwin_lk	*lr_new;	/* spare: the lock itself */
	uint64_t		 lr_file;
	uint64_t		 lr_owner;
	uint64_t		 lr_start;
	uint64_t		 lr_end;
	int			 lr_type;
};

static struct spinlock	 darwin_lk_lock = SPINLOCK_INIT("dlk");
static struct darwin_lk	*darwin_lk_head;	/* (k) */
static uint32_t		 darwin_lk_count;	/* (k) entries */
static uint32_t		 darwin_lk_peak;	/* (k) most entries at once */
static uint64_t		 darwin_lk_n_set;	/* (k) F_SETLK(W)s granted */
static uint64_t		 darwin_lk_n_busy;	/* (k) tries found blocked */
static uint64_t		 darwin_lk_n_wait;	/* F_SETLKWs that had to wait */

/*
 * The key a descriptor's file is locked under: its inode.  0 -- a built-in
 * /bin image, a copy per open -- cannot be locked.
 */
static uint64_t
darwin_lk_file(const struct darwin_ofile *of)
{

	return (of->of_handle.fh_ino);
}

static bool
darwin_lk_overlaps(const struct darwin_lk *lk, const struct darwin_lk_req *r)
{

	return (lk->lk_file == r->lr_file && lk->lk_start <= r->lr_end &&
	    r->lr_start <= lk->lk_end);
}

/* Another owner's lock the request cannot coexist with.  (k) */
static struct darwin_lk *
darwin_lk_conflict(const struct darwin_lk_req *r)
{
	struct darwin_lk	*lk;

	for (lk = darwin_lk_head; lk != NULL; lk = lk->lk_next) {
		if (lk->lk_owner == r->lr_owner || !darwin_lk_overlaps(lk, r))
			continue;
		if (lk->lk_type == DARWIN_F_WRLCK ||
		    r->lr_type == DARWIN_F_WRLCK)
			return (lk);
	}
	return (NULL);
}

/*
 * Take [lr_start, lr_end] out of the owner's entries on the file, then
 * put the new lock in if the request is one.  Entries wholly inside go to
 * *dead, for freeing after the unlock; one sticking out at both ends
 * splits, and only one can, since the owner's entries do not overlap.
 * True if any of the owner's entries changed -- a waiter may now fit.  (k)
 */
static bool
darwin_lk_apply(struct darwin_lk_req *r, struct darwin_lk **dead)
{
	struct darwin_lk	**pp;
	struct darwin_lk	*lk;
	struct darwin_lk	*tail;
	bool			 changed;

	changed = false;
	pp = &darwin_lk_head;
	while ((lk = *pp) != NULL) {
		if (lk->lk_owner != r->lr_owner || !darwin_lk_overlaps(lk, r)) {
			pp = &lk->lk_next;
			continue;
		}
		changed = true;
		if (lk->lk_start >= r->lr_start && lk->lk_end <= r->lr_end) {
			*pp = lk->lk_next;
			lk->lk_next = *dead;
			*dead = lk;
			darwin_lk_count--;
			continue;
		}
		if (lk->lk_start < r->lr_start && lk->lk_end > r->lr_end) {
			KASSERT(r->lr_split != NULL, ("darwin_lk: two splits"));
			tail = r->lr_split;
			r->lr_split = NULL;
			*tail = *lk;
			tail->lk_start = r->lr_end + 1;
			lk->lk_end = r->lr_start - 1;
			lk->lk_next = tail;
			darwin_lk_count++;
			pp = &tail->lk_next;
			continue;
		}
		if (lk->lk_start < r->lr_start)
			lk->lk_end = r->lr_start - 1;
		else
			lk->lk_start = r->lr_end + 1;
		pp = &lk->lk_next;
	}
	if (r->lr_type != DARWIN_F_UNLCK) {
		lk = r->lr_new;
		r->lr_new = NULL;
		lk->lk_file  = r->lr_file;
		lk->lk_owner = r->lr_owner;
		lk->lk_start = r->lr_start;
		lk->lk_end   = r->lr_end;
		lk->lk_type  = r->lr_type;
		lk->lk_next  = darwin_lk_head;
		darwin_lk_head = lk;
		darwin_lk_count++;
	}
	if (darwin_lk_count > darwin_lk_peak)
		darwin_lk_peak = darwin_lk_count;
	return (changed);
}

static void
darwin_lk_free(struct darwin_lk *dead)
{
	struct darwin_lk	*next;

	for (; dead != NULL; dead = next) {
		next = dead->lk_next;
		kfree(dead);
	}
}

/*
 * One attempt at F_SETLK: 1 granted, 0 blocked by another owner, or a
 * negative errno.  Also F_SETLKW's scan for darwin_readiness_wait.
 */
static long
darwin_lk_try(void *arg)
{
	struct darwin_lk_req	*r;
	struct darwin_lk	*dead;
	bool			 changed;

	r = arg;
	dead = NULL;
	spin_lock(&darwin_lk_lock);
	if (r->lr_type != DARWIN_F_UNLCK) {
		if (darwin_lk_conflict(r) != NULL) {
			darwin_lk_n_busy++;
			spin_unlock(&darwin_lk_lock);
			return (0);
		}
		/* An unlock is never refused, though it may split. */
		if (darwin_lk_count + 2 > DARWIN_LK_MAX) {
			spin_unlock(&darwin_lk_lock);
			return (-DARWIN_ENOLCK);
		}
	}
	changed = darwin_lk_apply(r, &dead);
	darwin_lk_n_set++;
	spin_unlock(&darwin_lk_lock);
	darwin_lk_free(dead);
	if (changed)
		darwin_select_news();
	return (1);
}

/* Drop what `owner` holds on `file`, or everywhere if `file` is 0. */
static void
darwin_lk_drop(uint64_t owner, uint64_t file)
{
	struct darwin_lk	**pp;
	struct darwin_lk	*lk;
	struct darwin_lk	*dead;

	dead = NULL;
	spin_lock(&darwin_lk_lock);
	pp = &darwin_lk_head;
	while ((lk = *pp) != NULL) {
		if (lk->lk_owner == owner &&
		    (file == 0 || lk->lk_file == file)) {
			*pp = lk->lk_next;
			lk->lk_next = dead;
			dead = lk;
			darwin_lk_count--;
			continue;
		}
		pp = &lk->lk_next;
	}
	spin_unlock(&darwin_lk_lock);
	if (dead != NULL) {
		darwin_lk_free(dead);
		darwin_select_news();
	}
}

/*
 * Entries still held can only be live processes': every exit drops its
 * owner's.  Read without the lock; a statistic.
 */
static void
darwin_lk_stats(void)
{

	if (darwin_lk_n_set == 0 && darwin_lk_n_busy == 0)
		return;
	kprintf("locks: %llu record lock(s) set, %llu found busy, "
	    "%llu F_SETLKW wait(s), peak %u entries, %u held now\n",
	    (unsigned long long)darwin_lk_n_set,
	    (unsigned long long)darwin_lk_n_busy,
	    (unsigned long long)darwin_lk_n_wait,
	    (unsigned)darwin_lk_peak, (unsigned)darwin_lk_count);
}

/*
 * fcntl(fd, F_GETLK / F_SETLK / F_SETLKW, struct flock *).  The range is
 * l_start from l_whence's base, l_len bytes on (before, if negative; to
 * the end however far, if 0).  A read lock needs a descriptor open for
 * reading, a write lock one open for writing.
 */
static long
darwin_fcntl_lock(struct syscall_frame *f, struct task *t, int fd, int cmd,
    void *uflock)
{
	struct darwin_flock	 fl;
	struct darwin_lk_req	 r;
	struct darwin_ofile	*of;
	struct darwin_lk	*lk;
	uint64_t		 base;
	int64_t			 start;
	int64_t			 len;
	uint32_t		 acc;
	long			 rv;

	of = &t->t_darwin_files[fd];
	if (of->of_type != DARWIN_OF_FILE)
		return (darwin_err(f, of->of_type == DARWIN_OF_FREE ?
		    DARWIN_EBADF : DARWIN_EINVAL));
	if (syscall_copyin(&fl, uflock, sizeof(fl)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));

	switch (fl.l_whence) {
	case 0:
		base = 0;
		break;
	case 1:
		mutex_lock(&of->of_foff->fo_lock);
		base = of->of_foff->fo_off;
		mutex_unlock(&of->of_foff->fo_lock);
		break;
	case 2:
		base = of->of_handle.fh_size;
		break;
	default:
		return (darwin_err(f, DARWIN_EINVAL));
	}
	if (fl.l_start > INT64_MAX - (int64_t)base)
		return (darwin_err(f, DARWIN_EINVAL));
	start = (int64_t)base + fl.l_start;
	len = fl.l_len;
	if (len < 0) {
		if (len == INT64_MIN)
			return (darwin_err(f, DARWIN_EINVAL));
		start += len;
		len = -len;
	}
	if (start < 0 || (len != 0 && len - 1 > INT64_MAX - start))
		return (darwin_err(f, DARWIN_EINVAL));
	if (fl.l_type != DARWIN_F_RDLCK && fl.l_type != DARWIN_F_WRLCK &&
	    fl.l_type != DARWIN_F_UNLCK)
		return (darwin_err(f, DARWIN_EINVAL));

	r.lr_split = NULL;
	r.lr_new   = NULL;
	r.lr_file  = darwin_lk_file(of);
	r.lr_owner = t->t_id;
	r.lr_start = (uint64_t)start;
	r.lr_end   = len == 0 ? DARWIN_LK_EOF : (uint64_t)(start + len - 1);
	r.lr_type  = fl.l_type;
	if (r.lr_file == 0)
		return (darwin_err(f, DARWIN_EINVAL));

	if (cmd == DARWIN_F_GETLK) {
		if (r.lr_type == DARWIN_F_UNLCK)
			return (darwin_err(f, DARWIN_EINVAL));
		spin_lock(&darwin_lk_lock);
		lk = darwin_lk_conflict(&r);
		if (lk != NULL) {
			fl.l_type   = (int16_t)lk->lk_type;
			fl.l_whence = 0;
			fl.l_start  = (int64_t)lk->lk_start;
			fl.l_len    = lk->lk_end == DARWIN_LK_EOF ? 0 :
			    (int64_t)(lk->lk_end - lk->lk_start + 1);
			fl.l_pid    = (int32_t)lk->lk_owner;
		} else
			fl.l_type = DARWIN_F_UNLCK;
		spin_unlock(&darwin_lk_lock);
		if (syscall_copyout(uflock, &fl, sizeof(fl)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		return (darwin_ok(f, 0));
	}

	acc = of->of_flags & DARWIN_O_ACCMODE;
	if ((r.lr_type == DARWIN_F_RDLCK && acc == DARWIN_O_WRONLY) ||
	    (r.lr_type == DARWIN_F_WRLCK && acc == DARWIN_O_RDONLY))
		return (darwin_err(f, DARWIN_EBADF));

	r.lr_split = kmalloc(sizeof(*r.lr_split));
	r.lr_new   = kmalloc(sizeof(*r.lr_new));
	if (r.lr_split == NULL || r.lr_new == NULL) {
		rv = -DARWIN_ENOLCK;
		goto out;
	}
	rv = darwin_lk_try(&r);
	if (rv == 0 && cmd == DARWIN_F_SETLKW) {
		__atomic_fetch_add(&darwin_lk_n_wait, 1, __ATOMIC_RELAXED);
		rv = darwin_readiness_wait(darwin_lk_try, &r, 0, false);
	}
out:
	if (r.lr_split != NULL)
		kfree(r.lr_split);
	if (r.lr_new != NULL)
		kfree(r.lr_new);
	if (rv < 0)
		return (darwin_err(f, (int)-rv));
	if (rv == 0)
		return (darwin_err(f, DARWIN_EAGAIN));
	return (darwin_ok(f, 0));
}

/*
 * select(2)'s three bit sets, in and out.  FD_SETSIZE bits each, in 32-bit
 * words; only the words nfds reaches are read or written.
 */
struct darwin_select_scan {
	struct task	*ss_task;
	uint32_t	 ss_want[3][DARWIN_FD_SETSIZE / DARWIN_NFDBITS];
	uint32_t	 ss_got[3][DARWIN_FD_SETSIZE / DARWIN_NFDBITS];
	size_t		 ss_words;
	int		 ss_nfds;
	bool		 ss_has[3];
};

static bool
darwin_fdset_isset(const uint32_t *set, int fd)
{

	return ((set[fd / DARWIN_NFDBITS] &
	    ((uint32_t)1 << (fd % DARWIN_NFDBITS))) != 0);
}

static void
darwin_fdset_set(uint32_t *set, int fd)
{

	set[fd / DARWIN_NFDBITS] |= (uint32_t)1 << (fd % DARWIN_NFDBITS);
}

static long
darwin_select_scan(void *arg)
{
	struct darwin_select_scan	*ss;
	uint32_t			 got;
	uint32_t			 want;
	size_t				 i;
	long				 n;
	int				 fd;
	int				 k;
	bool				 bad;

	ss = arg;
	for (k = 0; k < 3; k++)
		for (i = 0; i < ss->ss_words; i++)
			ss->ss_got[k][i] = 0;
	n = 0;
	for (fd = 0; fd < ss->ss_nfds; fd++) {
		want = 0;
		if (ss->ss_has[0] && darwin_fdset_isset(ss->ss_want[0], fd))
			want |= DARWIN_RD;
		if (ss->ss_has[1] && darwin_fdset_isset(ss->ss_want[1], fd))
			want |= DARWIN_WR;
		if (want == 0 && !(ss->ss_has[2] &&
		    darwin_fdset_isset(ss->ss_want[2], fd)))
			continue;
		got = darwin_fd_ready(ss->ss_task, fd, want, &bad);
		if (bad)
			return (-DARWIN_EBADF);
		if ((got & DARWIN_RD) != 0) {
			darwin_fdset_set(ss->ss_got[0], fd);
			n++;
		}
		if ((got & DARWIN_WR) != 0) {
			darwin_fdset_set(ss->ss_got[1], fd);
			n++;
		}
	}
	return (n);
}

/*
 * The shared body of select(2) and pselect: (nfds, in, out, except,
 * timeout [, sigmask]) in arg0..5.  `by_spec` means a timespec timeout and
 * an arg5 mask rather than a timeval.  A NULL timeout waits for ever, a
 * zero one scans once.  On return each set holds its ready subset (all
 * clear on timeout); the timeout is not written back.
 *
 * pselect's mask makes unblocking and waiting one operation, so a caller
 * that blocks SIGCHLD, checks its children and waits cannot miss a death.
 * On an EINTR exit the temporary mask stays installed and the delivery
 * path restores it (t_sig_mask_restore); restoring here would block the
 * very signal being delivered.
 */
static long
darwin_select_common(struct syscall_frame *f, bool by_spec)
{
	struct darwin_select_scan	ss;
	struct darwin_timespec		ts;
	struct darwin_timeval		tv;
	struct task			*t;
	uint64_t			 args[3];
	uint64_t			 deadline;
	uint64_t			 ms;
	uint32_t			 mask;
	uint32_t			 old_mask;
	long				 rv;
	int				 k;
	int				 nfds;
	bool				 masked;
	bool				 once;

	t    = current_thread->th_task;
	nfds = (int)f->sf_arg0;
	if (nfds < 0 || nfds > DARWIN_FD_SETSIZE)
		return (darwin_err(f, DARWIN_EINVAL));
	ss.ss_task  = t;
	ss.ss_nfds  = nfds;
	ss.ss_words = ((size_t)nfds + DARWIN_NFDBITS - 1) / DARWIN_NFDBITS;
	args[0] = f->sf_arg1;
	args[1] = f->sf_arg2;
	args[2] = f->sf_arg3;
	for (k = 0; k < 3; k++) {
		ss.ss_has[k] = args[k] != 0;
		if (!ss.ss_has[k] || ss.ss_words == 0)
			continue;
		if (syscall_copyin(ss.ss_want[k], (const void *)args[k],
		    ss.ss_words * sizeof(uint32_t)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
	}

	once     = false;
	deadline = 0;
	if (f->sf_arg4 != 0) {
		if (by_spec) {
			if (syscall_copyin(&ts, (const void *)f->sf_arg4,
			    sizeof(ts)) != 0)
				return (darwin_err(f, DARWIN_EFAULT));
			if (ts.ts_sec < 0 || ts.ts_nsec < 0 ||
			    ts.ts_nsec >= 1000000000LL)
				return (darwin_err(f, DARWIN_EINVAL));
			ms = (uint64_t)ts.ts_sec * 1000u +
			    ((uint64_t)ts.ts_nsec + 999999u) / 1000000u;
		} else {
			if (syscall_copyin(&tv, (const void *)f->sf_arg4,
			    sizeof(tv)) != 0)
				return (darwin_err(f, DARWIN_EFAULT));
			if (tv.tv_sec < 0 || tv.tv_usec < 0 ||
			    tv.tv_usec >= 1000000)
				return (darwin_err(f, DARWIN_EINVAL));
			ms = (uint64_t)tv.tv_sec * 1000u +
			    ((uint64_t)tv.tv_usec + 999u) / 1000u;
		}
		if (ms == 0)
			once = true;
		else
			deadline = darwin_deadline_ms(ms);
	}

	masked   = by_spec && f->sf_arg5 != 0;
	old_mask = t->t_sig_mask;
	if (masked) {
		if (syscall_copyin(&mask, (const void *)f->sf_arg5,
		    sizeof(mask)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		t->t_sig_mask = mask & ~darwin_sigbit(DARWIN_SIGKILL);
	}

	darwin_select_n_call++;
	rv = darwin_readiness_wait(darwin_select_scan, &ss, deadline, once);

	if (masked) {
		if (rv == -DARWIN_EINTR) {
			t->t_sig_mask_saved   = old_mask;
			t->t_sig_mask_restore = true;
		} else
			t->t_sig_mask = old_mask;
	}
	if (rv < 0)
		return (darwin_err(f, (int)-rv));

	for (k = 0; k < 3; k++) {
		if (!ss.ss_has[k] || ss.ss_words == 0)
			continue;
		if (syscall_copyout((void *)args[k], ss.ss_got[k],
		    ss.ss_words * sizeof(uint32_t)) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
	}
	return (darwin_ok(f, rv));
}

static long
darwin_s9_pselect(struct syscall_frame *f)
{

	return (darwin_select_common(f, true));
}

/* poll(2)'s array, in and out. */
struct darwin_poll_scan {
	struct task		*ps_task;
	struct darwin_pollfd	 ps_fds[DARWIN_POLL_MAX];
	uint32_t		 ps_nfds;
};

static long
darwin_poll_scan(void *arg)
{
	struct darwin_poll_scan	*ps;
	struct darwin_pollfd	*pf;
	uint32_t		 got;
	uint32_t		 i;
	uint32_t		 want;
	long			 n;
	int16_t			 rev;
	bool			 bad;

	ps = arg;
	n  = 0;
	for (i = 0; i < ps->ps_nfds; i++) {
		pf = &ps->ps_fds[i];
		pf->pfd_revents = 0;
		if (pf->pfd_fd < 0)
			continue;		/* POSIX: ignored, answers 0 */
		want = 0;
		if ((pf->pfd_events & DARWIN_POLLIN) != 0)
			want |= DARWIN_RD;
		if ((pf->pfd_events & DARWIN_POLLOUT) != 0)
			want |= DARWIN_WR;
		got = darwin_fd_ready(ps->ps_task, pf->pfd_fd, want, &bad);
		rev = 0;
		if (bad)
			rev = DARWIN_POLLNVAL;
		else {
			if ((got & DARWIN_RD) != 0)
				rev |= DARWIN_POLLIN;
			if ((got & DARWIN_WR) != 0)
				rev |= DARWIN_POLLOUT;
			if ((got & DARWIN_HUP) != 0)
				rev |= DARWIN_POLLHUP;
			if ((got & DARWIN_ERR) != 0)
				rev |= DARWIN_POLLERR;
		}
		pf->pfd_revents = rev;
		if (rev != 0)
			n++;
	}
	return (n);
}

/*
 * poll(2): (fds, nfds, timeout-in-ms) in arg0..2; a negative timeout waits
 * for ever, zero scans once.  Returns how many entries have a non-zero
 * revents, and writes the array back unless the wait failed.
 */
static long
darwin_sys_poll(struct syscall_frame *f)
{
	struct darwin_poll_scan	ps;
	uint64_t		 deadline;
	long			 rv;
	int			 timeout;
	bool			 once;

	ps.ps_task = current_thread->th_task;
	ps.ps_nfds = (uint32_t)f->sf_arg1;
	timeout    = (int)f->sf_arg2;
	if (ps.ps_nfds > DARWIN_POLL_MAX)
		return (darwin_err(f, DARWIN_EINVAL));
	if (ps.ps_nfds != 0 && syscall_copyin(ps.ps_fds,
	    (const void *)f->sf_arg0, ps.ps_nfds * sizeof(ps.ps_fds[0])) != 0)
		return (darwin_err(f, DARWIN_EFAULT));

	once     = timeout == 0;
	deadline = 0;
	if (timeout > 0)
		deadline = darwin_deadline_ms((uint64_t)timeout);
	darwin_select_n_call++;
	rv = darwin_readiness_wait(darwin_poll_scan, &ps, deadline, once);
	if (rv < 0)
		return (darwin_err(f, (int)-rv));
	if (ps.ps_nfds != 0 && syscall_copyout((void *)f->sf_arg0, ps.ps_fds,
	    ps.ps_nfds * sizeof(ps.ps_fds[0])) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, rv));
}

/*
 * Will argv + envp fit the dyld handoff page?  The arithmetic of
 * build_dyld_arg_stack (arch/amd64/usermode.c), asked before execve drops
 * the old image, while E2BIG can still be returned; past that point the
 * builder's own check can only exit the task with 127.
 */
static bool
darwin_frame_fits(int argc, char **argv, int envc, char **envp)
{
	size_t	need;
	size_t	i;
	int	k;

	need = 8u * (size_t)(argc + envc + 5) + 16u;
	for (k = 0; k < argc; k++) {
		for (i = 0; argv[k][i] != '\0'; i++)
			continue;
		need += i + 1;
	}
	for (k = 0; k < envc; k++) {
		for (i = 0; envp[k][i] != '\0'; i++)
			continue;
		need += i + 1;
	}
	return (need <= 4096u - 64u);
}

/* ---- the working directory ----------------------------------------------- */

/*
 * Resolve a user path against the task's working directory into an
 * absolute, normalised path in `out`.  The filesystem resolves components
 * literally and has no parent links, so `..' is handled here: "." is
 * dropped, ".." pops a component (a no-op at the root), and repeated
 * slashes collapse.
 *
 * Returns 0, or -1 if the path does not fit; `out` is then garbage, and
 * every caller fails the syscall without reading it.
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

	/* Strip a trailing separator, except the one that is the root. */
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
			darwin_ofile_clear(t, &t->t_darwin_files[i]);
	}
	darwin_lk_drop(t->t_id, 0);		/* anything a close missed */
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
		case DARWIN_OF_NULL:
			dst->of_type = src->of_type;
			break;
		case DARWIN_OF_DIR:
		case DARWIN_OF_FILE:
			/* The child shares the parent's cursor (POSIX). */
			buf = NULL;
			if (src->of_buf != NULL) {
				buf = kmalloc(src->of_size != 0 ?
				    src->of_size : 1);
				if (buf == NULL)
					return (-1);
				for (k = 0; k < src->of_size; k++)
					buf[k] = src->of_buf[k];
			}
			/* Each duplicated descriptor is another fs hold. */
			if (fs_hold(&src->of_handle) != FS_E_OK) {
				if (buf != NULL)
					kfree(buf);
				return (-1);
			}
			dst->of_buf    = buf;
			dst->of_handle = src->of_handle;
			dst->of_path   = darwin_path_dup(src->of_path);
			dst->of_size   = src->of_size;
			dst->of_foff   = src->of_foff;
			if (dst->of_foff != NULL)
				darwin_foff_hold(dst->of_foff);
			dst->of_flags  = src->of_flags;
			/* The source's type: a directory must stay one. */
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
		darwin_ofile_clear(t, dst);

	switch (type) {
	case DARWIN_OF_CONSOLE:
	case DARWIN_OF_NULL:
		dst->of_type = type;
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
		/*
		 * Take the hold before filling the slot, so a refusal leaves it
		 * free rather than holding an uncounted handle.
		 */
		if (fs_hold(&src->of_handle) != FS_E_OK) {
			if (buf != NULL)
				kfree(buf);
			return (-DARWIN_EMFILE);
		}
		dst->of_buf    = buf;
		dst->of_handle = src->of_handle;
		dst->of_path   = darwin_path_dup(src->of_path);
		dst->of_size   = src->of_size;
		dst->of_foff   = src->of_foff;	/* one cursor for both */
		if (dst->of_foff != NULL)
			darwin_foff_hold(dst->of_foff);
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

/* ---- zombies (exit status for wait4) ------------------------------------- */

/*
 * A dying Darwin task's {pid, ppid, status}, kept until the parent reaps
 * it with wait4.  A flat table, since style9 has no struct proc; 32
 * unreaped children is already pathological.
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

/* ---- signals ------------------------------------------------------------- */

/*
 * Bit for signal `signo` in the pending / mask words, in Apple's sigset_t
 * layout: signal n is bit n - 1 (XNU's sigmask(), <signal.h>'s __sigbits).
 * Masks arrive from user space as-is, and SDK-built binaries fill them
 * with the <signal.h> macros, so any other layout blocks the wrong
 * signal.  Valid signals are 1..DARWIN_NSIG-1; signal 0 (the kill(2)
 * existence probe) and anything out of range map to no bit, so posting
 * them is a silent no-op.
 */
static inline uint32_t
darwin_sigbit(int signo)
{

	if (signo <= 0 || signo >= DARWIN_NSIG)
		return (0);
	return ((uint32_t)1 << (signo - 1));
}

/*
 * Default action for a SIG_DFL signal: only SIGCHLD is ignored; every
 * other signal terminates the task.
 */
static bool
darwin_sig_default_is_ignore(int signo)
{

	return (signo == DARWIN_SIGCHLD);
}

/*
 * The mask a signal frame records for sigreturn: the one in force or,
 * after a pselect(2) ended by this signal, the one pselect must restore
 * (t_sig_mask_restore).  Reading it disarms the request; the frame now
 * carries the restore.
 */
static uint32_t
darwin_sig_mask_for_frame(struct task *t)
{
	uint32_t	m;

	if (t->t_sig_mask_restore) {
		t->t_sig_mask_restore = false;
		m = t->t_sig_mask_saved;
		return (m);
	}
	return (t->t_sig_mask);
}

/* Nothing was delivered after all: restore the mask now. */
static void
darwin_sig_mask_unarm(struct task *t)
{

	if (t->t_sig_mask_restore) {
		t->t_sig_mask_restore = false;
		t->t_sig_mask = t->t_sig_mask_saved;
	}
}

void
darwin_signal_post(struct task *t, int signo)
{
	uint32_t	bit;

	bit = darwin_sigbit(signo);
	if (t == NULL || bit == 0)
		return;
	__atomic_fetch_or(&t->t_sig_pending, bit, __ATOMIC_RELEASE);
	/*
	 * Wake the target's sleepers, or the bit is not seen until their wait
	 * ends on its own (never, for an idle pipe).  Unconditionally: whether
	 * the signal is deliverable depends on a mask only the target's thread
	 * may read, and a sleeper woken for nothing parks again.
	 */
	(void)sched_wake_sleepers_of(t);
}

bool
darwin_signal_pending(struct task *t)
{
	uint32_t	deliverable;
	int		signo;

	if (t == NULL)
		return (false);
	/*
	 * One atomic load of t_sig_pending, which other tasks write.
	 * t_sig_mask is written only by the owning task's thread, which is
	 * this one, so a plain read is right.
	 */
	deliverable = __atomic_load_n(&t->t_sig_pending, __ATOMIC_ACQUIRE) &
	    ~t->t_sig_mask;
	if (deliverable == 0)
		return (false);

	/*
	 * An ignored signal must not end a wait: it is consumed on the way to
	 * ring 3 and the EINTR would be for nothing.  SIGCHLD is the case that
	 * matters -- default-ignore, and it arrives exactly while the parent
	 * sits in wait4, which would otherwise return EINTR instead of reaping.
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
 * Post SIGCHLD to the task whose id is `ppid`; a no-op if it has exited.
 * Called outside darwin_zombie_lock: task_lookup_ref takes tasks_lock, and
 * no path nests a task lock under darwin_zombie_lock.
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
 * Pick the next signal to act on from `t`'s deliverable set (pending &
 * ~mask), lowest first, with its disposition in *disp_out.  Ignored
 * signals are consumed and skipped; a default-terminate one is consumed
 * and returned; a caught one (handler VA) is returned still pending for
 * on-stack delivery, which ends the scan, so a terminate queued behind it
 * waits.  Returns 0 when nothing is deliverable.
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
		signo = __builtin_ctz(deliverable) + 1;
		bit   = darwin_sigbit(signo);
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
	 * Default terminate.  The task never reaches exit(2), so record its
	 * wait4 status here: termsig in the low 7 bits (WIFSIGNALED).
	 */
	darwin_zombie_record(t->t_id, t->t_darwin_ppid, signo & 0x7F);
	thread_exit();
	/* NOTREACHED */
}

/*
 * RFLAGS to resume ring 3 with: the arithmetic flags and DF from the
 * sigframe, IF and reserved bit 1 forced on, everything else dropped.  The
 * frame is on the user stack, so unmasked a forged one could grant IOPL 3.
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
	frame.sf_mask   = (uint64_t)darwin_sig_mask_for_frame(t);
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
	if (signo == 0) {
		darwin_sig_mask_unarm(t);
		return;
	}
	if (disp != DARWIN_SIG_DFL) {
		/*
		 * Caught: consume the bit and deliver on the user stack; if the
		 * frame cannot be built, terminate instead.
		 */
		__atomic_fetch_and(&t->t_sig_pending,
		    ~darwin_sigbit(signo), __ATOMIC_RELEASE);
		if (darwin_signal_setup_frame(f, signo, disp, rv) == 0)
			return;
	}
	darwin_zombie_record(t->t_id, t->t_darwin_ppid, signo & 0x7F);
	thread_exit();
	/* NOTREACHED */
}

/*
 * The asynchronous twin of darwin_signal_setup_frame: same stack layout and
 * trampoline protocol, but it interrupted an arbitrary instruction, so
 * every GPR and the x87/SSE file are live and go in the frame.  (At a
 * syscall boundary the ABI already made scratch registers and the FPU
 * dead.)  The handler still travels in %r10, as on the syscall path where
 * SYSCALL owns %rcx, so one _sigtramp serves both.
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
	frame.sf_mask  = (uint64_t)darwin_sig_mask_for_frame(t);
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
	 * The interrupted thread's x87/SSE state is still in the registers
	 * (only thread_switch_asm spills th_fpu), so FXSAVE here captures what
	 * the handler is about to clobber.
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
		    ~darwin_sigbit(signo), __ATOMIC_RELEASE);
		if (darwin_signal_setup_frame_trap(tf, signo, disp) == 0)
			return;
	}
	darwin_zombie_record(t->t_id, t->t_darwin_ppid, signo & 0x7F);
	thread_exit();
	/* NOTREACHED */
}

/*
 * Resume from an SGFR2 frame: rebuild the interrupted state as a trapframe
 * and leave through IRETQ, since SYSRET destroys %rcx and %r11 (the SGFR1
 * path can use the ordinary syscall return).  Never returns: back to ring
 * 3 where the signal struck, or, on a corrupt frame, the task retires.
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
	 * FPU state last: the kernel (-mno-sse) touches no x87/XMM before the
	 * IRETQ, and a context switch in between preserves it.
	 */
	__asm__ __volatile__ ("fxrstor (%0)" : : "r"(frame.sf_fpu) : "memory");

	trapframe_iretq(&tf);
	/* NOTREACHED */
}

/* ---- zombies ------------------------------------------------------------- */

/*
 * The channels a parent in wait4(2) parks on, hashed by its own pid, so a
 * dying child can name one without a lookup and reference on the parent
 * (dropping that reference could itself start another teardown).
 * Collisions only cause spurious wakes, which every sleeper re-tests.
 */
#define	DARWIN_WAIT_CHANS	16
static char	darwin_wait_chan[DARWIN_WAIT_CHANS];

/*
 * One counter per channel, bumped whenever a parent on it is told
 * something.  A parent reads it before parking and after a deadline wake:
 * changed means it was told, however wake and timer raced; unchanged with
 * a zombie waiting means the news never came (a lost wake).
 */
static uint32_t	darwin_wait_gen[DARWIN_WAIT_CHANS];

static uint32_t
darwin_wait_generation(unsigned long long pid)
{

	return (__atomic_load_n(&darwin_wait_gen[pid % DARWIN_WAIT_CHANS],
	    __ATOMIC_ACQUIRE));
}

static void *
darwin_wait_channel(unsigned long long pid)
{

	return (&darwin_wait_chan[pid % DARWIN_WAIT_CHANS]);
}

/*
 * Tell a parent that something happened to one of its children.  Called
 * where a zombie is recorded, and from task teardown, when a child leaves
 * the live list and a wait for it can only answer ECHILD.  The teardown
 * call covers every exit route, recorded or not.
 */
void
darwin_child_news(unsigned long long ppid)
{

	if (ppid == 0)
		return;			/* no Darwin parent to tell */
	darwin_wait_n_told++;
	/* Bumped before the wake, so woken and awake parents both see it. */
	(void)__atomic_fetch_add(&darwin_wait_gen[ppid % DARWIN_WAIT_CHANS], 1,
	    __ATOMIC_RELEASE);
	darwin_wait_n_woke += sched_wakeup(darwin_wait_channel(ppid));
}

void
darwin_zombie_record(unsigned long long pid, unsigned long long ppid,
    int status)
{
	size_t	i;
	int	slot;

	/*
	 * A dead process holds no record locks, and its parent must not find
	 * any once wait4 says it is dead.  Its files close later, in
	 * thread_exit, too late for that; drop the locks now.
	 */
	darwin_lk_drop(pid, 0);

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
		darwin_child_news(ppid);
		return;
	}
	darwin_zombies[slot].z_pid    = pid;
	darwin_zombies[slot].z_ppid   = ppid;
	darwin_zombies[slot].z_status = status;
	darwin_zombies[slot].z_used   = true;
	spin_unlock(&darwin_zombie_lock);
	darwin_signal_notify_parent(ppid);
	/*
	 * Woken with the zombie lock dropped: a waiter parks on the channel
	 * while holding that lock, so it is already findable by now.
	 */
	darwin_child_news(ppid);
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
 * Is there a zombie for `ppid` (optionally a particular `pid`)?  Does not
 * reap; caller holds darwin_zombie_lock.  Lets wait4 make its last look and
 * its park under one lock, so a child dying in between cannot be missed.
 */
static bool
darwin_zombie_present_locked(uint64_t ppid, uint64_t pid)
{
	size_t	i;

	for (i = 0; i < DARWIN_NZOMBIE; i++) {
		if (!darwin_zombies[i].z_used)
			continue;
		if (darwin_zombies[i].z_ppid != ppid)
			continue;
		if (pid != 0 && darwin_zombies[i].z_pid != pid)
			continue;
		return (true);
	}
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
			    (const void *)f->sf_arg1, (size_t)f->sf_arg2,
			    NULL));
		case DARWIN_OF_NULL:
			/* Swallowed whole; the bytes are not even read. */
			return (darwin_ok(f, (long)f->sf_arg2));
		default:
			return (darwin_err(f, DARWIN_EBADF));
		}
	}
	case DARWIN_SYS_gettimeofday: {
		struct darwin_timeval	tv;
		int64_t			us;

		/*
		 * With no usable RTC, EPERM rather than 1970: a program told
		 * the time is unavailable can say so.
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
		 * Must be a directory, not merely exist, or later relative
		 * paths fail one component deeper.  The root always is.
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
		/* ERANGE, never a truncated path. */
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
		struct darwin_foff		*fo;
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
		 * /dev: three names, no directory.  /dev/console and /dev/tty
		 * (what ttyname(3) returns) open the console; /dev/null is the
		 * sink.  Answered before the volume, which might otherwise
		 * create a plain file called null.
		 */
		if (darwin_streq(path, "/dev/null") ||
		    darwin_streq(path, "/dev/console") ||
		    darwin_streq(path, "/dev/tty")) {
			fd = darwin_fd_alloc(current_thread->th_task);
			if (fd < 0)
				return (darwin_err(f, DARWIN_EMFILE));
			t = current_thread->th_task;
			t->t_darwin_files[fd].of_flags = (uint32_t)f->sf_arg1;
			t->t_darwin_files[fd].of_type  = path[5] == 'n' ?
			    DARWIN_OF_NULL : DARWIN_OF_CONSOLE;
			return (darwin_ok(f, fd));
		}

		/*
		 * O_CREAT, O_EXCL and O_TRUNC take effect here, not at the
		 * first write: when open(2) returns, the file exists and has
		 * the length the flags say.
		 */
		flags   = (uint32_t)f->sf_arg1;
		writing = (flags & DARWIN_O_ACCMODE) != DARWIN_O_RDONLY;

		/*
		 * A read-only tree (fs.h, /.xid) refuses writers at the open,
		 * as open(2) specifies; creating and truncating are writes.
		 */
		if (fs_readonly(path) && (writing ||
		    (flags & (DARWIN_O_CREAT | DARWIN_O_TRUNC)) != 0))
			return (darwin_err(f, DARWIN_EROFS));

		/*
		 * A directory can be opened (GNU mkdir opens what it makes);
		 * fs_open would say "not found" for one.  The descriptor
		 * carries only the path, which is what every call taking a
		 * directory fd wants; read(2) on it is EISDIR, and opening it
		 * for writing is refused.
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
				t->t_darwin_files[fd].of_foff  = NULL;
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

			/* The requested mode, less the umask. */
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
		 * A disk file is only resolved to a handle; bytes are read on
		 * demand.  Built-ins are copied into of_buf.
		 */
		if (rv == FS_E_NOTFOUND || rv == FS_E_NOMOUNT) {
			/* Not on the disk: try the synthetic /bin (progreg). */
			pe = darwin_bin_lookup(path);
			if (pe == NULL) {
				kprintf("darwin: open('%s') -- nothing of that "
				    "name on the volume or in /bin\n", path);
				return (darwin_err(f, DARWIN_ENOENT));
			}
			/* A built-in lives in kernel text: EROFS to writers. */
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
			 * Truncate now, so `> f' empties f even if nothing is
			 * ever written.
			 */
			if ((flags & DARWIN_O_TRUNC) != 0 &&
			    handle.fh_size != 0) {
				rv = fs_truncate(&handle, 0);
				if (rv != FS_E_OK) {
					kprintf("darwin: open('%s'): O_TRUNC "
					    "refused (rv=%d)\n", path, rv);
					/*
					 * Every exit past fs_open must close
					 * the handle, or its open-file row
					 * leaks for good.
					 */
					(void)fs_close(&handle);
					return (darwin_err(f,
					    darwin_fs_errno(rv)));
				}
			}
			if (handle.fh_size > 0xFFFFFFFFULL) {
				(void)fs_close(&handle);
				return (darwin_err(f, DARWIN_ENOMEM));
			}
			size = (uint32_t)handle.fh_size;
		}

		t  = current_thread->th_task;
		fo = darwin_foff_new();
		fd = fo != NULL ? darwin_fd_alloc(t) : -1;
		if (fd < 0) {
			if (fo != NULL)
				darwin_foff_drop(fo);
			if (buf != NULL)
				kfree(buf);
			(void)fs_close(&handle);
			return (darwin_err(f, fo != NULL ? DARWIN_EMFILE :
			    DARWIN_ENOMEM));
		}
		t->t_darwin_files[fd].of_buf    = buf;
		t->t_darwin_files[fd].of_handle = handle;
		/* For mmap, fstat and fdpath; reads use the handle. */
		t->t_darwin_files[fd].of_path =
		    on_disk ? darwin_path_dup(path) : NULL;
		t->t_darwin_files[fd].of_size  = size;
		t->t_darwin_files[fd].of_foff  = fo;
		t->t_darwin_files[fd].of_flags = flags;
		t->t_darwin_files[fd].of_type  = DARWIN_OF_FILE;
		kprintf("darwin: UNIX open('%s') -> fd=%d (%u bytes, %s%s)\n",
		    path, fd, (unsigned)size,
		    on_disk ? "on the volume" : "built in",
		    writing ? ", for writing" : "");
		return (darwin_ok(f, fd));
	}
	case DARWIN_SYS_read: {
		struct darwin_foff	*fo;
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
			 * readdir(3) uses the fs_readdir backchannel, not
			 * read(2); EISDIR, as a modern Unix answers.
			 */
			return (darwin_err(f, DARWIN_EISDIR));
		case DARWIN_OF_FILE:
			/*
			 * On the volume, fs_pread finds the end as it is now
			 * -- another descriptor may have moved it -- and a
			 * cursor past it (ftruncate(2) leaves one) reads 0.
			 */
			if (of->of_buf == NULL)
				return (darwin_file_read(f, of,
				    (void *)f->sf_arg1, n, NULL));

			/* Built-in image: the bytes are already here. */
			fo = of->of_foff;
			mutex_lock(&fo->fo_lock);
			avail = fo->fo_off < of->of_size ?
			    of->of_size - (uint32_t)fo->fo_off : 0;
			if (n > (size_t)avail)
				n = avail;
			rv = 0;
			if (n != 0) {
				rv = syscall_copyout((void *)f->sf_arg1,
				    of->of_buf + fo->fo_off, n);
				if (rv == 0)
					fo->fo_off += n;
			}
			mutex_unlock(&fo->fo_lock);
			if (rv != 0)
				return (darwin_err(f, DARWIN_EFAULT));
			return (darwin_ok(f, (long)n));
		case DARWIN_OF_PIPE_R:
			return (darwin_pipe_read(f, of->of_pipe,
			    (void *)f->sf_arg1, n));
		case DARWIN_OF_NULL:
			return (darwin_ok(f, 0));	/* always at its end */
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
		 * A file lives until its last name and last descriptor are
		 * gone.  fs_unlink decides: if the file is still open, the
		 * name goes and the bytes wait in the volume's private
		 * directory until the last close.  Ring 3 sees the same
		 * result either way.
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
	 * rename(2): resolve both names and hand them to one fs_rename, whose
	 * edit changes every leaf in memory before writing any, so the rename
	 * is atomic.
	 *
	 * An open descriptor still answers with the old path for the calls
	 * that ask about names (fchmod, the fdpath backchannel), since it
	 * keeps the name it was opened by.  Its bytes go through the handle,
	 * so a descriptor on a replaced file keeps reading that file, now
	 * nameless until its last close, while new opens get the newcomer.
	 * user/filewrite.c checks both.
	 */
	case DARWIN_SYS_rename: {
		char	opath[DARWIN_PATH_MAX];
		char	npath[DARWIN_PATH_MAX];
		char	raw[DARWIN_PATH_MAX];
		int	rv;

		if (syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw)) < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(current_thread->th_task, raw, opath,
		    sizeof(opath)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));
		if (syscall_copyin_str((const char *)f->sf_arg1, raw,
		    sizeof(raw)) < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(current_thread->th_task, raw, npath,
		    sizeof(npath)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));

		rv = fs_rename(opath, npath);
		if (rv != FS_E_OK) {
			if (rv != FS_E_NOTFOUND)
				kprintf("darwin: rename('%s', '%s') refused "
				    "(rv=%d)\n", opath, npath, rv);
			return (darwin_err(f, darwin_fs_errno(rv)));
		}
		kprintf("darwin: UNIX rename('%s', '%s') -- one name, moved\n",
		    opath, npath);
		return (darwin_ok(f, 0));
	}
	/*
	 * mkdir(2) and rmdir(2), shaped like unlink above.  mkdir's mode is
	 * the request less the umask: `mkdir foo' asks 0777 and gets 0755.
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
			/* ENOENT, and EEXIST for mkdir, are routine: no log. */
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
		darwin_ofile_clear(t, of);
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_fsync: {
		struct darwin_ofile	*of;
		struct task		*t;
		int			 fd;

		fd = (int)f->sf_arg0;
		t  = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		switch (of->of_type) {
		case DARWIN_OF_FREE:
			if (fd <= 2)	/* legacy std streams: no backing */
				return (darwin_ok(f, 0));
			return (darwin_err(f, DARWIN_EBADF));
		case DARWIN_OF_PIPE_R:
		case DARWIN_OF_PIPE_W:
			return (darwin_err(f, DARWIN_EINVAL));
		case DARWIN_OF_CONSOLE:
			return (darwin_ok(f, 0));	/* nothing is cached */
		default:
			break;		/* a file or a directory: the volume */
		}
		/*
		 * One volume, one open transaction: publishing this file means
		 * publishing everything, so fsync is sync.  write(2) only
		 * batches (ckpt_policy in fs/fs.c); a 0 here means the data is
		 * on disk.
		 */
		if (fs_sync() != FS_E_OK)
			return (darwin_err(f, DARWIN_EIO));
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_lseek: {
		struct darwin_foff	*fo;
		struct darwin_ofile	*of;
		struct task		*t;
		int64_t			 base;
		int64_t			 len;
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
		if (of->of_type == DARWIN_OF_NULL)
			return (darwin_ok(f, 0));	/* every offset is 0 */
		if (of->of_type != DARWIN_OF_FILE)
			return (darwin_err(f, DARWIN_EBADF));

		if (whence < 0 || whence > 2)
			return (darwin_err(f, DARWIN_EINVAL));

		/* The end as it is now, not as this descriptor last saw it. */
		fo = of->of_foff;
		mutex_lock(&fo->fo_lock);
		len = (int64_t)darwin_file_len(of);
		switch (whence) {
		case 0:	base = 0;				break; /* SET */
		case 1:	base = (int64_t)fo->fo_off;		break; /* CUR */
		default: base = len;				break; /* END */
		}
		pos = base + off;
		if (pos >= 0 && pos <= len)
			fo->fo_off = (uint64_t)pos;
		mutex_unlock(&fo->fo_lock);
		if (pos < 0 || pos > len)
			return (darwin_err(f, DARWIN_EINVAL));
		return (darwin_ok(f, (long)pos));
	}
	case DARWIN_SYS_access: {
		struct fs_statbuf	sb;
		char			path[DARWIN_PATH_MAX];
		char			raw[DARWIN_PATH_MAX];
		long			len;
		int			mode;
		int			rv;

		/*
		 * One user, root, so the only real permission question is
		 * W_OK: the synthetic /bin and read-only trees refuse it.
		 * The rest is existence (a PATH search's X_OK means that).
		 */
		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(current_thread->th_task, raw, path,
		    sizeof(path)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));
		mode = (int)f->sf_arg1;
		if (darwin_bin_lookup(path) != NULL) {
			if ((mode & DARWIN_W_OK) != 0)
				return (darwin_err(f, DARWIN_EROFS));
			return (darwin_ok(f, 0));
		}
		if (darwin_streq(path, "/dev/null") ||
		    darwin_streq(path, "/dev/console") ||
		    darwin_streq(path, "/dev/tty"))
			return (darwin_ok(f, 0));
		rv = fs_stat(path, &sb);
		if (rv != FS_E_OK)
			return (darwin_err(f, darwin_fs_errno(rv)));
		if ((mode & DARWIN_W_OK) != 0 && fs_readonly(path))
			return (darwin_err(f, DARWIN_EROFS));
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_select:
		return (darwin_select_common(f, false));
	case DARWIN_SYS_poll:
		return (darwin_sys_poll(f));
	case DARWIN_SYS_ftruncate: {
		struct darwin_ofile	*of;
		struct task		*t;
		int64_t			 len;
		int			 fd;
		int			 rv;

		fd  = (int)f->sf_arg0;
		len = (int64_t)f->sf_arg1;
		t   = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		if (of->of_type == DARWIN_OF_FREE)
			return (darwin_err(f, DARWIN_EBADF));
		if (len < 0)
			return (darwin_err(f, DARWIN_EINVAL));
		if (of->of_type == DARWIN_OF_DIR)
			return (darwin_err(f, DARWIN_EISDIR));
		if (of->of_type != DARWIN_OF_FILE)
			return (darwin_err(f, DARWIN_EINVAL));
		if (of->of_buf != NULL)
			return (darwin_err(f, DARWIN_EROFS)); /* a /bin image */
		if ((of->of_flags & DARWIN_O_ACCMODE) == DARWIN_O_RDONLY)
			return (darwin_err(f, DARWIN_EINVAL));
		rv = fs_truncate(&of->of_handle, (uint64_t)len);
		if (rv != FS_E_OK)
			return (darwin_err(f, darwin_fs_errno(rv)));
		/* The cursor stays put; read(2) handles one past the end. */
		of->of_size = (uint32_t)of->of_handle.fh_size;
		return (darwin_ok(f, 0));
	}
	case DARWIN_SYS_pread:
	case DARWIN_SYS_pwrite: {
		struct darwin_ofile	*of;
		struct task		*t;
		uint64_t		 at;
		uint32_t		 avail;
		int64_t			 off;
		size_t			 n;
		int			 fd;

		fd  = (int)f->sf_arg0;
		n   = (size_t)f->sf_arg2;
		off = (int64_t)f->sf_arg3;
		t   = current_thread->th_task;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &t->t_darwin_files[fd];
		switch (of->of_type) {
		case DARWIN_OF_FILE:
			break;
		case DARWIN_OF_FREE:
			return (darwin_err(f, DARWIN_EBADF));
		case DARWIN_OF_DIR:
			return (darwin_err(f, DARWIN_EISDIR));
		default:
			/* A pipe or a terminal has no offsets. */
			return (darwin_err(f, DARWIN_ESPIPE));
		}
		if (off < 0)
			return (darwin_err(f, DARWIN_EINVAL));
		at = (uint64_t)off;

		if (nr == DARWIN_SYS_pwrite) {
			/* Sizes and cursors are 32-bit here. */
			if (at + n > UINT32_MAX)
				return (darwin_err(f, DARWIN_EFBIG));
			return (darwin_file_write(f, of,
			    (const void *)f->sf_arg1, n, &at));
		}

		/* pread: read(2) with its own offset, the cursor untouched. */
		if (of->of_buf == NULL)
			return (darwin_file_read(f, of, (void *)f->sf_arg1, n,
			    &at));
		avail = at < of->of_size ? of->of_size - (uint32_t)at : 0;
		if (n > (size_t)avail)
			n = avail;
		if (n == 0)
			return (darwin_ok(f, 0));
		if (syscall_copyout((void *)f->sf_arg1, of->of_buf + at,
		    n) != 0)
			return (darwin_err(f, DARWIN_EFAULT));
		return (darwin_ok(f, (long)n));
	}
	case DARWIN_SYS_statfs64: {
		struct fs_statbuf	sb;
		char			path[DARWIN_PATH_MAX];
		char			raw[DARWIN_PATH_MAX];
		long			len;
		int			rv;

		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		if (darwin_path_resolve(current_thread->th_task, raw, path,
		    sizeof(path)) != 0)
			return (darwin_err(f, DARWIN_ENAMETOOLONG));
		rv = fs_stat(path, &sb);
		if (rv != FS_E_OK)
			return (darwin_err(f, darwin_fs_errno(rv)));
		return (darwin_statfs_out(f, path, (void *)f->sf_arg1));
	}
	case DARWIN_SYS_fstatfs64: {
		struct darwin_ofile	*of;
		int			 fd;

		fd = (int)f->sf_arg0;
		if (fd < 0 || fd >= DARWIN_NOFILE)
			return (darwin_err(f, DARWIN_EBADF));
		of = &current_thread->th_task->t_darwin_files[fd];
		if (of->of_type != DARWIN_OF_FILE &&
		    of->of_type != DARWIN_OF_DIR)
			return (darwin_err(f, of->of_type == DARWIN_OF_FREE ?
			    DARWIN_EBADF : DARWIN_EINVAL));
		return (darwin_statfs_out(f, of->of_path,
		    (void *)f->sf_arg1));
	}
	case DARWIN_SYS_mkfifo: {
		char	raw[DARWIN_PATH_MAX];
		long	len;

		/*
		 * No FIFOs: the volume has no node type for one.  EPERM, as a
		 * filesystem that cannot hold one answers; make's jobserver
		 * then falls back to pipe(2).
		 */
		len = syscall_copyin_str((const char *)f->sf_arg0, raw,
		    sizeof(raw));
		if (len < 0)
			return (darwin_err(f, DARWIN_EFAULT));
		kprintf("darwin: mkfifo('%s'): this kernel has no FIFOs\n",
		    raw);
		return (darwin_err(f, DARWIN_EPERM));
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
		uint32_t	 gen;
		bool		 by_net;

		t       = current_thread->th_task;
		pid     = (long)f->sf_arg0;
		options = (int)f->sf_arg2;
		by_net  = false;
		gen     = 0;
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
				darwin_wait_n_call++;
				/*
				 * Found only because the deadline woke us: some
				 * exit path lacks a darwin_child_news.
				 */
				if (by_net) {
					darwin_wait_n_lost++;
					kprintf("darwin: wait4 for pid %llu "
					    "was told by its deadline, not by "
					    "the child -- a wake is missing\n",
					    (unsigned long long)got);
				}
				return (darwin_ok(f, (long)got));
			}
			/*
			 * No zombie, and no live child left to make one:
			 * ECHILD.  A zombie is recorded before its task
			 * leaves the live list, so a child exiting between
			 * the two samples is caught next iteration.
			 */
			if (task_count_darwin_children(t->t_id,
			    pid > 0 ? (uint64_t)pid : 0) == 0) {
				/* Same reasoning as the reap above. */
				if (by_net) {
					darwin_wait_n_lost++;
					kprintf("darwin: wait4 found its last "
					    "child already gone when the "
					    "deadline woke it -- a wake is "
					    "missing\n");
				}
				return (darwin_err(f, DARWIN_ECHILD));
			}
			if (options & DARWIN_WNOHANG)
				return (darwin_ok(f, 0));
			/*
			 * Interruptible: this is where a shell sits when
			 * Ctrl-C arrives.  A caught SIGCHLD also breaks the
			 * wait, so its handler runs first.
			 */
			if (task_kill_pending(t) || darwin_signal_pending(t))
				return (darwin_err(f, DARWIN_EINTR));

			/*
			 * Sleep until a child has news.  Look once more under
			 * the lock we park with, so a child dying since the
			 * reap above cannot report to an empty channel.
			 */
			spin_lock(&darwin_zombie_lock);
			if (darwin_zombie_present_locked(t->t_id,
			    pid > 0 ? (uint64_t)pid : 0)) {
				spin_unlock(&darwin_zombie_lock);
				continue;
			}
			/*
			 * Park with a deadline (DARWIN_WAIT_NET_MS).  Waking
			 * on it is ordinary; the top of the loop decides
			 * whether a wake was lost.
			 */
			current_thread->th_wake_deadline_ms =
			    clock_uptime_ms() + DARWIN_WAIT_NET_MS;
			sched_add_timed_waiter(current_thread);
			darwin_wait_n_wait++;
			gen = darwin_wait_generation(t->t_id);
			thread_block_release(THREAD_BLOCK_SLEEP,
			    darwin_wait_channel(t->t_id),
			    &darwin_zombie_lock);
			sched_remove_timed_waiter(current_thread);
			if (current_thread->th_timed_out != 0) {
				darwin_wait_n_net++;
				/* Lost only if nothing was said: darwin_wait_gen. */
				by_net = darwin_wait_generation(t->t_id) == gen;
			} else
				by_net = false;
			current_thread->th_wake_deadline_ms = 0;
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
				darwin_ofile_clear(t, &t->t_darwin_files[rfd]);
			else
				darwin_pipe_drop(p, false);
			darwin_pipe_drop(p, true);
			return (darwin_err(f, DARWIN_EMFILE));
		}
		t->t_darwin_files[wfd].of_pipe = p;
		t->t_darwin_files[wfd].of_type = DARWIN_OF_PIPE_W;
		kprintf("darwin: UNIX pipe() -> r=%d w=%d\n", rfd, wfd);
		/*
		 * Both fds in one %rax: read end low, write end high.  XNU
		 * returns %rax/%rdx, but the entry stub returns one register
		 * and our libSystem, the only caller, unpacks this form.
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
	 * umask(2): set the bits a create may not grant; return the old value
	 * (the only way to read it).
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
	 * chmod(2) and fchmod(2).  No ownership check: one user, root, and
	 * every inode is uid 0.
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
			 * No vnodes: an fd is chmod'ed by the path it was
			 * opened by.  A pipe, the console or a built-in has
			 * none: EINVAL.
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
			 * No close-on-exec flag bits in the table, so this is
			 * plain F_DUPFD.
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
		case DARWIN_F_GETLK:
		case DARWIN_F_SETLK:
		case DARWIN_F_SETLKW:
			return (darwin_fcntl_lock(f, t, oldfd, cmd,
			    (void *)f->sf_arg2));
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
		char				**kenvp;
		const char			 *base;
		size_t				  i;
		long				  n;
		long				  rv;
		uint32_t			  magic;
		int				  argc;
		int				  envc;

		n = syscall_copyin_str((const char *)f->sf_arg0, path,
		    sizeof(path));
		if (n < 0)
			return (darwin_err(f, DARWIN_EFAULT));

		/*
		 * The program registry is flat: resolve by the final path
		 * component, so "/bin/gfactor" and "gfactor" are the same.
		 */
		base = path;
		for (i = 0; path[i] != '\0'; i++) {
			if (path[i] == '/')
				base = &path[i + 1];
		}
		e = darwin_bin_find(base);
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

		/*
		 * The environment crosses the exec (make's MAKEFLAGS, a
		 * shell's exports).  Copied under its own caps; vectors that
		 * will not fit the handoff page get E2BIG here, before the old
		 * image is dropped.
		 */
		kenvp = NULL;
		envc  = 0;
		rv = syscall_copyin_vec((char *const *)f->sf_arg2, &kenvp,
		    &envc, SPAWN_ENV_MAX, SPAWN_ENV_BYTES_MAX);
		if (rv < 0) {
			if (kargv != NULL)
				kfree(kargv);
			return (darwin_err(f, rv == SYS_E_NOMEM ?
			    DARWIN_ENOMEM : rv == SYS_E_INVAL ?
			    DARWIN_E2BIG : DARWIN_EFAULT));
		}
		if (!darwin_frame_fits(argc, kargv, envc, kenvp)) {
			if (kargv != NULL)
				kfree(kargv);
			if (kenvp != NULL)
				kfree(kenvp);
			return (darwin_err(f, DARWIN_E2BIG));
		}

		kprintf("darwin: UNIX execve('%s') argc=%d envc=%d\n", path,
		    argc, envc);
		rv = arch_darwin_execve(e->pr_image, e->pr_size, argc,
		    kargv, envc, kenvp, f);
		if (kargv != NULL)
			kfree(kargv);
		if (kenvp != NULL)
			kfree(kenvp);
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
		 * The target's disposition decides the mechanism.  One
		 * atomic load, no lock: sigaction(2) writes only its own
		 * task, and a kill that races it may act on either
		 * disposition, as if it came just before or just after.
		 */
		disp = DARWIN_SIG_DFL;
		if (sig > 0 && sig < DARWIN_NSIG)
			disp = __atomic_load_n(&target->t_sig_handler[sig],
			    __ATOMIC_RELAXED);
		if (sig != 0 && (disp != DARWIN_SIG_DFL ||
		    darwin_sig_default_is_ignore(sig) ||
		    target == current_thread->th_task)) {
			/*
			 * Post and let return-to-user delivery decide: a
			 * caught signal (the handler runs at the next return
			 * to ring 3, a timer IRQ being enough), an ignored or
			 * default-ignore one, and any self-signal, applied at
			 * this syscall's exit.
			 */
			darwin_signal_post(target, sig);
		} else if (sig != 0) {
			/*
			 * Cross-task default-terminate: record the wait4
			 * status (termsig in the low bits; the target never
			 * reaches exit(2)) and request the kill directly.
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
		uint64_t	old;
		int		signo;

		/*
		 * libSystem's sigaction/signal pass (signo, handler) in
		 * arg0/arg1: DARWIN_SIG_DFL (0), DARWIN_SIG_IGN (1) or a
		 * ring-3 handler VA, invoked on-stack at delivery.  SIGKILL is
		 * uncatchable.
		 */
		signo = (int)f->sf_arg0;
		if (signo <= 0 || signo >= DARWIN_NSIG ||
		    signo == DARWIN_SIGKILL)
			return (darwin_err(f, DARWIN_EINVAL));
		/*
		 * The old disposition comes back in %rax: make and background
		 * commands install a handler only where the signal was not
		 * already ignored.
		 */
		/* Atomic store: kill(2) from another task reads the slot. */
		old = current_thread->th_task->t_sig_handler[signo];
		__atomic_store_n(&current_thread->th_task->t_sig_handler[signo],
		    f->sf_arg1, __ATOMIC_RELAXED);
		/* arg2 carries libSystem's _sigtramp VA (same for every sig). */
		if (f->sf_arg2 != 0)
			current_thread->th_task->t_sig_tramp = f->sf_arg2;
		return (darwin_ok(f, (long)old));
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
		 * ucontext in arg0; the magic at offset 0 gives the flavour.
		 * SGFR2 (asynchronous) resumes by IRETQ in
		 * darwin_sigreturn_full.  SGFR1 reshapes this syscall frame so
		 * the sysret lands at the saved rip/rsp/rflags with the saved
		 * %rax.  A bad pointer or magic kills the task.  Reading the
		 * 64-byte SGFR1 size is safe for both: SGFR2 is larger and
		 * keeps the magic at offset 0.
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
		 * The address is only a hint; the map picks the range.
		 * MAP_FIXED is refused rather than silently placed elsewhere.
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
			/* A /bin built-in has no handle to page through. */
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
		 * Lazy: no frames or page tables until first touch, unlike
		 * vm_allocate, so mapping a 4 MiB file costs one kmalloc.
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
		 * Any sub-range, middle included; vm_map splits entries at the
		 * edges.  A range with a hole is refused, though POSIX allows
		 * it.
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
		 * A fresh SEND right to the host port in the caller's space;
		 * MACH_PORT_NULL on failure (no host_init yet, table full).
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
	case DARWIN_MACH_thread_self_trap:
	case DARWIN_MACH_thread_get_special_reply_port:
	case DARWIN_MACH_mk_timer_create_trap:
		/* Port-returning traps signal failure with a null name. */
		kprintf("darwin: MACH trap %u has no port to give\n",
		    (unsigned)trap);
		return (darwin_ok(f, (long)MACH_PORT_NULL));
	default:
		/* The rest return a kern_return_t, where 0 is success. */
		kprintf("darwin: unhandled mach trap %u\n", (unsigned)trap);
		return (darwin_ok(f, DARWIN_KERN_INVALID_ARGUMENT));
	}
}

/*
 * Map a style9 mach_msg result -- MACH_MSG_OK / a positive MACH_E_*, or the
 * negative SYS_E_FAULT a user-range check returns -- onto the Darwin
 * mach_msg_return_t the caller reads.  `sending` selects the SEND_* vs RCV_*
 * code family.  A send never waits for queue room, so a full queue is a
 * timeout to a caller that set one and a lack of buffers to one that did not.
 */
static long
darwin_mach_msg_err(long rv, bool sending, uint32_t option)
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
	case MACH_E_NOMSG:		/* a zero timeout on an empty queue */
		return (sending ? DARWIN_MACH_SEND_TIMED_OUT :
		    DARWIN_MACH_RCV_TIMED_OUT);
	case MACH_E_NOSPACE:
		if (!sending)
			return (DARWIN_MACH_RCV_INVALID_DATA);
		return ((option & DARWIN_MACH_SEND_TIMEOUT) != 0 ?
		    DARWIN_MACH_SEND_TIMED_OUT : DARWIN_MACH_SEND_NO_BUFFER);
	default:
		return (sending ? DARWIN_MACH_SEND_INVALID_DATA :
		    DARWIN_MACH_RCV_INVALID_DATA);
	}
}

/*
 * mach_msg_trap (Mach class 1, trap 31): the classic combined send/receive.
 * Arguments (msg, option, send_size, rcv_size, rcv_name, timeout) in the
 * usual registers; send_size is ignored in favour of msgh_size, and the
 * 7th (notify) is unsupported.  SEND|RCV sends, then receives into the same
 * buffer, through the syscall_msg_* helpers (range check, SMAP bracket).
 * Returns a mach_msg_return_t in %rax, carry clear.
 *
 * Only simple messages pass.  The header is Darwin's byte for byte, the
 * descriptors are not (Darwin's port descriptor is 12 bytes with the type
 * in its last byte, ours 8 with it in the first), so a complex send is
 * refused rather than misread.  No trailer follows a received message.
 */
static long
darwin_mach_msg(struct syscall_frame *f)
{
	struct mach_msg_header	*msg;
	uint64_t		 timeout;
	uint32_t		 bits;
	uint32_t		 option;
	uint32_t		 rcv_size;
	mach_port_name_t	 rcv_name;
	long			 rv;

	msg      = (struct mach_msg_header *)f->sf_arg0;
	option   = (uint32_t)f->sf_arg1;
	rcv_size = (uint32_t)f->sf_arg3;
	rcv_name = (mach_port_name_t)f->sf_arg4;
	timeout  = (uint32_t)f->sf_arg5;	/* mach_msg_timeout_t, in ms */

	if (option & DARWIN_MACH_SEND_MSG) {
		if (syscall_copyin(&bits, &msg->msgh_bits, sizeof(bits)) != 0)
			return (darwin_ok(f, DARWIN_MACH_SEND_INVALID_DATA));
		if ((bits & MACH_MSGH_BITS_COMPLEX) != 0) {
			kprintf("darwin: MACH mach_msg complex send refused\n");
			return (darwin_ok(f, DARWIN_MACH_SEND_INVALID_TYPE));
		}
		rv = syscall_msg_send(msg);
		if (rv != MACH_MSG_OK) {
			kprintf("darwin: MACH mach_msg send -> rv=%ld\n", rv);
			return (darwin_ok(f,
			    darwin_mach_msg_err(rv, true, option)));
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
			return (darwin_ok(f,
			    darwin_mach_msg_err(rv, false, option)));
		}
	}

	kprintf("darwin: MACH mach_msg(option=0x%x) -> KERN_SUCCESS\n",
	    (unsigned)option);
	return (darwin_ok(f, DARWIN_MACH_MSG_SUCCESS));
}

/*
 * style9-private call gate (class DARWIN_SYSCALL_CLASS_STYLE9), reached
 * only from our own dyld and libSystem, never from Apple code.  See
 * darwin.h.
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
	case DARWIN_S9_pselect:
		return (darwin_s9_pselect(f));
	default:
		kprintf("darwin: unimplemented style9 call %u\n",
		    (unsigned)num);
		return (darwin_err(f, DARWIN_ENOSYS));
	}
}

/*
 * map_image(const char *path): map the embedded dylib registered under
 * `path` at the task's next dylib base and return that base.  dyld passes
 * the name from an LC_LOAD_DYLIB; the kernel, which holds the blob, does
 * the mapping.  Carry set with a Darwin errno on failure (unknown path,
 * fault, map error).
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
 * Metadata for a program-registry entry, which lives in the kernel image.
 * Every timestamp is boot time, computed as wall time minus uptime (the
 * RTC anchor clock_init took) so repeated stats agree; 0 with no usable
 * RTC.  The mode is read-only and executable.
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
 * fs_stat(const char *path, struct fs_statbuf *out): the probe behind
 * libSystem's stat$INODE64.  Copies out the neutral fs_statbuf and returns
 * 0, or carry set (ENOENT, EFAULT, ENAMETOOLONG).  libSystem converts it to
 * Apple's struct stat, keeping the macOS layout out of the kernel.
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
	 * /bin is an overlay (see fs_readdir below), so the directory keeps
	 * a synthetic inode even when the volume has a /bin.  Names under it
	 * resolve registry first.
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
 * libSystem's opendir/readdir.  Returns 1 when an entry was written, 0 at
 * end-of-directory (carry clear either way), carry set on error.  No
 * cursor is kept: each call re-resolves and re-scans to `index`.
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
		 * /bin is an overlay: the volume's entries, then the program
		 * registry, so the listing agrees with open() and stat().
		 */
		if (fs_readdir(path, index, &de) != 1) {
			/*
			 * Past the volume's entries; count them, since the
			 * registry's numbering starts where the disk's stops.
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
			 * End of the on-disk listing.  The root gets one
			 * synthetic "bin" at exactly the first end index (the
			 * probe at index-1 proves it), so a walker finds the
			 * program registry.
			 */
			if (!darwin_streq(path, "/"))
				return (darwin_ok(f, 0));
			if (index > 0 &&
			    fs_readdir(path, index - 1, &de) != 1)
				return (darwin_ok(f, 0));
			/* Unless the volume has its own /bin, already listed. */
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
 * holds for libSystem's fstat64, which maps the kinds onto S_IFREG /
 * S_IFCHR / S_IFIFO / S_IFDIR; the implicit std streams (FREE at 0..2)
 * classify as the console they reach.
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
	ds.fds_ino  = 0;
	switch (of->of_type) {
	case DARWIN_OF_FREE:
		if (fd > 2)
			return (darwin_err(f, DARWIN_EBADF));
		ds.fds_kind = DARWIN_FDSTAT_CHR;
		break;
	case DARWIN_OF_CONSOLE:
	case DARWIN_OF_NULL:
		ds.fds_kind = DARWIN_FDSTAT_CHR;
		break;
	case DARWIN_OF_FILE:
		ds.fds_size = (uint32_t)darwin_file_len(of);
		ds.fds_kind = DARWIN_FDSTAT_REG;
		ds.fds_ino  = of->of_handle.fh_ino;
		break;
	case DARWIN_OF_DIR: {
		struct fs_statbuf	sb;

		ds.fds_kind = DARWIN_FDSTAT_DIR;
		if (of->of_path != NULL && fs_stat(of->of_path, &sb) == FS_E_OK)
			ds.fds_ino = sb.fs_ino;
		break;
	}
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
 * fs_fdpath(int fd, char *buf, size_t cap): the path an fd was opened by.
 * With no vnodes, this is what libSystem builds the *at family,
 * fdopendir, fchdir and fchmod on.  A file renamed since the open answers
 * with its old name; the fix is an inode-keyed answer, not a patch.
 *
 * Returns the length; EBADF if not open, EINVAL if it has no name (pipe,
 * console, built-in), ERANGE if `cap' is too small.
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
 * The Darwin identity reported through uname(): release 23.x (macOS 14
 * Sonoma) on x86_64, a plausible Mac for tools that print it, with a
 * version banner that says style9.  The hostname matches libSystem's
 * gethostname() ("style9").
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
 * uname(struct darwin_uname *out): copy the identity out; libSystem
 * reshapes it into Apple's struct utsname.  Returns 0, or EFAULT.
 */
static long
darwin_s9_uname(struct syscall_frame *f)
{

	if (syscall_copyout((void *)f->sf_arg0, &darwin_uname_id,
	    sizeof(darwin_uname_id)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 0));
}

/* NUL-terminated string equality. */
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
 * A name in the Darwin view of /bin.  One alias: "sh" is dash.  Programs
 * (make's default SHELL) ask for /bin/sh, but the registry's "sh" is the
 * native ELF shell, which the Darwin loader refuses.  Every Darwin-side
 * program lookup passes here, so stat, open and execve agree; the native
 * side does not, and `sh' there is still sh.elf.
 */
static const struct progreg_entry *
darwin_bin_find(const char *name)
{

	if (darwin_streq(name, "sh"))
		name = "dash";
	return (progreg_find(name));
}

/*
 * The synthetic /bin: the program registry as a directory.  A shell's PATH
 * search stat(2)s before it execve's, so the path calls must see what
 * execve runs.  Answers "/bin/<name>" for open, access and fs_stat
 * (fs_readdir walks the registry directly); execve resolves by basename
 * itself, and every other path goes to the volume.
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
	return (darwin_bin_find(path + i + 1));
}
