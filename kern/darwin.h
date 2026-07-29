/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DARWIN_H_
#define	_SYS_DARWIN_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Darwin (XNU) syscall personality -- the second rung of the Mach-O
 * compatibility ladder (S2).  S1 taught the kernel to map the Mach-O
 * container (kern/macho.c); this teaches it to answer the syscalls a
 * genuine Apple-ABI binary issues.  A task is opted in when its image
 * declared PLATFORM_MACOS (see macho.h / TASK_PERSONALITY_DARWIN); for
 * those tasks syscall_dispatch calls darwin_dispatch instead of the native
 * style9 table, leaving the native path untouched.
 *
 * Apple encodes a CLASS in the high byte of the syscall number in %rax and
 * the call number in the low 24 bits.  The argument registers
 * (rdi, rsi, rdx, r10, r8, r9) are exactly the order the style9 entry stub
 * already marshals into struct syscall_frame, so no register shuffling is
 * needed -- only the number decode and the return convention differ.
 *
 * We honour a deliberately small subset, enough to prove the personality
 * end to end and translate each call onto the style9 primitive that already
 * implements it.  The mach_msg trap (S3) routes onto the kernel's existing
 * message path; MIG stub generation and a real libSystem stay userspace/S4.
 */

#define	DARWIN_SYSCALL_CLASS_SHIFT	24
#define	DARWIN_SYSCALL_CLASS_MASK	0xFFu
#define	DARWIN_SYSCALL_NUMBER_MASK	0x00FFFFFFu

/* Syscall classes (xnu osfmk/mach/i386/syscall_sw.h). */
#define	DARWIN_SYSCALL_CLASS_NONE	0
#define	DARWIN_SYSCALL_CLASS_MACH	1	/* Mach trap gate          */
#define	DARWIN_SYSCALL_CLASS_UNIX	2	/* BSD/Unix call gate      */
#define	DARWIN_SYSCALL_CLASS_MDEP	3	/* machine-dependent       */
#define	DARWIN_SYSCALL_CLASS_DIAG	4	/* diagnostics             */
#define	DARWIN_SYSCALL_CLASS_IPC	5	/* IPC                     */

/*
 * style9-private syscall class -- NOT one of Apple's.  Our clean-room dyld
 * (user/dyld.c) issues it to ask the kernel to map a dependency by path: the
 * stand-in for the open()+mmap() / dyld-shared-cache machinery we deliberately
 * do not have.  A genuine Apple binary never encodes this class; only our own
 * linker does.  Chosen well clear of Apple's 0..5 so the two cannot collide.
 */
#define	DARWIN_SYSCALL_CLASS_STYLE9	0x2A

/*
 * BSD (class 2) call numbers we translate -- Darwin's numbering from
 * bsd/kern/syscalls.master, NOT Linux's.  A class-2 call returns its result
 * in %rax with the carry flag clear, or a positive errno in %rax with carry
 * set; see darwin.c.
 */
#define	DARWIN_SYS_exit		1
#define	DARWIN_SYS_fork		2
#define	DARWIN_SYS_read		3
#define	DARWIN_SYS_write	4
#define	DARWIN_SYS_open		5
#define	DARWIN_SYS_close	6
#define	DARWIN_SYS_wait4	7
#define	DARWIN_SYS_unlink	10
#define	DARWIN_SYS_chdir	12
#define	DARWIN_SYS_chmod	15
#define	DARWIN_SYS_getpid	20
#define	DARWIN_SYS_kill		37
#define	DARWIN_SYS_getppid	39
#define	DARWIN_SYS_dup		41
#define	DARWIN_SYS_pipe		42
#define	DARWIN_SYS_sigaction	46
#define	DARWIN_SYS_sigprocmask	48
#define	DARWIN_SYS_ioctl	54
#define	DARWIN_SYS_execve	59
#define	DARWIN_SYS_umask	60
#define	DARWIN_SYS_munmap	73
#define	DARWIN_SYS_setitimer	83
#define	DARWIN_SYS_dup2		90
#define	DARWIN_SYS_gettimeofday	116
#define	DARWIN_SYS_fchmod	124
#define	DARWIN_SYS_fcntl	92
#define	DARWIN_SYS_rename	128
#define	DARWIN_SYS_mkdir	136
#define	DARWIN_SYS_rmdir	137
#define	DARWIN_SYS_sigreturn	184
#define	DARWIN_SYS_mmap		197
#define	DARWIN_SYS_lseek	199
/*
 * getcwd(3) is not a syscall on Darwin; libc calls __getcwd(2), which fills a
 * caller-supplied buffer with the absolute path or fails with ERANGE.  The
 * number is Darwin's, not an invention.
 */
#define	DARWIN_SYS___getcwd	326

/*
 * open(2)'s vocabulary, as Darwin's <sys/fcntl.h> spells it.
 *
 * The access mode is NOT a bitmask of the other flags: it is the low two bits,
 * where 0 means read, 1 write and 2 both -- so "is this open for writing" is a
 * comparison and not a bit test, and treating it as one is how O_RDONLY|O_TRUNC
 * (a legal, if odd, request) gets mistaken for a write.
 */
#define	DARWIN_O_ACCMODE	0x0003
#define	DARWIN_O_RDONLY		0x0000
#define	DARWIN_O_WRONLY		0x0001
#define	DARWIN_O_RDWR		0x0002
#define	DARWIN_O_APPEND		0x0008
#define	DARWIN_O_CREAT		0x0200
#define	DARWIN_O_TRUNC		0x0400
#define	DARWIN_O_EXCL		0x0800

/*
 * mmap(2)'s vocabulary, as <sys/mman.h> spells it on Darwin.  The protection
 * bits happen to match this kernel's VM_PROT_READ/WRITE/EXEC one for one --
 * both descend from the same BSD ancestor -- but the syscall layer translates
 * them explicitly rather than trusting a coincidence to survive.
 */
#define	DARWIN_PROT_NONE	0x00
#define	DARWIN_PROT_READ	0x01
#define	DARWIN_PROT_WRITE	0x02
#define	DARWIN_PROT_EXEC	0x04

#define	DARWIN_MAP_SHARED	0x0001
#define	DARWIN_MAP_PRIVATE	0x0002
#define	DARWIN_MAP_FIXED	0x0010
#define	DARWIN_MAP_ANON		0x1000

/* wait4 option bits (Darwin <sys/wait.h>). */
#define	DARWIN_WNOHANG	1

/*
 * Signal numbers we act on (Darwin <sys/signal.h>).  A task's disposition
 * for each lives in struct task.t_sig_handler[]; posting, masking, default
 * actions, and return-to-user delivery are in kern/darwin.c.
 */
#define	DARWIN_SIGINT	2	/* interrupt (Ctrl-C)        -- default terminate */
#define	DARWIN_SIGKILL	9	/* uncatchable kill          -- always terminate  */
#define	DARWIN_SIGPIPE	13	/* write to reader-less pipe -- default terminate */
#define	DARWIN_SIGTERM	15	/* termination request       -- default terminate */
#define	DARWIN_SIGCHLD	20	/* child exited/stopped      -- default ignore    */

/* sigprocmask(2) `how` values (Darwin); 0 is our "no change" (query only). */
#define	DARWIN_SIG_BLOCK	1
#define	DARWIN_SIG_UNBLOCK	2
#define	DARWIN_SIG_SETMASK	3

/*
 * fcntl(2) commands (Darwin <sys/fcntl.h>).  F_DUPFD is the one with real
 * semantics here (a shell parks its saved fds at 10+ with it); the fd-flag
 * and status-flag commands are accepted and answer 0 -- there is nothing
 * to set on this fd table (no close-on-exec: exec already preserves fds
 * deliberately, and a shell's FD_CLOEXEC requests are about hygiene it
 * re-establishes anyway).
 */
#define	DARWIN_F_DUPFD		0
#define	DARWIN_F_GETFD		1
#define	DARWIN_F_SETFD		2
#define	DARWIN_F_GETFL		3
#define	DARWIN_F_SETFL		4
#define	DARWIN_F_DUPFD_CLOEXEC	67	/* dash's savefd uses this one */

/*
 * Mach traps (class 1) we answer -- positive indices into xnu's
 * mach_trap_table.  These return a port name or a kern_return_t directly in
 * %rax with no carry convention.
 */
#define	DARWIN_MACH_mach_reply_port	26
#define	DARWIN_MACH_thread_self_trap	27
#define	DARWIN_MACH_task_self_trap	28
#define	DARWIN_MACH_host_self_trap	29	/* mach_host_self()            */
#define	DARWIN_MACH_mach_msg_trap	31	/* classic combined mach_msg() */

/*
 * style9-private calls (class DARWIN_SYSCALL_CLASS_STYLE9), issued only by our
 * dyld.  map_image(const char *path) maps the embedded dylib registered under
 * `path` into the caller's task and returns the base it landed at in %rax (0 +
 * carry on failure).
 */
#define	DARWIN_S9_dyld_map_image	1

/*
 * fs_stat(const char *path, struct fs_fat_statbuf *out): report whether a file
 * exists in the read-only FS, plus its size / type / inode, WITHOUT the kernel
 * knowing anything about Apple's struct stat.  libSystem's stat$INODE64 issues
 * this, then fills the macOS-ABI struct itself (keeping the layout knowledge in
 * the clean-room ABI layer, not the kernel).  Copies the small fs_fat_statbuf
 * (fs/fat/fat.h) out to *out; returns 0 in %rax (carry clear), or carry set on
 * absence.
 *
 * fs_readdir(const char *path, uint32_t index, struct fs_fat_dirent *out):
 * fill *out with the index-th entry of the directory at `path`.  Returns 1 in
 * %rax when an entry was written, 0 at end-of-directory, carry set on error.
 * libSystem's opendir/readdir drive it (stateless: re-resolved per index).
 */
#define	DARWIN_S9_fs_stat		2
#define	DARWIN_S9_fs_readdir		3
#define	DARWIN_S9_uname			4
#define	DARWIN_S9_fs_fstat		5
#define	DARWIN_S9_fs_fdpath		6

/*
 * fs_fstat(int fd, struct darwin_fdstat *out): the fd-flavored sibling of
 * fs_stat, behind libSystem's fstat64.  The kernel classifies what the fd
 * actually holds (regular buffered file / console / pipe end) into this
 * neutral struct and libSystem reshapes it into Apple's struct stat --
 * same division of ABI knowledge as fs_stat.  Returns 0 (carry clear) or
 * carry set with EBADF/EFAULT.
 */
#define	DARWIN_FDSTAT_REG	0
#define	DARWIN_FDSTAT_CHR	1
#define	DARWIN_FDSTAT_FIFO	2
#define	DARWIN_FDSTAT_DIR	3

struct darwin_fdstat {
	uint32_t	fds_size;	/* byte length (regular files)   */
	uint8_t		fds_kind;	/* DARWIN_FDSTAT_*               */
};

/*
 * THE TERMINAL, as Darwin describes one.
 *
 * struct termios is written into and read out of a genuine Apple binary's own
 * storage, so none of this is ours to choose.  The layout is not guessed
 * either: Darwin encodes the size of the argument INTO the ioctl number --
 * _IOR('t', 19, struct termios) is 0x40000000 | (size << 16) | ('t' << 8) | 19
 * -- and the number the world calls TIOCGETA is 0x40487413, which says the
 * size is 0x48.  The _Static_assert below is the same claim, made by the
 * compiler.  (FIONREAD, 0x4004667f, was already in this tree and encodes a
 * 4-byte int the same way, which is how the arithmetic was checked.)
 *
 * tcflag_t and speed_t are unsigned long -- 64 bits here -- and cc_t is a
 * single byte, so c_cc lands at 32 and the four bytes after it are pure
 * alignment, exactly like the padding spelled out in struct timeval below.
 */
#define	DARWIN_NCCS	20

struct darwin_termios {
	uint64_t	c_iflag;
	uint64_t	c_oflag;
	uint64_t	c_cflag;
	uint64_t	c_lflag;
	uint8_t		c_cc[DARWIN_NCCS];
	uint8_t		c_pad[4];
	uint64_t	c_ispeed;
	uint64_t	c_ospeed;
};

_Static_assert(sizeof(struct darwin_termios) == 72,
    "struct termios is the 0x48 in TIOCGETA");
_Static_assert(__builtin_offsetof(struct darwin_termios, c_cc) == 32,
    "c_cc follows four 64-bit flag words");
_Static_assert(__builtin_offsetof(struct darwin_termios, c_ispeed) == 56,
    "the speeds are aligned past c_cc's four bytes of padding");

/* struct winsize: four u_shorts, and the 0x0008 in TIOCGWINSZ. */
struct darwin_winsize {
	uint16_t	ws_row;
	uint16_t	ws_col;
	uint16_t	ws_xpixel;
	uint16_t	ws_ypixel;
};

_Static_assert(sizeof(struct darwin_winsize) == 8, "the 8 in TIOCGWINSZ");

#define	DARWIN_TIOCGETA		0x40487413UL	/* _IOR('t', 19, termios) */
#define	DARWIN_TIOCSETA		0x80487414UL	/* _IOW('t', 20, termios) */
#define	DARWIN_TIOCSETAW	0x80487415UL	/* ..., drain first       */
#define	DARWIN_TIOCSETAF	0x80487416UL	/* ..., drain and flush   */
#define	DARWIN_TIOCGWINSZ	0x40087468UL	/* _IOR('t', 104, winsize) */
#define	DARWIN_TIOCSWINSZ	0x80087467UL	/* _IOW('t', 103, winsize) */
#define	DARWIN_FIONREAD		0x4004667FUL	/* _IOR('f', 127, int)    */

/* c_iflag */
#define	DARWIN_BRKINT	0x00000002UL
#define	DARWIN_ICRNL	0x00000100UL
#define	DARWIN_IXON	0x00000200UL
#define	DARWIN_IMAXBEL	0x00002000UL

/* c_oflag */
#define	DARWIN_OPOST	0x00000001UL
#define	DARWIN_ONLCR	0x00000002UL

/* c_cflag */
#define	DARWIN_CS8	0x00000300UL
#define	DARWIN_CREAD	0x00000800UL
#define	DARWIN_CLOCAL	0x00008000UL

/* c_lflag -- the four this discipline actually acts on, and their neighbours */
#define	DARWIN_ECHOKE	0x00000001UL
#define	DARWIN_ECHOE	0x00000002UL
#define	DARWIN_ECHOK	0x00000004UL
#define	DARWIN_ECHO	0x00000008UL
#define	DARWIN_ECHONL	0x00000010UL
#define	DARWIN_ECHOCTL	0x00000040UL
#define	DARWIN_ISIG	0x00000080UL
#define	DARWIN_ICANON	0x00000100UL
#define	DARWIN_IEXTEN	0x00000400UL

/* c_cc subscripts, Darwin's order */
#define	DARWIN_VEOF	0
#define	DARWIN_VERASE	3
#define	DARWIN_VKILL	5
#define	DARWIN_VINTR	8
#define	DARWIN_VQUIT	9
#define	DARWIN_VSUSP	10
#define	DARWIN_VSTART	12
#define	DARWIN_VSTOP	13
#define	DARWIN_VMIN	16
#define	DARWIN_VTIME	17

/*
 * The speed a terminal that is not a serial line reports.  BSD keeps the
 * number itself rather than an index, so this IS 38400 baud.
 */
#define	DARWIN_B38400	38400UL

/*
 * struct timeval, EXACTLY as x86_64 Darwin lays it out -- this one is written
 * into a genuine Apple binary's own storage, so the layout is not ours to
 * choose.  tv_sec is time_t (64-bit); tv_usec is suseconds_t, a 32-BIT int at
 * offset 8, with the last four bytes pure alignment padding.  Spelling the
 * padding out beats writing a 64-bit microsecond field and relying on
 * little-endian byte order to make it come out right.
 *
 * The time is UTC.  There is no timezone database in this kernel, and the
 * second argument gettimeofday(2) accepts for one has been ignored by every
 * real system for decades.
 */
struct darwin_timeval {
	int64_t		tv_sec;
	int32_t		tv_usec;
	int32_t		tv_pad;
};

_Static_assert(sizeof(struct darwin_timeval) == 16,
    "struct timeval is 16 bytes on x86_64 Darwin");

/*
 * uname(struct darwin_uname *out): report the machine's identity card.  A
 * libSystem-only tool that asks "what am I running on?" (guname) reaches here
 * via libSystem's uname(3); the kernel is the thing claiming to be Darwin, so
 * the (fabricated) identity it answers with lives here.  As with fs_stat, the
 * kernel hands back a neutral struct and libSystem reshapes it into Apple's
 * struct utsname (256-byte fields) -- the macOS ABI layout stays in the
 * clean-room library, not the kernel.  Fields are generously sized for the
 * (long) version string; libSystem bounds the copy into utsname.  Returns 0 in
 * %rax (carry clear); carry set only if *out faults.
 */
#define	DARWIN_UNAME_FIELD	128

struct darwin_uname {
	char	un_sysname[DARWIN_UNAME_FIELD];	 /* "Darwin"            */
	char	un_nodename[DARWIN_UNAME_FIELD]; /* host name           */
	char	un_release[DARWIN_UNAME_FIELD];	 /* "23.6.0" (kernel)   */
	char	un_version[DARWIN_UNAME_FIELD];	 /* build/version banner */
	char	un_machine[DARWIN_UNAME_FIELD];	 /* "x86_64"            */
};

/*
 * Base VA at which the first dylib is mapped into a Darwin task; further
 * dylibs bump upward from there (struct task.t_darwin_dylib_next).  Inside the
 * style9 user-VA window [0x40000000, 0x80000000) and clear of the main image
 * (0x50000000), dyld (0x60000000), and the user stack (0x4000F000).
 */
#define	DARWIN_DYLIB_BASE	0x70000000ULL

/*
 * BSD errno values (Darwin <sys/errno.h>) for the failures we can produce.
 * style9 has no errno of its own -- the kernel speaks MACH_E_* / ELF_E_* --
 * so darwin.c maps its internal failures onto these on the carry-set path.
 */
#define	DARWIN_EPERM	1
#define	DARWIN_ENOENT	2
#define	DARWIN_ESRCH	3
#define	DARWIN_EINTR	4
#define	DARWIN_EIO	5
#define	DARWIN_ENOEXEC	8
#define	DARWIN_EBADF	9
#define	DARWIN_ECHILD	10
#define	DARWIN_ENOMEM	12
#define	DARWIN_EFAULT	14
#define	DARWIN_ENODEV	19
#define	DARWIN_EINVAL	22
#define	DARWIN_EMFILE	24
#define	DARWIN_ENOTTY	25
#define	DARWIN_EEXIST	17
#define	DARWIN_EISDIR	21
#define	DARWIN_ENOSPC	28
#define	DARWIN_ESPIPE	29
#define	DARWIN_EROFS	30
#define	DARWIN_EPIPE	32
#define	DARWIN_ERANGE	34
#define	DARWIN_ENAMETOOLONG	63
#define	DARWIN_ENOTDIR	20
#define	DARWIN_ENOTEMPTY	66
#define	DARWIN_ENOSYS	78

/*
 * mach_msg option flags + the mach_msg_return_t values darwin_dispatch
 * produces (Darwin <mach/message.h>).  mach_msg_trap returns one of these in
 * %rax with NO carry convention -- it is a Mach trap (class 1), so even a
 * receive timeout comes back as a code in %rax with carry clear.
 */
#define	DARWIN_MACH_SEND_MSG		0x00000001u
#define	DARWIN_MACH_RCV_MSG		0x00000002u
#define	DARWIN_MACH_SEND_TIMEOUT	0x00000010u
#define	DARWIN_MACH_RCV_TIMEOUT		0x00000100u

#define	DARWIN_MACH_MSG_SUCCESS		0x00000000
#define	DARWIN_MACH_SEND_INVALID_DATA	0x10000002
#define	DARWIN_MACH_SEND_INVALID_DEST	0x10000003
#define	DARWIN_MACH_SEND_TIMED_OUT	0x10000004
#define	DARWIN_MACH_RCV_INVALID_NAME	0x10004002
#define	DARWIN_MACH_RCV_TIMED_OUT	0x10004003
#define	DARWIN_MACH_RCV_TOO_LARGE	0x10004004
#define	DARWIN_MACH_RCV_INVALID_DATA	0x10004005

struct syscall_frame;
struct task;

/*
 * Dispatch one syscall issued by a TASK_PERSONALITY_DARWIN task.  Decodes
 * the class/number out of f->sf_nr, translates onto a style9 primitive, and
 * sets the carry flag in f->sf_user_rflags per the class's return
 * convention.  Returns the value to land in the caller's %rax.
 */
long	darwin_dispatch(struct syscall_frame *f);

/*
 * Open-file table lifecycle (kern/darwin.c).  darwin_files_teardown
 * releases every typed slot in t's table -- FILE buffers freed, pipe-end
 * references dropped -- and is the one teardown path task_deref calls.
 * darwin_files_fork_copy clones the parent's table into the child at
 * fork: FILE slots get their own copy of the buffer, CONSOLE slots copy
 * plainly, PIPE slots share the pipe object with the matching end's
 * reference count bumped.  Returns 0 or a negative SYS_E_* (the caller
 * derefs the half-built child; teardown releases what was cloned).
 */
void	darwin_files_teardown(struct task *t);
int	darwin_files_fork_copy(struct task *parent, struct task *child);

/*
 * The Darwin console (kern/darwin.c) -- a terminal in canonical mode, whose
 * line discipline lives behind darwin_cons_input: echo, erase, Ctrl-C to a
 * signal, Ctrl-D to end-of-file, and delivery a line at a time to the
 * personality's read(2) on a console fd.
 *
 * Two producers feed it.  darwin_cons_sink is the keyboard driver's: it
 * answers whether a Darwin task currently holds the console and, if so,
 * swallows the key -- the ONE place the keyboard is arbitrated between a
 * Darwin binary and the native shell's Mach input port.  darwin_cons_feed is
 * the scripted one behind the SYS_CONS_FEED native syscall, which pushes a
 * canned session through the same discipline and then declares end-of-input,
 * so the boot demo drives the interactive path rather than a parallel one.
 *
 * darwin_cons_release drops the claim when its owner dies; task teardown
 * calls it, because a claim that outlives its task routes every keystroke
 * into a ring nobody drains.
 */
bool	darwin_cons_input(char c);
bool	darwin_cons_sink(char c);
void	darwin_cons_feed(const char *buf, size_t n);
void	darwin_cons_release(struct task *t);
void	darwin_cons_stats(void);

/*
 * Zombie bookkeeping (kern/darwin.c).  Records {pid, ppid, wait4-format
 * status} for a dying Darwin task so the parent's wait4 can reap it; a
 * ppid of 0 (no Darwin parent) records nothing.  Also sweeps the dying
 * task's own unreaped zombie children -- orphans no one is left to wait
 * for.  Called from the exit(2) path and from the execve point-of-no-
 * return failure path (arch/amd64/usermode.c).
 */
void	darwin_zombie_record(unsigned long long pid, unsigned long long ppid,
	    int status);

/*
 * Signal subsystem (kern/darwin.c).  darwin_signal_post OR's `signo` into
 * `t`'s pending set from any context -- it never delivers synchronously, the
 * signal takes effect at `t`'s next return to user.  darwin_signal_deliver is
 * that return-to-user hook: it applies the current task's deliverable set,
 * silently discarding ignored (and default-ignore) signals, leaving a caught
 * signal pending for phase-2 on-stack delivery, and, for a signal whose
 * default action is terminate, recording the wait4 status and retiring the
 * thread -- NORETURN in that case.  Both no-op on a NULL task; deliver is
 * only ever called for a TASK_PERSONALITY_DARWIN task.
 */
void	darwin_signal_post(struct task *t, int signo);

/*
 * Has `t` a signal posted that will actually DO something?  The question every
 * wait in this personality has to ask before it goes round again, asked
 * through one function so the four places that wait cannot drift apart on the
 * answer -- three of them used to ask only whether the task had been killed,
 * which is a different and much narrower question.
 *
 * "Will do something" is the operative part, and posted-and-unblocked is not
 * it.  A signal whose disposition is SIG_IGN, or whose default is to be
 * ignored, is consumed on the way back to ring 3 and leaves no trace; a wait
 * that ended for one would report EINTR for nothing at all.  SIGCHLD is
 * default-ignore and arrives exactly when a parent sits in wait4, so counting
 * it makes wait4 fail the moment its child dies -- which is what the first
 * version of this did.
 *
 * A true answer delivers nothing.  It means the caller should stop waiting and
 * return EINTR; delivery happens where it always has, on the way out.
 *
 * Safe to call only on the current thread's own task: t_sig_mask and
 * t_sig_handler have a single writer and this reads them without a lock.
 */
bool	darwin_signal_pending(struct task *t);
void	darwin_signal_deliver(struct task *t);

/*
 * Phase-2 on-stack delivery.  darwin_signal_deliver_syscall is the
 * syscall-exit variant of darwin_signal_deliver: with the caller's
 * syscall_frame in hand (and `rv`, the value the syscall was about to
 * return), it can deliver a CAUGHT signal to a ring-3 handler -- it saves
 * the interrupted context into a darwin_sigframe on the user stack, then
 * reshapes the frame so the sysret enters the task's registered _sigtramp
 * with (signo, siginfo, ucontext, handler).  Default-terminate and ignore
 * are handled exactly as darwin_signal_deliver.  The trampoline calls the
 * handler then issues DARWIN_SYS_sigreturn, which restores the sigframe.
 */
struct syscall_frame;
void	darwin_signal_deliver_syscall(struct syscall_frame *f, long rv);

/*
 * Phase-3 asynchronous delivery.  darwin_signal_deliver_trap is the IRQ- and
 * fault-return variant: it delivers a caught signal to a thread that is not
 * at a syscall boundary at all -- a pure ring-3 compute loop that a timer
 * IRQ happens to interrupt.  Because the interrupted code was mid-expression
 * rather than mid-ABI-call, the saved context has to be the WHOLE machine
 * state (all 15 GPRs and the FPU file), and the resume has to be an IRETQ.
 * Default-terminate and ignore behave exactly as darwin_signal_deliver.
 */
struct trapframe;
void	darwin_signal_deliver_trap(struct trapframe *tf);

/*
 * On-stack signal context the kernel writes at delivery and reads back at
 * sigreturn.  Private to the clean-room ABI (our _sigtramp is the only
 * producer of the sigreturn call), so the layout is ours to define.
 *
 * Two flavours, told apart by the magic at offset 0 -- how much state has to
 * be saved depends on where the signal was taken:
 *
 *	SGFR1	taken at a syscall boundary.  The SysV/SYSCALL ABI already
 *		declares the argument and scratch registers dead across the
 *		call, so saving rip/rsp/rflags/rax is enough and the resume
 *		can ride the ordinary SYSRET exit.
 *	SGFR2	taken asynchronously from the IRQ path.  Nothing is dead:
 *		every GPR and the FPU file must come back bit-exact, so the
 *		frame carries all of it and sigreturn leaves via IRETQ.
 *
 * Both carry the signal mask in force at delivery: the kernel blocks the
 * signal being handled for the duration of its handler (POSIX) and restores
 * the saved mask at sigreturn.
 */
#define	DARWIN_SIGFRAME_MAGIC		0x5347465231ULL	/* "SGFR1" */
#define	DARWIN_SIGFRAME_MAGIC_FULL	0x5347465232ULL	/* "SGFR2" */

/*
 * RFLAGS bits a sigreturn is allowed to restore: CF PF AF ZF SF DF OF.  The
 * saved value comes back through the user's own stack, so it is attacker-
 * controlled in the limit; masking keeps a forged frame from handing ring 3
 * IOPL (I/O port access + CLI/STI), NT, or a single-step trap.  IF and the
 * must-be-set bit 1 are OR'd back in unconditionally.
 */
#define	DARWIN_SIGRETURN_RFLAGS_MASK	0x00000CD5ULL

struct darwin_sigframe {
	uint64_t	sf_magic;
	uint64_t	sf_signo;
	uint64_t	sf_rip;
	uint64_t	sf_rsp;
	uint64_t	sf_rflags;
	uint64_t	sf_rax;
	uint64_t	sf_mask;
	uint64_t	sf_pad;		/* size %16 == 0: keeps rsp aligned */
};

struct darwin_sigframe_full {
	uint64_t	sf_magic;
	uint64_t	sf_signo;
	uint64_t	sf_mask;
	uint64_t	sf_r15;
	uint64_t	sf_r14;
	uint64_t	sf_r13;
	uint64_t	sf_r12;
	uint64_t	sf_r11;
	uint64_t	sf_r10;
	uint64_t	sf_r9;
	uint64_t	sf_r8;
	uint64_t	sf_rdi;
	uint64_t	sf_rsi;
	uint64_t	sf_rbp;
	uint64_t	sf_rbx;
	uint64_t	sf_rdx;
	uint64_t	sf_rcx;
	uint64_t	sf_rax;
	uint64_t	sf_rip;
	uint64_t	sf_rsp;
	uint64_t	sf_rflags;
	uint8_t		sf_fpu[512] __attribute__((aligned(16)));
};

_Static_assert(sizeof(struct darwin_sigframe) % 16 == 0,
    "sigframe must be a multiple of 16 to keep the user stack aligned");
_Static_assert(sizeof(struct darwin_sigframe_full) % 16 == 0,
    "full sigframe must be a multiple of 16 to keep the user stack aligned");
_Static_assert(__builtin_offsetof(struct darwin_sigframe_full, sf_fpu) % 16 == 0,
    "FXSAVE area must be 16-byte aligned or FXSAVE/FXRSTOR #GP");

/*
 * Process-lifecycle arch hooks (arch/amd64/usermode.c).  arch_darwin_fork
 * builds the child task -- address-space copy, fd-table clone, a thread
 * that iretqs to the parent's saved user rip/rsp with %rax = 0 -- and
 * returns the child's pid, or a negative SYS_E_*.  arch_darwin_execve
 * replaces the calling task's user address space with `image` (argv is a
 * kernel-owned flat block) and rewrites the frame's user rip/rsp so the
 * sysret lands in the fresh image; returns 0, and does not return at all
 * if setup fails past the point of no return (the task exits with wait4
 * status 127).
 */
long	arch_darwin_fork(struct syscall_frame *f);
long	arch_darwin_execve(const unsigned char *image,
	    unsigned long image_size, int argc, char **argv,
	    struct syscall_frame *f);

#endif /* !_SYS_DARWIN_H_ */
