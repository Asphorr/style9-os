/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "darwin.h"
#include "fs_fat.h"
#include "host.h"
#include "kmem.h"
#include "kprintf.h"
#include "macho.h"
#include "panic.h"
#include "port.h"
#include "progreg.h"
#include "sched.h"
#include "spinlock.h"
#include "syscall.h"
#include "task.h"
#include "thread.h"

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

static long	darwin_unix(struct syscall_frame *f, uint32_t nr);
static long	darwin_mach(struct syscall_frame *f, uint32_t trap);
static long	darwin_mach_msg(struct syscall_frame *f);
static long	darwin_mach_msg_err(long rv, bool sending);
static long	darwin_style9(struct syscall_frame *f, uint32_t num);
static long	darwin_s9_map_image(struct syscall_frame *f);
static long	darwin_s9_fs_stat(struct syscall_frame *f);
static long	darwin_s9_fs_readdir(struct syscall_frame *f);
static long	darwin_s9_uname(struct syscall_frame *f);
static bool	darwin_streq(const char *a, const char *b);

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

	p = (struct darwin_pipe *)kmalloc(sizeof(*p));
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
 * darwin_fd_alloc returns the lowest FREE slot at 3 or above (0..2 keep
 * their implicit std-stream meaning until dup2 explicitly retargets
 * them), or -1 when the table is full.
 */
static int
darwin_fd_alloc(struct task *t)
{
	int	i;

	for (i = 3; i < DARWIN_NOFILE; i++) {
		if (t->t_darwin_files[i].of_type == DARWIN_OF_FREE)
			return (i);
	}
	return (-1);
}

/* Release whatever one slot holds and return it to FREE. */
static void
darwin_ofile_clear(struct darwin_ofile *of)
{

	switch (of->of_type) {
	case DARWIN_OF_FILE:
		if (of->of_buf != NULL)
			kfree(of->of_buf);
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
	of->of_size = 0;
	of->of_off  = 0;
	of->of_type = DARWIN_OF_FREE;
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
			buf = (uint8_t *)kmalloc(src->of_size != 0 ?
			    src->of_size : 1);
			if (buf == NULL)
				return (-1);
			for (k = 0; k < src->of_size; k++)
				buf[k] = src->of_buf[k];
			dst->of_buf  = buf;
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
		buf = (uint8_t *)kmalloc(src->of_size != 0 ?
		    src->of_size : 1);
		if (buf == NULL)
			return (-DARWIN_ENOMEM);
		for (k = 0; k < src->of_size; k++)
			buf[k] = src->of_buf[k];
		dst->of_buf  = buf;
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
		return;
	}
	darwin_zombies[slot].z_pid    = pid;
	darwin_zombies[slot].z_ppid   = ppid;
	darwin_zombies[slot].z_status = status;
	darwin_zombies[slot].z_used   = true;
	spin_unlock(&darwin_zombie_lock);
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
		char		 path[256];
		struct task	*t;
		uint8_t		*buf;
		uint32_t	 size;
		long		 len;
		int		 fd;
		int		 rv;

		/* Read-only FS: the O_* flags (sf_arg1) and mode are ignored. */
		len = syscall_copyin_str((const char *)f->sf_arg0, path,
		    sizeof(path));
		if (len < 0) {
			kprintf("darwin: open: bad path pointer\n");
			return (darwin_err(f, DARWIN_EFAULT));
		}

		rv = fs_fat_slurp(path, &buf, &size);
		if (rv != FS_FAT_E_OK) {
			kprintf("darwin: open('%s') -> fs_fat rv=%d\n",
			    path, rv);
			if (rv == FS_FAT_E_NOTFOUND || rv == FS_FAT_E_NOMOUNT)
				return (darwin_err(f, DARWIN_ENOENT));
			if (rv == FS_FAT_E_NOMEM || rv == FS_FAT_E_TOOBIG)
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
			if (fd == 0)	/* legacy stdin: nothing -> EOF */
				return (darwin_ok(f, 0));
			return (darwin_err(f, DARWIN_EBADF));
		case DARWIN_OF_CONSOLE:
			return (darwin_ok(f, 0));	/* no console input */
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
		long		 pid;
		int		 sig;

		pid = (long)f->sf_arg0;
		sig = (int)f->sf_arg1;
		if (pid <= 0)
			return (darwin_err(f, DARWIN_EINVAL));
		target = task_lookup_ref((uint64_t)pid);
		if (target == NULL)
			return (darwin_err(f, DARWIN_ESRCH));
		if (sig != 0) {
			/*
			 * No signal delivery exists -- kill IS terminate.
			 * Record the wait4 status here (termsig in the
			 * low bits): a terminated task never reaches its
			 * own exit(2), so this is the only writer.
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
	case DARWIN_SYS_sigaction:
	case DARWIN_SYS_sigprocmask:
	case DARWIN_SYS_setitimer:
		/*
		 * Signal delivery does not exist yet; these succeed
		 * without recording anything.  Enough for tools that
		 * install handlers defensively (timeout, shells) on runs
		 * where no signal ever fires.
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
 * fs_stat(const char *path, struct fs_fat_statbuf *out): existence + size +
 * type + inode probe behind libSystem's stat$INODE64.  Copies the small
 * fs_fat_statbuf out to the caller and returns 0 (carry clear) if the file is
 * present, carry set otherwise.  The kernel reports only this neutral struct;
 * libSystem turns it into Apple's struct stat, so the macOS ABI layout stays
 * out of the kernel.
 */
static long
darwin_s9_fs_stat(struct syscall_frame *f)
{
	char			path[256];
	struct fs_fat_statbuf	sb;
	long			n;
	int			rv;

	n = syscall_copyin_str((const char *)f->sf_arg0, path, sizeof(path));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));

	rv = fs_fat_stat2(path, &sb);
	if (rv != FS_FAT_E_OK)
		return (darwin_err(f, DARWIN_ENOENT));
	if (syscall_copyout((void *)f->sf_arg1, &sb, sizeof(sb)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 0));
}

/*
 * fs_readdir(const char *path, uint32_t index, struct fs_fat_dirent *out):
 * fill *out with the index-th entry of the directory at `path`, behind
 * libSystem's opendir/readdir.  Returns 1 in %rax when an entry was written, 0
 * at end-of-directory (carry clear either way), carry set on error.  The
 * kernel keeps no per-fd cursor: each call re-resolves and re-scans to
 * `index`, which is cheap for the small read-only directories this serves.
 */
static long
darwin_s9_fs_readdir(struct syscall_frame *f)
{
	char			path[256];
	struct fs_fat_dirent	de;
	long			n;
	int			rv;

	n = syscall_copyin_str((const char *)f->sf_arg0, path, sizeof(path));
	if (n < 0)
		return (darwin_err(f, DARWIN_EFAULT));

	rv = fs_fat_readdir(path, (uint32_t)f->sf_arg1, &de);
	if (rv < 0)
		return (darwin_err(f, DARWIN_ENOENT));
	if (rv == 0)
		return (darwin_ok(f, 0));		/* end of directory */
	if (syscall_copyout((void *)f->sf_arg2, &de, sizeof(de)) != 0)
		return (darwin_err(f, DARWIN_EFAULT));
	return (darwin_ok(f, 1));
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
