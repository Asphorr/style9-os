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
 * Darwin console input.  The DARWIN_OF_CONSOLE / implicit-stdin read path
 * drains this ring; darwin_cons_feed() (driven by the SYS_CONS_FEED native
 * syscall) fills it.  Deliberately ISOLATED from the kbd/uart Mach input
 * ports the native shell consumes -- a Darwin binary reading its stdin never
 * competes with sh.elf for keystrokes, and nothing it leaves behind can leak
 * into the native surface.  v1 is single-shot: a feed marks end-of-input, so
 * once the ring drains read(2) returns 0 (EOF), which an interactive shell
 * treats as ^D and exits on.
 */
#define	DARWIN_CONS_BUF		512u
#define	DARWIN_CONS_MASK	(DARWIN_CONS_BUF - 1u)

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

static char	darwin_cons_buf[DARWIN_CONS_BUF];
static uint32_t	darwin_cons_head;	/* producer: darwin_cons_feed   */
static uint32_t	darwin_cons_tail;	/* consumer: darwin_cons_read   */
static bool	darwin_cons_eof;	/* set once a feed completes    */

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
		if (task_kill_pending(current_thread->th_task))
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
				if (task_kill_pending(
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

/* Release whatever one slot holds and return it to FREE. */
static void
darwin_ofile_clear(struct darwin_ofile *of)
{

	switch (of->of_type) {
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
	of->of_pipe = NULL;
	of->of_buf  = NULL;
	of->of_path = NULL;
	of->of_size = 0;
	of->of_off  = 0;
	of->of_type = DARWIN_OF_FREE;
}

/*
 * Foreground task for console input: the id of the last Darwin task to
 * read(2) the console.  A Ctrl-C (ETX) in the feed posts SIGINT here.  This
 * is a stand-in for process groups -- enough for the common case of one
 * interactive shell draining the console.
 */
static uint64_t	darwin_cons_fg_id;

/*
 * Append up to `n` bytes of console input to the ring, dropping any that
 * would overflow (scripted feeds are far smaller than DARWIN_CONS_BUF).
 * An ETX (Ctrl-C, 0x03) is not enqueued: it posts SIGINT to the foreground
 * task instead.  Marks end-of-input: a feed is a whole-input, single-shot
 * delivery, so the reader returns EOF once it drains what was queued here.
 * Driven by the SYS_CONS_FEED native syscall (kern/syscall.c).
 */
void
darwin_cons_feed(const char *buf, size_t n)
{
	struct task	*fg;
	size_t		 i;
	uint32_t	 next;

	for (i = 0; i < n; i++) {
		if (buf[i] == 0x03) {		/* Ctrl-C -> SIGINT to fg task */
			fg = task_lookup_ref(darwin_cons_fg_id);
			if (fg != NULL) {
				darwin_signal_post(fg, DARWIN_SIGINT);
				task_deref(fg);
			}
			continue;
		}
		next = darwin_cons_head + 1u;
		if (next - darwin_cons_tail > DARWIN_CONS_BUF)
			break;
		darwin_cons_buf[darwin_cons_head & DARWIN_CONS_MASK] = buf[i];
		darwin_cons_head = next;
	}
	darwin_cons_eof = true;
}

/*
 * read(2) on a console fd (implicit stdin or an explicit CONSOLE slot):
 * serve one line of console input.  Drains the feed ring up to `n` bytes,
 * stopping after a newline -- canonical-mode shape, so an interactive
 * shell gets one complete line per read -- and echoes each consumed byte
 * so a scripted session reads like a live terminal.  An empty ring
 * returns EOF (0) once the feed is exhausted; otherwise it yields until
 * the producer delivers more.
 */
static long
darwin_cons_read(struct syscall_frame *f, void *ubuf, size_t n)
{
	char	line[256];
	size_t	got;
	char	c;

	got = 0;
	darwin_cons_fg_id = current_thread->th_task->t_id;
	if (n > sizeof(line))
		n = sizeof(line);

	for (;;) {
		while (got < n && darwin_cons_tail != darwin_cons_head) {
			c = darwin_cons_buf[darwin_cons_tail & DARWIN_CONS_MASK];
			darwin_cons_tail++;
			line[got++] = c;
			tty_putc(c);
			if (c == '\n')
				break;
		}
		if (got > 0)
			break;
		if (darwin_cons_eof)
			return (darwin_ok(f, 0));	/* EOF */
		/*
		 * A pending, unblocked signal (e.g. SIGINT from Ctrl-C) breaks
		 * the blocking read with EINTR; the syscall-exit path then
		 * delivers it -- a caught handler runs, an uncaught SIGINT
		 * terminates.  Mirrors the pipe read's interrupt check.
		 */
		if ((current_thread->th_task->t_sig_pending &
		    ~current_thread->th_task->t_sig_mask) != 0)
			return (darwin_err(f, DARWIN_EINTR));
		thread_yield();
	}

	if (syscall_copyout(ubuf, line, got) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, (long)got));
}

void
darwin_files_teardown(struct task *t)
{
	size_t	i;

	for (i = 0; i < DARWIN_NOFILE; i++) {
		if (t->t_darwin_files[i].of_type != DARWIN_OF_FREE)
			darwin_ofile_clear(&t->t_darwin_files[i]);
	}
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
		case DARWIN_OF_FILE:
			/*
			 * Private copy, private cursor.  POSIX shares the
			 * offset through the open-file description; for
			 * the read-only slurped files this serves, cursor
			 * divergence after fork is unobservable.
			 */
			buf = kmalloc(src->of_size != 0 ?
			    src->of_size : 1);
			if (buf == NULL)
				return (-1);
			for (k = 0; k < src->of_size; k++)
				buf[k] = src->of_buf[k];
			dst->of_buf  = buf;
			dst->of_path = darwin_path_dup(src->of_path);
			dst->of_size = src->of_size;
			dst->of_off  = src->of_off;
			dst->of_type = DARWIN_OF_FILE;
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
	case DARWIN_OF_FILE:
		buf = kmalloc(src->of_size != 0 ?
		    src->of_size : 1);
		if (buf == NULL)
			return (-DARWIN_ENOMEM);
		for (k = 0; k < src->of_size; k++)
			buf[k] = src->of_buf[k];
		dst->of_buf  = buf;
		dst->of_path = darwin_path_dup(src->of_path);
		dst->of_size = src->of_size;
		dst->of_off  = src->of_off;
		dst->of_type = DARWIN_OF_FILE;
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
		char				 path[256];
		const struct progreg_entry	*pe;
		struct task			*t;
		uint8_t				*buf;
		uint32_t			 size;
		uint32_t			 k;
		long				 len;
		int				 fd;
		int				 rv;
		bool				 on_disk;

		len = syscall_copyin_str((const char *)f->sf_arg0, path,
		    sizeof(path));
		if (len < 0) {
			kprintf("darwin: open: bad path pointer\n");
			return (darwin_err(f, DARWIN_EFAULT));
		}

		/*
		 * Everything reachable here is read-only; say so instead of
		 * accepting a write-mode open whose writes would then vanish.
		 * O_WRONLY/O_RDWR live in the low access-mode bits; O_APPEND
		 * is 0x8, O_CREAT 0x200, O_TRUNC 0x400 (Darwin <sys/fcntl.h>).
		 */
		if ((f->sf_arg1 & 3) != 0 || (f->sf_arg1 & 0x608) != 0)
			return (darwin_err(f, DARWIN_EROFS));

		on_disk = true;
		rv = fs_slurp(path, &buf, &size);
		if (rv == FS_E_NOTFOUND || rv == FS_E_NOMOUNT) {
			/* Not on the disk: try the synthetic /bin (progreg). */
			pe = darwin_bin_lookup(path);
			if (pe == NULL)
				return (darwin_err(f, DARWIN_ENOENT));
			if (pe->pr_size > 0x7FFFFFFF)
				return (darwin_err(f, DARWIN_ENOMEM));
			buf = kmalloc(pe->pr_size != 0 ?
			    pe->pr_size : 1);
			if (buf == NULL)
				return (darwin_err(f, DARWIN_ENOMEM));
			for (k = 0; k < (uint32_t)pe->pr_size; k++)
				buf[k] = pe->pr_image[k];
			size = (uint32_t)pe->pr_size;
			on_disk = false;
		} else if (rv != FS_E_OK) {
			kprintf("darwin: open('%s') -> %s rv=%d\n",
			    path, fs_kind(), rv);
			if (rv == FS_E_NOMEM || rv == FS_E_TOOBIG)
				return (darwin_err(f, DARWIN_ENOMEM));
			return (darwin_err(f, DARWIN_EIO));
		}

		t  = current_thread->th_task;
		fd = darwin_fd_alloc(t);
		if (fd < 0) {
			kfree(buf);
			return (darwin_err(f, DARWIN_EMFILE));
		}
		t->t_darwin_files[fd].of_buf  = buf;
		/*
		 * Only a file that really is on the volume gets a name kept
		 * for it.  The synthetic /bin entries are built into the
		 * kernel image and no path resolves to them, so an fd on one
		 * is readable but not mappable -- better than handing mmap a
		 * name its pager would fail on at the first fault.
		 */
		t->t_darwin_files[fd].of_path =
		    on_disk ? darwin_path_dup(path) : NULL;
		t->t_darwin_files[fd].of_size = size;
		t->t_darwin_files[fd].of_off  = 0;
		t->t_darwin_files[fd].of_type = DARWIN_OF_FILE;
		kprintf("darwin: UNIX open('%s') -> fd=%d (%u bytes)\n",
		    path, fd, (unsigned)size);
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
		case DARWIN_OF_FILE:
			avail = of->of_size - of->of_off;
			if (n > (size_t)avail)
				n = avail;
			if (n > 0) {
				rv = syscall_copyout((void *)f->sf_arg1,
				    of->of_buf + of->of_off, n);
				if (rv != 0)
					return (darwin_err(f, DARWIN_EFAULT));
				of->of_off += (uint32_t)n;
			}
			return (darwin_ok(f, (long)n));
		case DARWIN_OF_PIPE_R:
			return (darwin_pipe_read(f, of->of_pipe,
			    (void *)f->sf_arg1, n));
		default:
			return (darwin_err(f, DARWIN_EBADF));
		}
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
			if (task_kill_pending(t))
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
			if (of->of_path == NULL)
				return (darwin_err(f, DARWIN_ENODEV));
			if ((off & 0xFFFull) != 0)
				return (darwin_err(f, DARWIN_EINVAL));
			obj = vm_object_file(of->of_path, of->of_size);
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
		 * Whole mappings only.  Unmapping the middle of a range means
		 * splitting an entry in two, which this vm_map cannot do yet;
		 * refusing is honest, where succeeding without doing it would
		 * leave the caller believing memory had been returned.
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
	char				 path[256];
	struct fs_statbuf		 sb;
	const struct progreg_entry	*pe;
	long				 n;
	int				 rv;

	n = syscall_copyin_str((const char *)f->sf_arg0, path, sizeof(path));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));

	/*
	 * /bin answers as the overlay it is (see fs_readdir below): the
	 * directory itself is neither the volume's nor the registry's alone,
	 * so it keeps a stable synthetic inode even when the volume has a
	 * /bin.  Everything under it resolves normally, registry first.
	 */
	if (darwin_streq(path, DARWIN_BIN_DIR)) {
		sb.fs_size   = 0;
		sb.fs_ino    = DARWIN_BIN_INO_BASE;
		sb.fs_is_dir = 1;
	} else if ((pe = darwin_bin_lookup(path)) != NULL) {
		sb.fs_size   = pe->pr_size;
		sb.fs_ino    = DARWIN_BIN_INO_BASE + 1 +
		    (uint32_t)(pe - progreg_at(0));
		sb.fs_is_dir = 0;
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
	char				 path[256];
	struct fs_dirent		 de;
	struct fs_statbuf		 sb;
	const struct progreg_entry	*pe;
	uint32_t			 index;
	uint32_t			 nreal;
	long				 n;
	int				 i;
	int				 rv;

	n = syscall_copyin_str((const char *)f->sf_arg0, path, sizeof(path));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));
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
