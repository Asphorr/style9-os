/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DARWIN_H_
#define	_SYS_DARWIN_H_

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
#define	DARWIN_SYS_getpid	20
#define	DARWIN_SYS_kill		37
#define	DARWIN_SYS_getppid	39
#define	DARWIN_SYS_dup		41
#define	DARWIN_SYS_pipe		42
#define	DARWIN_SYS_sigaction	46
#define	DARWIN_SYS_sigprocmask	48
#define	DARWIN_SYS_execve	59
#define	DARWIN_SYS_setitimer	83
#define	DARWIN_SYS_dup2		90
#define	DARWIN_SYS_lseek	199

/* wait4 option bits (Darwin <sys/wait.h>). */
#define	DARWIN_WNOHANG	1

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
 * (kern/fs_fat.h) out to *out; returns 0 in %rax (carry clear), or carry set on
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
#define	DARWIN_EINVAL	22
#define	DARWIN_EMFILE	24
#define	DARWIN_ESPIPE	29
#define	DARWIN_EPIPE	32
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
