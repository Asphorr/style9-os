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
 * Darwin (XNU) syscall personality: the syscalls a genuine Apple-ABI binary
 * issues, mapped onto style9 primitives (kern/macho.c loads the image).  A
 * task whose image declared PLATFORM_MACOS gets TASK_PERSONALITY_DARWIN,
 * and syscall_dispatch sends its calls to darwin_dispatch instead of the
 * native table.
 *
 * Apple encodes a class in the high byte of the number in %rax and the
 * call number in the low 24 bits.  The argument registers (rdi, rsi, rdx,
 * r10, r8, r9) match struct syscall_frame's order, so only the number
 * decode and the return convention differ.  A subset is implemented;
 * mach_msg routes onto the kernel's own message path.
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
 * style9-private syscall class, not one of Apple's, well clear of 0..5.
 * Only our clean-room dyld (user/dyld.c) and libSystem issue it, e.g. to
 * map a dependency by path in place of open()+mmap() or a shared cache.
 */
#define	DARWIN_SYSCALL_CLASS_STYLE9	0x2A

/*
 * BSD (class 2) call numbers we translate, from Darwin's
 * bsd/kern/syscalls.master (not Linux's).  The result is in %rax with carry
 * clear, or a positive errno with carry set; see darwin.c.
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
#define	DARWIN_SYS_fsync	95
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
 * getcwd(3) is not a syscall on Darwin; libc calls __getcwd(2), which fills
 * a caller-supplied buffer with the absolute path or fails with ERANGE.
 */
#define	DARWIN_SYS___getcwd	326

/*
 * More class-2 numbers, checked like those above.  pselect is not here:
 * XNU's native number was not verified from a source, so it rides the
 * style9-private class instead (DARWIN_S9_pselect), called only by our
 * libSystem.
 */
#define	DARWIN_SYS_access	33
#define	DARWIN_SYS_select	93
#define	DARWIN_SYS_mkfifo	132
#define	DARWIN_SYS_ftruncate	201
#define	DARWIN_SYS_poll		230
#define	DARWIN_SYS_pread	153
#define	DARWIN_SYS_pwrite	154
#define	DARWIN_SYS_statfs64	345	/* statfs$INODE64  */
#define	DARWIN_SYS_fstatfs64	346	/* fstatfs$INODE64 */
#define	DARWIN_SYS_getrusage	117
#define	DARWIN_SYS_utimes	138
#define	DARWIN_SYS_futimes	139

/*
 * struct statfs as a 64-bit-inode program sees it (<sys/mount.h>), 2168
 * bytes.  MNT_RDONLY is the one f_flags bit reported.
 */
#define	DARWIN_MFSTYPENAMELEN	16
#define	DARWIN_MAXPATHLEN	1024
#define	DARWIN_MNT_RDONLY	0x00000001u

struct darwin_statfs64 {
	uint32_t	f_bsize;
	int32_t		f_iosize;
	uint64_t	f_blocks;
	uint64_t	f_bfree;
	uint64_t	f_bavail;
	uint64_t	f_files;
	uint64_t	f_ffree;
	int32_t		f_fsid[2];
	uint32_t	f_owner;
	uint32_t	f_type;
	uint32_t	f_flags;
	uint32_t	f_fssubtype;
	char		f_fstypename[DARWIN_MFSTYPENAMELEN];
	char		f_mntonname[DARWIN_MAXPATHLEN];
	char		f_mntfromname[DARWIN_MAXPATHLEN];
	uint32_t	f_flags_ext;
	uint32_t	f_reserved[7];
};
_Static_assert(sizeof(struct darwin_statfs64) == 2168,
    "struct statfs (64-bit inode) is 2168 bytes");

/* access(2)'s mode word, <unistd.h> spelling. */
#define	DARWIN_F_OK		0
#define	DARWIN_X_OK		1
#define	DARWIN_W_OK		2
#define	DARWIN_R_OK		4

/*
 * select(2)'s fd_set is FD_SETSIZE bits in 32-bit words; poll(2)'s pollfd
 * is {int fd; short events; short revents}, with <sys/poll.h>'s bits.
 * Only POLLIN, POLLOUT and the unrequested ERR, HUP and NVAL are ever
 * set; PRI is accepted but never reported.
 */
#define	DARWIN_FD_SETSIZE	1024
#define	DARWIN_NFDBITS		32

#define	DARWIN_POLLIN		0x0001
#define	DARWIN_POLLPRI		0x0002
#define	DARWIN_POLLOUT		0x0004
#define	DARWIN_POLLERR		0x0008
#define	DARWIN_POLLHUP		0x0010
#define	DARWIN_POLLNVAL		0x0020

struct darwin_pollfd {
	int32_t		pfd_fd;
	int16_t		pfd_events;
	int16_t		pfd_revents;
};
_Static_assert(sizeof(struct darwin_pollfd) == 8, "pollfd is 8 bytes");

/*
 * open(2) flags, Darwin <sys/fcntl.h>.  The access mode is the low two bits
 * (0 read, 1 write, 2 both), so "open for writing" is a comparison under
 * O_ACCMODE, not a bit test.
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
 * mmap(2) flags, Darwin <sys/mman.h>.  The PROT bits happen to match
 * VM_PROT_*, but darwin.c translates them explicitly.
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
 * fcntl(2) commands (Darwin <sys/fcntl.h>).  F_DUPFD duplicates (a shell
 * saves fds at 10+ with it) and the three lock commands keep POSIX record
 * locks; the flag commands answer 0, as the fd table has no flag bits (no
 * close-on-exec).
 */
#define	DARWIN_F_DUPFD		0
#define	DARWIN_F_GETFD		1
#define	DARWIN_F_SETFD		2
#define	DARWIN_F_GETFL		3
#define	DARWIN_F_SETFL		4
#define	DARWIN_F_GETLK		7
#define	DARWIN_F_SETLK		8
#define	DARWIN_F_SETLKW		9
#define	DARWIN_F_DUPFD_CLOEXEC	67	/* dash's savefd uses this one */

#define	DARWIN_F_RDLCK		1
#define	DARWIN_F_UNLCK		2
#define	DARWIN_F_WRLCK		3

/* struct flock, what the three lock commands point at. */
struct darwin_flock {
	int64_t		l_start;
	int64_t		l_len;		/* 0: to the end, however far */
	int32_t		l_pid;
	int16_t		l_type;		/* DARWIN_F_RDLCK/UNLCK/WRLCK */
	int16_t		l_whence;	/* SEEK_SET/CUR/END */
};
_Static_assert(sizeof(struct darwin_flock) == 24, "struct flock is 24 bytes");

/*
 * Mach traps (class 1): positive indices into xnu's mach_trap_table.  They
 * return a port name or kern_return_t in %rax, no carry convention.
 */
#define	DARWIN_MACH_mach_reply_port	26
#define	DARWIN_MACH_thread_self_trap	27
#define	DARWIN_MACH_task_self_trap	28
#define	DARWIN_MACH_host_self_trap	29	/* mach_host_self()            */
#define	DARWIN_MACH_mach_msg_trap	31	/* classic combined mach_msg() */
#define	DARWIN_MACH_thread_get_special_reply_port 50
#define	DARWIN_MACH_mk_timer_create_trap 91

/* What xnu's kern_invalid answers for a trap it does not have. */
#define	DARWIN_KERN_INVALID_ARGUMENT	4

/*
 * style9-private calls (class DARWIN_SYSCALL_CLASS_STYLE9), issued only by
 * our dyld and libSystem.  map_image(const char *path) maps the embedded
 * dylib registered under `path` into the caller's task and returns the
 * base it landed at; carry and an errno on failure.
 */
#define	DARWIN_S9_dyld_map_image	1

/*
 * fs_stat(const char *path, struct fs_statbuf *out): size, type, inode and
 * times of a file, in fs/fs.h's neutral struct; libSystem's stat$INODE64
 * builds Apple's struct stat from it, keeping that layout out of the
 * kernel.  Returns 0, or carry set (ENOENT when absent).
 *
 * fs_readdir(const char *path, uint32_t index, struct fs_dirent *out):
 * fill *out with the index-th entry of the directory at `path`.  Returns 1
 * when an entry was written, 0 at end-of-directory, carry set on error.
 * libSystem's opendir/readdir drive it (stateless: re-resolved per index).
 *
 * uname, fs_fstat and fs_fdpath are described with their structs below and
 * in darwin.c.
 */
#define	DARWIN_S9_fs_stat		2
#define	DARWIN_S9_fs_readdir		3
#define	DARWIN_S9_uname			4
#define	DARWIN_S9_fs_fstat		5
#define	DARWIN_S9_fs_fdpath		6

/*
 * pselect(int nfds, uint32_t *in, uint32_t *ou, uint32_t *ex,
 *     const struct darwin_timespec *ts, const uint32_t *sigmask):
 * select(2) with a signal mask swapped in for the wait.  In this class
 * rather than class 2: see DARWIN_SYS_select.
 */
#define	DARWIN_S9_pselect		7

/*
 * fs_fstat(int fd, struct darwin_fdstat *out): fs_stat's fd sibling,
 * behind libSystem's fstat64.  The kernel classifies what the fd holds
 * (file, directory, console or /dev/null, pipe end) and libSystem builds
 * Apple's struct stat.  Returns 0, or carry set with EBADF/EFAULT.
 */
#define	DARWIN_FDSTAT_REG	0
#define	DARWIN_FDSTAT_CHR	1
#define	DARWIN_FDSTAT_FIFO	2
#define	DARWIN_FDSTAT_DIR	3

struct darwin_fdstat {
	/*
	 * Tells descriptors on the same file from different ones; cat
	 * compares it to refuse copying a file onto itself.
	 */
	uint64_t	fds_ino;
	uint64_t	fds_size;	/* byte length (regular files)   */
	uint8_t		fds_kind;	/* DARWIN_FDSTAT_*               */
};

/*
 * struct termios, Darwin x86_64 layout, read and written in an Apple
 * binary's own storage.  The ioctl number encodes the argument size:
 * _IOR('t', 19, struct termios) is 0x40000000 | (size << 16) | ('t' << 8)
 * | 19, and TIOCGETA is 0x40487413, so the size is 0x48 (asserted below).
 * tcflag_t and speed_t are 64-bit unsigned long and cc_t one byte, so c_cc
 * sits at 32 followed by four bytes of alignment padding.
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

/* c_lflag */
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
 * The speed a non-serial terminal reports.  BSD stores the baud rate
 * itself, not an index.
 */
#define	DARWIN_B38400	38400UL

/*
 * struct timeval, x86_64 Darwin layout (written into Apple binaries'
 * storage): 64-bit tv_sec, then tv_usec as a 32-bit suseconds_t at offset
 * 8, then four bytes of explicit padding.  The time is UTC;
 * gettimeofday(2)'s timezone argument is ignored.
 */
struct darwin_timeval {
	int64_t		tv_sec;
	int32_t		tv_usec;
	int32_t		tv_pad;
};

/* struct timespec, Darwin x86-64: two longs.  pselect's deadline. */
struct darwin_timespec {
	int64_t		ts_sec;
	int64_t		ts_nsec;
};
_Static_assert(sizeof(struct darwin_timespec) == 16,
    "Darwin timespec is 16 bytes");

_Static_assert(sizeof(struct darwin_timeval) == 16,
    "struct timeval is 16 bytes on x86_64 Darwin");

/*
 * struct rusage (<sys/resource.h>), what getrusage(2) and wait4(2) fill.
 * Only the two times are kept; the fourteen counters read zero.
 */
#define	DARWIN_RUSAGE_SELF	0
#define	DARWIN_RUSAGE_CHILDREN	(-1)

struct darwin_rusage {
	struct darwin_timeval	ru_utime;
	struct darwin_timeval	ru_stime;
	int64_t			ru_counters[14];	/* ru_maxrss on */
};
_Static_assert(sizeof(struct darwin_rusage) == 144,
    "struct rusage is 144 bytes on x86_64 Darwin");

/*
 * uname(struct darwin_uname *out): the Darwin identity the kernel claims,
 * behind libSystem's uname(3) (guname).  As with fs_stat, a neutral struct
 * that libSystem reshapes into Apple's struct utsname (256-byte fields),
 * bounding each copy.  Returns 0, or carry set if *out faults.
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
 * Base VA of the first dylib in a Darwin task; further dylibs bump upward
 * from it (t_darwin_dylib_next).  Inside the user window [0x40000000,
 * 0x80000000), above the main image (0x50000000) and dyld (0x60000000);
 * the stack grows down from DARWIN_STACK_TOP (0x50000000).
 */
#define	DARWIN_DYLIB_BASE	0x70000000ULL

/*
 * BSD errno values (Darwin <sys/errno.h>) we produce.  style9 has no errno
 * of its own, so darwin.c maps internal failures onto these.
 */
#define	DARWIN_EPERM	1
#define	DARWIN_ENOENT	2
#define	DARWIN_ESRCH	3
#define	DARWIN_EINTR	4
#define	DARWIN_EIO	5
#define	DARWIN_E2BIG	7
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
#define	DARWIN_EFBIG	27
#define	DARWIN_ENOSPC	28
#define	DARWIN_ESPIPE	29
#define	DARWIN_EROFS	30
#define	DARWIN_EPIPE	32
#define	DARWIN_ERANGE	34
#define	DARWIN_EAGAIN	35
#define	DARWIN_ENOLCK	77
#define	DARWIN_ENAMETOOLONG	63
#define	DARWIN_ENOTDIR	20
#define	DARWIN_ENOTEMPTY	66
#define	DARWIN_ESTALE	70	/* a handle onto a checkpoint since let go */
#define	DARWIN_ENOSYS	78

/*
 * mach_msg option flags and the mach_msg_return_t values we produce
 * (Darwin <mach/message.h>).  Returned in %rax with carry clear, even a
 * receive timeout: mach_msg_trap is a class-1 trap.
 */
#define	DARWIN_MACH_SEND_MSG		0x00000001u
#define	DARWIN_MACH_RCV_MSG		0x00000002u
#define	DARWIN_MACH_SEND_TIMEOUT	0x00000010u
#define	DARWIN_MACH_RCV_TIMEOUT		0x00000100u

#define	DARWIN_MACH_MSG_SUCCESS		0x00000000
#define	DARWIN_MACH_SEND_INVALID_DATA	0x10000002
#define	DARWIN_MACH_SEND_INVALID_DEST	0x10000003
#define	DARWIN_MACH_SEND_TIMED_OUT	0x10000004
#define	DARWIN_MACH_SEND_NO_BUFFER	0x1000000d
#define	DARWIN_MACH_SEND_INVALID_TYPE	0x1000000f
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
 * releases every slot in t's table (files closed, pipe ends dropped) and
 * the console claim.  The task's last thread calls it in thread_exit,
 * since closing can block; task_deref calls it again as an idempotent
 * fallback.  darwin_files_fork_copy clones the parent's table into the
 * child at fork: files take another fs hold (a built-in's buffer is
 * copied), pipe ends bump the matching count.  Returns 0 or -1; the caller
 * derefs the half-built child, and teardown releases what was cloned.
 */
void	darwin_files_teardown(struct task *t);
int	darwin_files_fork_copy(struct task *parent, struct task *child);

/*
 * The Darwin console (kern/darwin.c): a terminal whose line discipline,
 * behind darwin_cons_input, does echo, erase, Ctrl-C to a signal and
 * Ctrl-D to end-of-file per the termios flags, and feeds read(2) on a
 * console fd.
 *
 * darwin_cons_sink is the keyboard driver's producer: if a Darwin task
 * holds the console it takes the key, else returns false and the key goes
 * to the native shell's Mach input port; nothing else arbitrates the
 * keyboard.  darwin_cons_feed loads a canned session for SYS_CONS_FEED,
 * released a line at a time through the same discipline, then
 * end-of-input.
 *
 * darwin_cons_release drops the claim (and restores default settings) when
 * its owner dies; file teardown calls it.
 */
bool	darwin_cons_input(char c);
bool	darwin_cons_sink(char c);
void	darwin_cons_feed(const char *buf, size_t n);
void	darwin_cons_release(struct task *t);
void	darwin_cons_stats(void);
void	darwin_wait_stats(void);

/*
 * Wake a parent parked in wait4(2) for the pid `ppid`.  Called where a
 * zombie is recorded and from task teardown, when the child is gone.  A
 * no-op for ppid 0 (no Darwin parent).
 */
void	darwin_child_news(unsigned long long ppid);

/*
 * Zombie bookkeeping (kern/darwin.c).  Records {pid, ppid, wait4-format
 * status, CPU times} for a dying Darwin task `t` so the parent's wait4 can
 * reap it; a ppid of 0 records nothing.  Also drops the dying task's own
 * unreaped zombie children, which no one is left to wait for.  Called on
 * every Darwin death: exit(2), signal termination, kill(2), a bad
 * sigreturn frame, and the execve point-of-no-return failure
 * (arch/amd64/usermode.c).
 */
void	darwin_zombie_record(struct task *t, int status);

/*
 * Signal subsystem (kern/darwin.c).  darwin_signal_post ORs `signo` into
 * `t`'s pending set from any context and wakes t's sleepers; the signal
 * takes effect at t's next return to user.  darwin_signal_deliver is a
 * return-to-user hook without a frame: it discards ignored and
 * default-ignore signals, leaves a caught one pending for on-stack
 * delivery, and for a default-terminate one records the wait4 status and
 * retires the thread (no return).  Both no-op on a NULL task; deliver is
 * called only for TASK_PERSONALITY_DARWIN tasks.
 */
void	darwin_signal_post(struct task *t, int signo);

/*
 * Has `t` a pending, unblocked signal that will act (caught or fatal)?
 * Every interruptible wait in the personality asks this, alongside
 * task_kill_pending.  Ignored and default-ignore signals do not count: they
 * are consumed on the way to ring 3, and SIGCHLD would otherwise make wait4
 * return EINTR the moment its child died.  A true answer delivers nothing;
 * the caller returns EINTR and delivery happens on the way out.
 *
 * Only for the current thread's own task: t_sig_mask and t_sig_handler
 * have a single writer and are read without a lock.
 */
bool	darwin_signal_pending(struct task *t);
void	darwin_signal_deliver(struct task *t);

/*
 * darwin_signal_deliver_syscall: the syscall-exit variant.  With the
 * syscall_frame and `rv` (the value about to be returned) it delivers a
 * caught signal on-stack: saves the context in a darwin_sigframe on the
 * user stack and reshapes the frame so the sysret enters the task's
 * _sigtramp with (signo, siginfo, ucontext, handler).  Terminate and
 * ignore as darwin_signal_deliver.  The trampoline calls the handler, then
 * DARWIN_SYS_sigreturn restores the sigframe.
 */
struct syscall_frame;
void	darwin_signal_deliver_syscall(struct syscall_frame *f, long rv);

/*
 * darwin_signal_deliver_trap: the IRQ- and fault-return variant, for a
 * thread not at a syscall boundary (say, a ring-3 loop a timer IRQ
 * interrupted).  Nothing is dead there, so the frame holds the whole
 * machine state (all 15 GPRs and the FPU file) and the resume is an IRETQ.
 * Terminate and ignore as darwin_signal_deliver.
 */
struct trapframe;
void	darwin_signal_deliver_trap(struct trapframe *tf);

/*
 * On-stack signal context, written at delivery and read back at sigreturn.
 * Private to the clean-room ABI (our _sigtramp is the only sigreturn
 * caller), so the layout is ours.  Two flavours, by the magic at offset 0:
 *
 *	SGFR1	taken at a syscall boundary, where the ABI makes argument
 *		and scratch registers dead: rip/rsp/rflags/rax suffice and
 *		the resume uses the ordinary SYSRET exit.
 *	SGFR2	taken asynchronously: every GPR and the FPU file are saved
 *		and sigreturn leaves via IRETQ.
 *
 * Both record the mask in force at delivery; the handled signal is blocked
 * during its handler (POSIX) and the mask restored at sigreturn.
 */
#define	DARWIN_SIGFRAME_MAGIC		0x5347465231ULL	/* "SGFR1" */
#define	DARWIN_SIGFRAME_MAGIC_FULL	0x5347465232ULL	/* "SGFR2" */

/*
 * RFLAGS bits a sigreturn may restore: CF PF AF ZF SF DF OF.  The saved
 * value comes from the user stack, so masking keeps a forged frame from
 * granting IOPL, NT or a single-step trap.  IF and reserved bit 1 are
 * ORed back in.
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
 * builds the child (address-space copy, fd-table clone, a thread that
 * iretqs to the parent's user rip/rsp with %rax = 0) and returns its pid,
 * or a negative SYS_E_*.  arch_darwin_execve replaces the caller's address
 * space with `image` (argv and envp kernel-owned) and rewrites the frame's
 * user rip/rsp to enter it; returns 0, or does not return at all on a
 * failure past the point of no return (wait4 status 127).
 */
long	arch_darwin_fork(struct syscall_frame *f);
long	arch_darwin_execve(const unsigned char *image,
	    unsigned long image_size, int argc, char **argv,
	    int envc, char **envp, struct syscall_frame *f);

#endif /* !_SYS_DARWIN_H_ */
