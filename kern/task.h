/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_TASK_H_
#define	_SYS_TASK_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs.h"
#include "port.h"
#include "spinlock.h"

/*
 * Mach-style task: the resource container.
 *
 * A task owns an address space (t_pmap + t_map), a port name space, and
 * its threads.  A thread belongs to exactly one task; a task may have any
 * number of threads.  Kernel and user threads share struct thread; the
 * task pointer is what distinguishes them.
 */

struct mach_msg_header;
struct pmap;
struct port;
struct port_space;
struct thread;
struct vm_map;

/*
 * Syscall ABI personality: which syscall table a ring-3 thread in this
 * task dispatches through, as XNU gates a process on its Mach-O's
 * platform.  STYLE9, the default and every ELF program's, uses the native
 * SYS_* numbers in syscall.h.  DARWIN is set by macho_load for an image
 * whose LC_BUILD_VERSION names PLATFORM_MACOS; its syscalls go through
 * darwin_dispatch (kern/darwin.c), which decodes Apple's class bits in
 * %rax and the carry-flag error convention.  A STYLE9 task never enters
 * darwin_dispatch.
 */
#define	TASK_PERSONALITY_STYLE9	0
#define	TASK_PERSONALITY_DARWIN	1

/*
 * Per-task open-file table for the Darwin file syscalls (kern/darwin.c).
 * Slots are typed (DARWIN_OF_*; see struct darwin_ofile).  A CONSOLE slot
 * is an explicit std-stream binding: read from the console, write to the
 * tty.  A PIPE_R/PIPE_W slot is one end of a kernel pipe, whose struct
 * darwin_pipe is shared, refcounted per end, with every fd dup2/fork
 * cloned from it.
 *
 * fds 0..2 with a FREE slot keep their implicit std-stream meaning (the
 * console) until dup2 puts a typed slot there, as a shell's redirection
 * does.
 */
#define	DARWIN_NOFILE	16

/*
 * Longest path this kernel carries (Darwin's PATH_MAX is 1024).  Named so
 * the working directory below and the path buffers in kern/darwin.c
 * cannot drift apart; here rather than in darwin.h because it sizes
 * struct task.
 */
#define	DARWIN_PATH_MAX	256

/*
 * Darwin signals are numbered 1..31 (Darwin's NSIG is 32) and index
 * t_sig_handler[].  A slot holds DARWIN_SIG_DFL (default action),
 * DARWIN_SIG_IGN (discard on delivery), or a ring-3 handler VA.
 */
#define	DARWIN_NSIG	32
#define	DARWIN_SIG_DFL	0
#define	DARWIN_SIG_IGN	1

#define	DARWIN_OF_FREE		0
#define	DARWIN_OF_FILE		1
#define	DARWIN_OF_CONSOLE	2
#define	DARWIN_OF_PIPE_R	3
#define	DARWIN_OF_PIPE_W	4
/*
 * An open directory.  Its own type, not a flag on a file: it names a place,
 * and every call that takes it (openat, fdopendir, fchdir, fchmod) wants
 * the name, not the bytes.  read(2) on one answers EISDIR.
 */
#define	DARWIN_OF_DIR		5
/*
 * /dev/null: reads as empty, swallows writes.  Neither a file (nothing on
 * any volume) nor the console (nothing reaches the screen).
 */
#define	DARWIN_OF_NULL		6

struct darwin_foff;
struct darwin_pipe;

/*
 * An open file.  A disk-backed one is a handle plus a cursor; the bytes
 * stay on the volume and are read on demand.  The synthetic /bin entries
 * are built into the kernel image, so they keep a buffer instead.  The
 * cursor is the open file description's, shared with every copy dup(2)
 * and fork(2) make (kern/darwin.c).
 */
struct darwin_ofile {
	struct darwin_pipe	*of_pipe;	/* PIPE_*: shared object   */
	struct darwin_foff	*of_foff;	/* FILE: the shared cursor */
	struct fs_handle	 of_handle;	/* FILE: the file, resolved */
	uint8_t			*of_buf;	/* FILE: image, if no handle */
	char			*of_path;	/* FILE: what it was named  */
	uint64_t		 of_size;	/* FILE: length last seen  */
	uint32_t		 of_flags;	/* FILE: DARWIN_O_* it was opened with */
	uint8_t			 of_type;	/* DARWIN_OF_*             */
};

struct task {
	struct spinlock		 t_lock;
	uint64_t		 t_id;		/* (c) printable id        */
	const char		*t_name;	/* (c) for ps-style listing */
	uint32_t		 t_personality;	/* (c) TASK_PERSONALITY_*  */
	struct port_space	*t_port_space;	/* (c) name table          */
	struct port		*t_self_port;	/* (c) kernel-RECEIVE port  */
	struct vm_map		*t_map;		/* (c) per-task vm map      */
	struct pmap		*t_pmap;	/* (c) per-task page-table  */
	struct thread		*t_threads;	/* (t) head of thread list */
	uint32_t		 t_nthreads;	/* (t) count                */
	uint32_t		 t_refs;	/* (t) lifetime refs        */

	/*
	 * CPU time in TSC cycles: of the threads that have left t_threads,
	 * and of the children wait4 has reaped, their own reaped children's
	 * included (getrusage's RUSAGE_CHILDREN).  (t)
	 */
	uint64_t		 t_utime;
	uint64_t		 t_stime;
	uint64_t		 t_cutime;
	uint64_t		 t_cstime;
	/*
	 * Per-type task-level exception ports.  user_fault_die maps the trap
	 * vector to an EXC_TYPE_* index and posts MACH_EXC_FAULT to
	 * t_exc_ports[type]; a NULL slot drops it.  One SEND ref per non-NULL
	 * slot, balanced on replace, on clear and at task destruction.  Set
	 * by SYS_TASK_SET_EXC_PORTS (mask form) or SYS_TASK_SET_EXC_PORT
	 * (every slot from one port).  (t)
	 */
	struct port		*t_exc_ports[EXC_TYPE_COUNT];

	/*
	 * EXC_FLAG_* for exception dispatch (only EXC_FLAG_RESUMABLE so far).
	 * With RESUMABLE, user_fault_die posts the exception with an implicit
	 * reply port and parks the thread until a mach_exception_reply lands,
	 * or kills it on timeout.  Clear by default; set through the high
	 * half of SYS_TASK_SET_EXC_PORTS's mask.  (t)
	 */
	uint32_t		 t_exc_flags;

	/*
	 * Async-termination flag: set once by task_request_terminate (RELEASE
	 * store), never cleared, read with ACQUIRE (task_kill_pending).
	 * Checked wherever a thread could commit to more work:
	 *	1. syscall_dispatch entry.
	 *	2. thread_block_release, before parking -- a thread about to
	 *	   commit to BLOCKED would otherwise miss the kill's wake.
	 *	3. thread_block_release, after waking -- retire rather than
	 *	   return into the caller's loop.
	 *	4. intr_dispatch's return to ring 3 -- catches compute loops
	 *	   that never syscall.
	 *	5. syscall_dispatch exit -- a syscall that killed its own task
	 *	   never returns to ring 3.
	 * Checks 2 and 3 decline while the thread holds a mutex.
	 *
	 * kernel_task is never killable: task_request_terminate refuses it,
	 * and every check site skips kernel_task threads.  (a)
	 */
	volatile bool		 t_killed;

	/*
	 * Darwin signal state (kern/darwin.c).  t_sig_pending holds posted,
	 * undelivered signals (bit N == signal N), OR'd in atomically by
	 * darwin_signal_post from any context (SIGCHLD from a child's exit,
	 * SIGPIPE from a broken-pipe write).  t_sig_mask is the blocked set
	 * (sigprocmask); t_sig_handler[N] the disposition.  pending & ~mask
	 * is delivered at the return-to-user points by darwin_signal_deliver.
	 * At task_create nothing is pending or blocked and every signal is at
	 * its default.
	 *
	 * (a) t_sig_pending (RELEASE post / ACQUIRE load).  t_sig_mask and
	 * t_sig_handler are written only by the task's own thread, via
	 * sigaction/sigprocmask: single writer, no lock.
	 */
	uint64_t		 t_sig_handler[DARWIN_NSIG];
	uint64_t		 t_sig_tramp;	/* libSystem _sigtramp VA     */
	volatile uint32_t	 t_sig_pending;
	uint32_t		 t_sig_mask;

	/*
	 * The mask pselect(2) swapped in for one wait, and the one to put
	 * back.  If the wait ends with a signal, the temporary mask must stay
	 * until that signal is delivered -- restoring it first would block
	 * the very signal the caller unblocked, leaving it pending while
	 * every later pselect returns EINTR on entry.  So the wait leaves
	 * this pair armed; delivery puts t_sig_mask_saved into the signal
	 * frame for sigreturn to restore (as Linux's TIF_RESTORE_SIGMASK), or
	 * restores it directly if nothing was delivered.
	 */
	uint32_t		 t_sig_mask_saved;
	bool			 t_sig_mask_restore;

	/*
	 * Bump pointer for where dyld's map-image-by-path backchannel
	 * (kern/darwin.c) maps the next dylib: 0 until the first map, then
	 * from DARWIN_DYLIB_BASE up by each image's page-rounded span.
	 * Touched only by this task's own single-threaded dyld: no lock.
	 */
	uint64_t		 t_darwin_dylib_next;

	/*
	 * t_id of the Darwin task that fork()ed this one, or 0 if none (a
	 * native spawn).  Read by getppid(2) and wait4(2).  Set at fork
	 * before the child's first instruction; never changed.
	 */
	uint64_t		 t_darwin_ppid;

	/*
	 * Open files for the Darwin file syscalls.  All DARWIN_OF_FREE at
	 * task_create; whatever is still open is released by
	 * darwin_files_teardown.
	 */
	struct darwin_ofile	 t_darwin_files[DARWIN_NOFILE];

	/*
	 * The working directory: absolute, normalised, no trailing slash
	 * except at the root, always valid to paste a relative path onto.
	 * A property of the process -- inherited across fork, surviving
	 * execve.  "/" at task_create, copied at fork.  Read and written only
	 * by this task's own threads: no lock.
	 */
	char			 t_darwin_cwd[DARWIN_PATH_MAX];

	/*
	 * The umask: permission bits a create must not grant (why `mkdir foo'
	 * gives 0755 when the caller asks for 0777).  022 at task_create,
	 * copied at fork; only this task's own threads touch it.
	 */
	uint16_t		 t_darwin_umask;
};

extern struct task		*kernel_task;

void			 task_subsystem_init(void);
struct task		*task_create(const char *name);
void			 task_ref(struct task *);
void			 task_deref(struct task *);

/*
 * Link a thread into or out of t->t_threads (under t_lock), taking or
 * dropping a task ref for it.  Detach drops what may be the task's last
 * ref, which tears the task down.
 */
void			 task_attach_thread(struct task *, struct thread *);
void			 task_detach_thread(struct task *, struct thread *);

/*
 * The CPU time a task has used, in TSC cycles: its departed threads' and
 * its live ones' as last charged.  A caller asking about its own task
 * calls sched_cpu_charge first to count up to now.
 */
void			 task_cpu_times(struct task *, uint64_t *user,
			    uint64_t *sys);

void			 task_print(struct task *);
void			 task_list_print(void);

/*
 * Copy up to `max' live-task pointers into `out'; returns how many.
 *
 * task_snapshot takes no refs: a task may be torn down the moment the
 * call returns.  Only for ddb, which must not take t_lock and runs with
 * the rest of the machine stopped or suspect anyway.
 *
 * task_snapshot_ref takes one ref per task (a task already at zero refs
 * is left out), so its pointers, t_port_space and t_map stay valid until
 * task_snapshot_release drops them.  The release may be a task's last
 * ref and tear it down, so no spinlock may be held across it.
 */
size_t			 task_snapshot(struct task **out, size_t max);
size_t			 task_snapshot_ref(struct task **out, size_t max);
void			 task_snapshot_release(struct task **tasks, size_t n);

/*
 * Whether a task with this id is live at the moment of the call.  No ref
 * is taken; the answer is stale once tasks_lock drops, so it is a hint,
 * not a handle.  Backs SYS_TASK_ALIVE, which the shell polls until a
 * spawned child is gone.
 */
bool			 task_is_alive(uint64_t id);

/*
 * Count live tasks whose t_darwin_ppid is `ppid', restricted to the child
 * `pid' when pid != 0.  Stale once tasks_lock drops -- a hint.  Backs
 * Darwin wait4(2)'s "could a child still produce a zombie?"; the loop
 * checks the zombie table first, and a child's zombie record is written
 * before it leaves the live list, so none is missed.
 */
int			 task_count_darwin_children(uint64_t ppid,
			    uint64_t pid);

/*
 * Request asynchronous termination of the task `task_id': set t_killed
 * (RELEASE), then thread_wake every thread in t_threads.  A BLOCKED
 * thread wakes and retires at thread_block_release's post-wake check; one
 * about to park sees t_killed under sched_lock and retires; one in ring 3
 * retires at its next syscall or interrupt return (see t_killed).
 *
 * Lock order: tasks_lock -> t_lock -> sched_lock -> th_lock.  Nothing
 * takes t_lock while holding sched_lock, so no cycle.
 *
 * kernel_task is refused and an unknown id is ignored (the caller is
 * probably racing a natural exit), both silently.
 */
void			 task_request_terminate(uint64_t task_id);

/*
 * Whether `t' has been killed (ACQUIRE load of t_killed); false for NULL.
 * Used at every detection site listed under t_killed.
 */
bool			 task_kill_pending(struct task *t);

/*
 * task_self_port_for: take one SEND ref on the task-self port of task
 * `task_id' and return the port (the caller drops it with port_deref(...,
 * MACH_PORT_RIGHT_SEND)).  NULL if the id is unknown, names kernel_task,
 * or has no self port.  Lets launchd hand a new child's task-self SEND to
 * launchctl for DEAD_NAME arming.
 */
struct port		*task_self_port_for(uint64_t task_id);

/*
 * task_lookup_ref: the live task with `id', with one ref taken (drop it
 * with task_deref), or NULL.  Unlike task_is_alive, the ref keeps the
 * task from being freed, so the pointer is safe to use.
 *
 * Backs task-self port dispatch: the port stores only the task id, never
 * a pointer that would dangle once the task is reaped while a SEND keeps
 * the port alive, so a stale port resolves to NULL and fails safe.
 *
 * Lock order: tasks_lock -> t_lock (via task_ref), as in
 * task_request_terminate.
 */
struct task		*task_lookup_ref(uint64_t id);

/*
 * Synchronous dispatcher called by mach_msg_send for a destination port
 * tagged PORT_SPECIAL_TASK_SELF.  Picks the op from req->msgh_id
 * (TASK_OP_* in port.h) and sends the reply to req->msgh_local.  Returns
 * MACH_MSG_OK on success.
 */
int			 task_self_dispatch(struct task *target,
			    const struct mach_msg_header *req,
			    struct port_space *from);

/*
 * Install `port' into every slot of t->t_exc_ports named by `types_mask'
 * (EXC_MASK_* bits), taking one SEND ref per slot -- a port covering N
 * types holds N refs -- and dropping the previous occupants' refs.
 * port == NULL clears the slots.
 *
 * Returns MACH_MSG_OK (an empty mask is a successful no-op), or
 * MACH_E_INVAL for a NULL task or bits outside EXC_MASK_ALL.
 */
int			 task_set_exception_ports(struct task *t,
			    uint32_t types_mask, struct port *port);

#endif /* !_SYS_TASK_H_ */
