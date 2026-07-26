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
 * A task owns a per-task address space (t_pmap + t_map), a port name
 * space, and the set of threads scheduled within it.  A thread always
 * belongs to exactly one task, but a task may have zero, one, or many
 * threads.
 *
 * The split between task and thread is deliberate: unlike UNIX where
 * fork() conflates resource container and execution unit, here you
 * can talk about "spawn a worker thread inside this task" without
 * any chicken-and-egg.  kthreads, userland threads, and threads-of-
 * another-task all use the same struct thread; the task pointer is
 * what distinguishes them.
 */

struct mach_msg_header;
struct pmap;
struct port;
struct port_space;
struct thread;
struct vm_map;

/*
 * Syscall ABI personality.  Selects which syscall table a ring-3 thread in
 * this task dispatches through, mirroring how XNU gates a process on the
 * platform recorded in its Mach-O.  STYLE9 -- the default for every task
 * and for every ELF program -- uses the native SYS_* numbers in syscall.h.
 * DARWIN is set by macho_load when the image carries an LC_BUILD_VERSION
 * naming PLATFORM_MACOS; it routes every syscall through darwin_dispatch
 * (kern/darwin.c), which decodes the Apple class bits in %rax (Mach traps,
 * BSD calls) and honours the carry-flag error convention.  The native path
 * is left completely untouched: a STYLE9 task never enters darwin_dispatch.
 */
#define	TASK_PERSONALITY_STYLE9	0
#define	TASK_PERSONALITY_DARWIN	1

/*
 * Per-task open-file table for the Darwin file syscalls (kern/darwin.c).
 * Slots are typed.  A FILE slot holds a whole file slurped from the
 * read-only FS into of_buf at open() time (read/lseek move of_off over it;
 * close frees of_buf).  A CONSOLE slot is an explicit std-stream binding
 * (write -> tty, read -> EOF).  A PIPE_R/PIPE_W slot is one end of a
 * kernel pipe; the struct darwin_pipe is shared (reference-counted per
 * end) with every other fd cloned from it by dup2/fork.
 *
 * fds 0..2 whose slot is FREE keep the historical implicit std-stream
 * behavior (stdin EOF, stdout/stderr -> console).  dup2 can overwrite
 * them with a typed slot -- a pipe end standing in for stdout is exactly
 * what a shell's redirection plumbing does.
 */
#define	DARWIN_NOFILE	16

/*
 * How long a path this kernel will carry.  Darwin's PATH_MAX is 1024; this is
 * the 256 every path buffer in kern/darwin.c already used, given a name so
 * that the working directory below and the buffers it gets pasted into cannot
 * drift apart.  It lives here rather than in darwin.h because struct task is
 * what it sizes, exactly like DARWIN_NOFILE above.
 */
#define	DARWIN_PATH_MAX	256

/*
 * Darwin signal sizing.  Signals are numbered 1..31 (Darwin's NSIG is 32);
 * the per-task disposition table t_sig_handler[] is indexed by that number.
 * A slot holds DARWIN_SIG_DFL (take the default action), DARWIN_SIG_IGN
 * (discard on delivery), or a ring-3 handler VA (on-stack delivery, phase 2).
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
 * A DIRECTORY, opened.  Its own type rather than a flag on a file, because
 * what a descriptor onto one is FOR is different: it names a place, and every
 * call that takes it -- openat, fdopendir, fchdir, fchmod -- wants the name,
 * not the bytes.  read(2) on one answers EISDIR, which is what a modern Unix
 * does and what makes the distinction visible from ring 3.
 */
#define	DARWIN_OF_DIR		5

struct darwin_pipe;

/*
 * An open file.  A disk-backed one is a HANDLE plus a cursor: the bytes stay
 * on the volume and are read as they are asked for.  The synthetic /bin
 * entries have no volume to be read from -- they are built into the kernel
 * image -- so those keep a buffer, which is what every open used to do.
 */
struct darwin_ofile {
	struct darwin_pipe	*of_pipe;	/* PIPE_*: shared object   */
	struct fs_handle	 of_handle;	/* FILE: the file, resolved */
	uint8_t			*of_buf;	/* FILE: image, if no handle */
	char			*of_path;	/* FILE: what it was named  */
	uint32_t		 of_size;	/* FILE: valid bytes       */
	uint32_t		 of_off;	/* FILE: read cursor       */
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
	 * Per-type task-level exception ports.  user_fault_die maps the
	 * x86 trap vector down to an EXC_TYPE_* index and posts
	 * MACH_EXC_FAULT onto t_exc_ports[type] (NULL slots silently
	 * drop).  Each slot is independent: the kernel holds one SEND
	 * ref per non-NULL slot; refs balance on slot replace, on
	 * mask-clear, and on task destruction.  Set via
	 * SYS_TASK_SET_EXC_PORTS (mask form) or SYS_TASK_SET_EXC_PORT
	 * (sets every slot from a single port, back-compat with A v1).
	 * All under t_lock.
	 */
	struct port		*t_exc_ports[EXC_TYPE_COUNT];

	/*
	 * Behavior flags for the exception dispatch path.  Bitwise OR
	 * of EXC_FLAG_* (today: only EXC_FLAG_RESUMABLE).  When
	 * RESUMABLE is set, user_fault_die uses the reply protocol --
	 * post the exception with an implicit reply port and park the
	 * thread until a mach_exception_reply lands (or the timeout
	 * expires, which falls back to KILL).  Clear by default; opted
	 * into by passing flag bits in the high half of the mask
	 * argument to SYS_TASK_SET_EXC_PORTS.  (t) under t_lock.
	 */
	uint32_t		 t_exc_flags;

	/*
	 * Async-termination flag.  Set once by task_request_terminate
	 * via __atomic_store(RELEASE); never cleared.  Five detection
	 * points, every place a thread can transition from "still
	 * running this task" to "about to commit to more work":
	 *	1. syscall_dispatch ENTRY -- next syscall after a ring-3
	 *	   compute window with t_killed already set.
	 *	2. thread_block_release pre-park -- caught between wake-
	 *	   eligibility (BLOCKED) and actually-blocked, otherwise
	 *	   we'd park and never observe the wake.
	 *	3. thread_block_release post-wake -- woken by the kill,
	 *	   retire before returning into the caller's recv/RPC loop.
	 *	4. intr_dispatch tail when iretq'ing to ring 3 -- catches
	 *	   pure compute loops that never syscall.  PIT timer (or
	 *	   any IRQ) brings the thread into the kernel; we retire
	 *	   instead of resuming user code.
	 *	5. syscall_dispatch EXIT -- catches "this syscall ITSELF
	 *	   caused the kill" (SYS_TASK_KILL on self), so the user
	 *	   never observes a sysretq on the kill-issuing call.
	 *
	 * kernel_task is never killable -- task_request_terminate
	 * refuses task_id matching kernel_task and every check site
	 * fast-paths past kernel-task threads before the atomic load.
	 *
	 * (a) atomic, set-once semantics; readers use ACQUIRE load.
	 */
	volatile bool		 t_killed;

	/*
	 * Darwin signal state (kern/darwin.c).  t_sig_pending is the set of
	 * posted-but-undelivered signals (bit N == Darwin signal N), OR'd in
	 * atomically by darwin_signal_post from any context (a child's exit
	 * posting SIGCHLD to its parent, a broken-pipe write posting SIGPIPE
	 * to itself).  t_sig_mask is the blocked set installed via sigprocmask;
	 * t_sig_handler[N] records the disposition (DARWIN_SIG_DFL /
	 * DARWIN_SIG_IGN / a ring-3 handler VA).  The deliverable set
	 * (pending & ~mask) is applied at the return-to-user points by
	 * darwin_signal_deliver.  Zeroed at task_create: no pending, nothing
	 * blocked, every signal at its default.
	 *
	 * (a) t_sig_pending atomic (RELEASE post / ACQUIRE load); t_sig_mask
	 * and t_sig_handler are mutated only by the owning task, on its own
	 * thread, via sigaction/sigprocmask -- single-writer, no lock.
	 */
	uint64_t		 t_sig_handler[DARWIN_NSIG];
	uint64_t		 t_sig_tramp;	/* libSystem _sigtramp VA     */
	volatile uint32_t	 t_sig_pending;
	uint32_t		 t_sig_mask;

	/*
	 * Next base VA at which the Darwin dynamic linker's "map image by
	 * path" backchannel (kern/darwin.c, S4) maps a dylib into this task.
	 * A bump pointer: 0 until the first map, then DARWIN_DYLIB_BASE and
	 * upward by each mapped image's page-rounded span.  Touched only by
	 * this task's own (single-threaded) dyld, so it carries no lock.
	 */
	uint64_t		 t_darwin_dylib_next;

	/*
	 * Darwin parentage: the t_id of the Darwin task that fork()ed
	 * this one, or 0 when no Darwin parent exists (the native spawn
	 * rig).  Read by getppid(2) and by wait4(2), which matches both
	 * zombies and live children against the caller's id.  Set once
	 * at fork, before the child's first instruction; never changed.
	 */
	uint64_t		 t_darwin_ppid;

	/*
	 * Open files for the Darwin file syscalls (see struct darwin_ofile).
	 * Zeroed at task_create (all slots DARWIN_OF_FREE); anything still
	 * open is released on task teardown via darwin_files_teardown.
	 */
	struct darwin_ofile	 t_darwin_files[DARWIN_NOFILE];

	/*
	 * The working directory: absolute, normalised, no trailing slash
	 * except at the root, always valid to paste a relative path onto.
	 *
	 * It lives here because that is what a working directory IS -- a
	 * property of the process, inherited across fork and surviving
	 * execve.  Until this existed the kernel had no such notion, and
	 * libSystem covered for it: chdir(2) returned success without doing
	 * anything, getcwd(3) always answered "/", and relative paths were
	 * rewritten on the userspace side of the syscall by a function whose
	 * own comment called the working directory a fiction.  A program that
	 * changed directory and then opened a relative path got the wrong
	 * file, quietly and correctly-looking.
	 *
	 * "/" at task_create; copied from the parent at fork.  Only this
	 * task's own threads read or write it, so it carries no lock -- the
	 * same reasoning t_darwin_dylib_next records.
	 */
	char			 t_darwin_cwd[DARWIN_PATH_MAX];

	/*
	 * The permission bits a create must NOT grant.
	 *
	 * A umask is not a security measure here -- there is one user and it is
	 * root -- it is the reason `mkdir foo` produces 0755 when every program
	 * on earth asks for 0777 and expects the system to take the rest away.
	 * Without one, the mode a caller passed had to be ignored outright, and
	 * that was the honest edge documented on mkdir(2) until this.
	 *
	 * 022 at task_create and copied at fork, like the working directory
	 * above, and read and written only by this task's own threads.
	 */
	uint16_t		 t_darwin_umask;
};

extern struct task		*kernel_task;

void			 task_subsystem_init(void);
struct task		*task_create(const char *name);
void			 task_ref(struct task *);
void			 task_deref(struct task *);

/*
 * Linked-list helpers used by thread.c on the t_threads list, which
 * is threaded through struct thread.  Defined here so both files
 * agree on the lock acquisition order (t_lock then th_lock).
 */
void			 task_attach_thread(struct task *, struct thread *);
void			 task_detach_thread(struct task *, struct thread *);

void			 task_print(struct task *);
void			 task_list_print(void);

/*
 * Snapshot live-task pointers into the caller's `out` array (max
 * entries).  No refs are bumped -- the snapshot is best-effort and
 * intended for short-lived, kernel-side use (e.g. the `tasks` service
 * dispatcher building its reply payload).  Returns the number of
 * pointers written.
 */
size_t			 task_snapshot(struct task **out, size_t max);

/*
 * Best-effort liveness probe by task id.  Returns true if a task with
 * that id is in the global task_list under tasks_lock at the moment of
 * the call.  No ref is bumped -- the result is stale the instant
 * tasks_lock is released, so callers must treat this as a hint, not a
 * handle.  Powers the SYS_TASK_ALIVE syscall: the shell yield-spins
 * until a spawned child drops off the live list.
 */
bool			 task_is_alive(uint64_t id);

/*
 * Count the live tasks whose t_darwin_ppid is `ppid` -- restricted to the
 * single child `pid` when pid != 0.  Walks the task list under tasks_lock;
 * the count is stale the instant the lock drops, so callers treat it as a
 * hint.  Powers the Darwin wait4(2): "do I still have a child that could
 * produce a zombie?" -- the loop re-checks the zombie table first, so a
 * child that exits between samples is never missed (its zombie record is
 * written before its task leaves the live list).
 */
int			 task_count_darwin_children(uint64_t ppid,
			    uint64_t pid);

/*
 * Request asynchronous termination of the task identified by `task_id`.
 * Sets t_killed via __atomic_store(RELEASE) and then best-effort wakes
 * every thread on the target's t_threads list.  Any thread already in
 * BLOCKED state observes the wake, returns through thread_block_release's
 * post-wake check, and retires; any thread about to park observes
 * t_killed under sched_lock in the pre-park check and retires
 * immediately; threads running in ring 3 observe t_killed at the top
 * of their next syscall_dispatch.
 *
 * Lock-order: tasks_lock -> t_lock -> sched_lock -> th_lock.  The
 * tasks_lock dance owns task identification + the t_killed store; the
 * t_lock walk + thread_wake fan-out introduces the new t_lock ->
 * sched_lock edge but no existing path takes sched_lock then t_lock,
 * so no cycle.
 *
 * Pure ring-3 compute loops that never syscall will NOT terminate
 * under v1 -- the IRQ-return-to-user check is deferred for v2.
 *
 * task_id == kernel_task->t_id is silently refused; missing ids are
 * silently no-ops (the caller probably races a natural exit).
 */
void			 task_request_terminate(uint64_t task_id);

/*
 * Non-blocking accessor for the t_killed flag.  ACQUIRE load.  NULL
 * task pointer returns false.  Used at the syscall-dispatch entry +
 * thread_block_release detection sites.
 */
bool			 task_kill_pending(struct task *t);

/*
 * task_self_port_for: take one SEND ref on `task_id`'s task-self port
 * and return the port object (caller drops the ref via port_deref with
 * MACH_PORT_RIGHT_SEND).  NULL if the id is unknown, names kernel_task,
 * or has no self port.  Lets launchd hand a freshly spawned child's
 * task-self SEND to launchctl for DEAD_NAME arming.
 */
struct port		*task_self_port_for(uint64_t task_id);

/*
 * task_lookup_ref: find the live task with `id` under tasks_lock and
 * return it with one extra ref taken (caller drops via task_deref), or
 * NULL if no live task has that id.  Unlike task_is_alive -- a bare
 * liveness hint that is stale the instant tasks_lock drops -- the ref
 * guarantees the returned task cannot be chain-removed + freed until
 * the caller derefs, so the pointer is safe to dereference.
 *
 * Powers the task-self port dispatch: the port stores only the
 * immutable task id (never a raw pointer that would dangle once the
 * task is reaped while an external SEND keeps the port alive), so a
 * stale port resolves to NULL here and the caller fails safe.
 *
 * Lock-order: tasks_lock -> t_lock (via task_ref), the same edge
 * task_request_terminate already uses.
 */
struct task		*task_lookup_ref(uint64_t id);

/*
 * Synchronous dispatcher invoked by mach_msg_send when the destination
 * port is tagged PORT_SPECIAL_TASK_SELF.  Reads `req->msgh_id` to pick
 * an op (see TASK_OP_* in port.h), assembles a reply message, and
 * sends it back to `req->msgh_local` using the COPY_SEND right the
 * caller's space holds on that name.  Returns MACH_MSG_OK on success.
 */
int			 task_self_dispatch(struct task *target,
			    const struct mach_msg_header *req,
			    struct port_space *from);

/*
 * Install `port` into every slot of `t->t_exc_ports` named by
 * `types_mask` (a bitwise OR of EXC_MASK_* values).  Existing
 * occupants of those slots get port_deref'd; the kernel takes one
 * fresh SEND ref per slot in `types_mask` on the new port (so a
 * single port covering multiple types holds N refs, balanced at
 * teardown).  Passing port=NULL clears the named slots.
 *
 * Returns MACH_MSG_OK or MACH_E_INVAL if `types_mask` has bits
 * outside EXC_MASK_ALL.  Empty mask is a no-op success.
 */
int			 task_set_exception_ports(struct task *t,
			    uint32_t types_mask, struct port *port);

#endif /* !_SYS_TASK_H_ */
