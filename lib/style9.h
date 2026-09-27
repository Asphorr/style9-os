/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _STYLE9_H_
#define	_STYLE9_H_

/*
 * libstyle9 -- the runtime every native ring-3 program links against:
 * bare types, syscall stubs, a few libc-shaped helpers (string, memory,
 * bump allocator, printf) and the Mach IPC wrappers.  All of it is
 * declared here; the implementations are split across lib/style9_*.c.
 *
 * crt0.S calls main(argc, argv) and then exit() with its return value;
 * `int main(void)' works as well.
 */

/* ---- bare types ----------------------------------------------------- */

typedef unsigned long		size_t;
typedef long			ssize_t;
typedef unsigned long		uintptr_t;
typedef long			intptr_t;

typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef unsigned long long	uint64_t;

typedef signed char		int8_t;
typedef short			int16_t;
typedef int			int32_t;
typedef long long		int64_t;

#define	NULL			((void *)0)

/* ---- syscall ABI (mirrors kern/syscall.h) -------------------------- */

#define	SYS_PRINT		0
#define	SYS_EXIT		1
#define	SYS_YIELD		2
#define	SYS_PORT_ALLOC		3
#define	SYS_PORT_DEALLOC	4
#define	SYS_MSG_SEND		5
#define	SYS_MSG_RECV		6
#define	SYS_MSG_RECV_TIMED	7
#define	SYS_MSG_RPC		8
#define	SYS_SPAWN		9
#define	SYS_TASK_ALIVE		10
#define	SYS_VM_ALLOCATE		11
#define	SYS_VM_DEALLOCATE	12
#define	SYS_PORT_MOD_REFS	13
#define	SYS_PORT_SET_ALLOC	14
#define	SYS_PORT_SET_INSERT	15
#define	SYS_PORT_SET_REMOVE	16
#define	SYS_PORT_REQUEST_NOTIFICATION 17
#define	SYS_SPAWN_WITH_PORT	18
#define	SYS_TASK_SET_EXC_PORT	19
#define	SYS_PORT_SET_EXTRACT	20
#define	SYS_TASK_SET_EXC_PORTS	21
#define	SYS_THREAD_SET_EXC_PORTS 22
#define	SYS_TASK_GET_PORT_SNAPSHOT 23
#define	SYS_TASK_GET_VM_REGIONS	24
#define	SYS_TASK_KILL		25
#define	SYS_SPAWN_RETURNS_TASKPORT 26
#define	SYS_SPAWN_ARGS		27
#define	SYS_CONS_FEED		28
#define	SYS_MSG_SEND_TIMED	29

#define	SYS_E_NOSYS		(-1)
#define	SYS_E_FAULT		(-2)
#define	SYS_E_INVAL		(-3)
#define	SYS_E_NOMEM		(-4)

/* Raw `syscall' wrappers; return the kernel's %rax verbatim. */
long	syscall0(long nr);
long	syscall1(long nr, long a0);
long	syscall2(long nr, long a0, long a1);
long	syscall3(long nr, long a0, long a1, long a2);
long	syscall4(long nr, long a0, long a1, long a2, long a3);

/* ---- process ------------------------------------------------------- */

void	exit(int code) __attribute__((noreturn));
/*
 * Give up the CPU.  Returns 1 if another thread took it, 0 if there was
 * none -- which with several CPUs is common and means only that this
 * CPU's runqueue is empty.
 */
long	yield(void);

/*
 * One turn of a poll loop: about one timer tick of real time.  Every
 * bounded "wait for the other task" loop must use this, not yield(): with
 * several CPUs a yield returns at once and a budget of them is spent in
 * microseconds.  See style9_sys.c.
 */
long	poll_turn(void);

/*
 * Spawn the named program from the kernel's progreg.  Returns the new
 * task's id or a negative SYS_E_*.  Wait for it with task_alive().
 */
long	spawn(const char *name);

/* spawn_with_port: declared with the Mach ABI below. */

/*
 * task_alive: 1 if a task with this id is still running, 0 otherwise.
 * Poll it with poll_turn() to wait for a spawned child.
 */
int	task_alive(uint64_t task_id);

/* task_kill: prototype lives below mach_port_name_t's typedef. */

/* ---- I/O ----------------------------------------------------------- */

ssize_t	write(const char *buf, size_t len);
void	putchar(char c);
void	puts(const char *s);
int	printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- string -------------------------------------------------------- */

size_t	strlen(const char *s);
int	strcmp(const char *a, const char *b);
int	strncmp(const char *a, const char *b, size_t n);
char	*strcpy(char *dst, const char *src);
char	*strncpy(char *dst, const char *src, size_t n);

/* ---- memory -------------------------------------------------------- */

void	*memcpy(void *dst, const void *src, size_t n);
void	*memset(void *dst, int c, size_t n);
int	memcmp(const void *a, const void *b, size_t n);

/*
 * Bump allocator over a .bss arena (4 KiB unless STYLE9_HEAP_BYTES is
 * set at compile time).  free() is a no-op.
 */
void	*malloc(size_t n);
void	free(void *p);

/* ---- vm ------------------------------------------------------------ */

#define	VM_PROT_READ	0x01u
#define	VM_PROT_WRITE	0x02u
#define	VM_PROT_EXEC	0x04u

/*
 * vm_allocate: a zero-filled anonymous range of `bytes' rounded up to
 * 4 KiB, `prot' any OR of VM_PROT_*.  Returns the VA, or NULL when out
 * of address space or frames.
 */
void	*vm_allocate(size_t bytes, uint32_t prot);

/*
 * vm_deallocate: release anonymous memory from vm_allocate.  `va' must be
 * page-aligned and `bytes' is rounded up to 4 KiB; any part of an
 * allocation, or a run across several, may be released.  A range with an
 * unmapped page or non-anonymous memory in it is refused whole.  Returns
 * 0, or a negative SYS_E_*.
 */
int	 vm_deallocate(void *va, size_t bytes);

/* ---- Mach ABI (mirrors mach/port.h) -------------------------------- */

typedef uint32_t		mach_port_name_t;

#define	MACH_PORT_NULL			((mach_port_name_t)0)
#define	MACH_PORT_DEAD			((mach_port_name_t)0xFFFFFFFFu)

#define	MACH_PORT_RIGHT_RECEIVE		0x01u
#define	MACH_PORT_RIGHT_SEND		0x02u
#define	MACH_PORT_RIGHT_SEND_ONCE	0x04u
#define	MACH_PORT_RIGHT_PORT_SET	0x08u

#define	MACH_PORT_TASK_SELF		((mach_port_name_t)1)
#define	MACH_PORT_BOOTSTRAP		((mach_port_name_t)2)
#define	MACH_PORT_PARENT		((mach_port_name_t)3)

/*
 * Notification types for mach_port_request_notification; the kernel's
 * mach_notify_header carries the type as msgh_id.
 */
#define	MACH_NOTIFY_FIRST		64
#define	MACH_NOTIFY_NO_SENDERS		(MACH_NOTIFY_FIRST + 6)
#define	MACH_NOTIFY_DEAD_NAME		(MACH_NOTIFY_FIRST + 8)
#define	MACH_EXC_FAULT			100

/*
 * Exception types; the kernel maps an x86 trap vector to one of these and
 * sends to that port slot.  task_set_exception_ports takes EXC_MASK_*.
 */
#define	EXC_TYPE_BAD_ACCESS		0u
#define	EXC_TYPE_BAD_INSTRUCTION	1u
#define	EXC_TYPE_ARITHMETIC		2u
#define	EXC_TYPE_BREAKPOINT		3u
#define	EXC_TYPE_COUNT			4u

#define	EXC_MASK_BAD_ACCESS		(1u << EXC_TYPE_BAD_ACCESS)
#define	EXC_MASK_BAD_INSTRUCTION	(1u << EXC_TYPE_BAD_INSTRUCTION)
#define	EXC_MASK_ARITHMETIC		(1u << EXC_TYPE_ARITHMETIC)
#define	EXC_MASK_BREAKPOINT		(1u << EXC_TYPE_BREAKPOINT)
#define	EXC_MASK_ALL			((1u << EXC_TYPE_COUNT) - 1u)

/* Exception-port behavior flags (high bits of the SET_EXC_PORTS arg). */
#define	EXC_FLAG_RESUMABLE		0x00010000u

/* Exception reply opcode + verdicts.  Mirrors mach/port.h. */
#define	MACH_EXC_REPLY			101
#define	EXC_VERDICT_KILL		0u
#define	EXC_VERDICT_RESUME		1u
#define	EXC_VERDICT_STEP		2u

#define	MACH_MSG_TYPE_MOVE_RECEIVE	16
#define	MACH_MSG_TYPE_MOVE_SEND		17
#define	MACH_MSG_TYPE_MOVE_SEND_ONCE	18
#define	MACH_MSG_TYPE_COPY_SEND		19
#define	MACH_MSG_TYPE_MAKE_SEND		20
#define	MACH_MSG_TYPE_MAKE_SEND_ONCE	21

#define	MACH_MSG_PORT_DESCRIPTOR	0
#define	MACH_MSG_OOL_DESCRIPTOR		1
#define	MACH_MSG_VIRTUAL_COPY		0
#define	MACH_MSG_PHYSICAL_COPY		1
#define	MACH_MSG_OOL_MAX_BYTES		(1u << 20)	/* 1 MiB             */
#define	MACH_MSGH_BITS_COMPLEX		0x80000000u
#define	MACH_MSGH_BITS(remote, local)	\
	(((uint32_t)(remote) & 0xFFu) | (((uint32_t)(local) & 0xFFu) << 8))
#define	MACH_MSGH_BITS_USED_MASK	\
	((uint32_t)MACH_MSGH_BITS_COMPLEX | (uint32_t)0xFFFFu)

#define	MACH_MSG_OK			0
#define	MACH_E_INVAL			1
#define	MACH_E_NAME			2
#define	MACH_E_RIGHT			3
#define	MACH_E_DEAD			4
#define	MACH_E_NOSPACE			5
#define	MACH_E_NOMSG			6
#define	MACH_E_TOOSMALL			7
#define	MACH_E_NOMEM			8
#define	MACH_E_TIMEOUT			9
#define	MACH_E_INTR			10

#define	MACH_TIMEOUT_NONE		((uint64_t)0)
#define	MACH_TIMEOUT_FOREVER		((uint64_t)~0ull)

/*
 * The WIRE FORMAT structs below mirror the kernel's declarations
 * (mach/port.h, mach/bootstrap.h, mach/services.h, mach/host.h,
 * dev/dev_proto.h, vm/vm.h) and must stay byte-identical: a drifted
 * offset misparses silently.  Change the kernel side first, then this,
 * and check the _Static_assert sizes on both.
 */

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_msg_header {
	uint32_t		msgh_bits;
	uint32_t		msgh_size;
	mach_port_name_t	msgh_remote;
	mach_port_name_t	msgh_local;
	uint32_t		msgh_voucher;
	uint32_t		msgh_id;
};

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_msg_body {
	uint32_t		msgh_descriptor_count;
};

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_msg_port_descriptor {
	uint8_t			type;		/* MACH_MSG_PORT_DESCRIPTOR */
	uint8_t			disposition;
	uint8_t			pad1;
	uint8_t			pad2;
	mach_port_name_t	name;
};

/*
 * Packed: 8-byte alignment would open a 4-byte gap after the body, which
 * the kernel's descriptor walker reads as a bogus port descriptor (see
 * mach/port.h).  `deallocate = 1' asks the kernel to release the source
 * range after the copy; best-effort.
 */
/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_msg_ool_descriptor {
	uint8_t			type;		/* == MACH_MSG_OOL_DESCRIPTOR */
	uint8_t			copy;		/* MACH_MSG_PHYSICAL_COPY     */
	uint8_t			deallocate;	/* 1: release src after copy  */
	uint8_t			pad;
	uint32_t		size;		/* bytes to ferry             */
	uint64_t		address;	/* sender VA / receiver VA    */
} __attribute__((packed));

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_notify_header {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_NOTIFY_*    */
	uint32_t		nh_msgid;	/* user tag from request call */
	uint32_t		nh_pad;
};

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_exception_header {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_EXC_FAULT   */
	uint32_t		eh_trapno;
	uint32_t		eh_err;
	uint64_t		eh_rip;
	uint64_t		eh_rsp;
	uint64_t		eh_rflags;
	uint64_t		eh_cr2;
	uint64_t		eh_task_id;
};

/* WIRE FORMAT.  Mirrors mach/port.h.  A RESUMABLE watcher's verdict. */
struct mach_exception_reply {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_EXC_REPLY   */
	uint32_t		er_verdict;
	uint32_t		er_rip_advance;
};

/* Port-snapshot tags, as the kernel's PORT_SPECIAL_*, for lsmp. */
#define	PORT_SPECIAL_NONE		0
#define	PORT_SPECIAL_TASK_SELF		1
#define	PORT_SPECIAL_BOOTSTRAP		2
#define	PORT_SPECIAL_SERVICE		3

#define	PORT_SNAPSHOT_KIND_PORT		1u
#define	PORT_SNAPSHOT_KIND_SET		2u

#define	PORT_SNAPSHOT_FLAG_DEAD		0x01u

#define	MACH_PORT_SNAPSHOT_MAX		64

/* vm_map region snapshot, read by vmmap from SYS_TASK_GET_VM_REGIONS. */
#define	MACH_VM_REGION_MAX		64

#define	VME_F_ANON			0x01u	/* anonymous (pmm) backing  */
#define	VME_F_COW			0x02u	/* shared; copy on write    */

/* WIRE FORMAT.  Mirrors vm/vm.h. */
struct mach_vm_region_entry {
	uint64_t	mvr_start;
	uint64_t	mvr_end;
	uint64_t	mvr_offset;
	uint8_t		mvr_prot;
	uint8_t		mvr_flags;
	uint8_t		mvr_pad[6];
};

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_port_snapshot_entry {
	mach_port_name_t	mpse_name;
	uint8_t			mpse_kind;
	uint8_t			mpse_rights;
	uint8_t			mpse_special;
	uint8_t			mpse_flags;
	uint64_t		mpse_object_id;
	uint32_t		mpse_qlen;
	uint32_t		mpse_qmax;
	uint32_t		mpse_refs;
	uint32_t		mpse_send_count;
	uint32_t		mpse_send_once_count;
	uint32_t		mpse_member_count;
};

/* Op codes the kernel exposes on its well-known ports. */
#define	TASK_OP_GET_INFO		1
#define	TASK_OP_VM_ALLOCATE		2
#define	TASK_OP_VM_DEALLOCATE		3
#define	TASK_OP_GET_SPECIAL_PORT	4
#define	BOOTSTRAP_OP_LOOKUP		1
#define	BOOTSTRAP_OP_REGISTER		2
#define	BOOTSTRAP_OP_DEREGISTER		3
#define	BOOTSTRAP_OP_CHECK_IN		4
#define	BOOTSTRAP_REPLY_NOT_FOUND	0xFFFFFFFFu
#define	BOOTSTRAP_NAME_MAX		32

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct task_info_reply {
	uint64_t	tir_task_id;
	uint32_t	tir_nthreads;
	uint32_t	tir_pad;
	char		tir_name[32];
};

/* WIRE FORMAT.  Mirrors mach/port.h.  Task-port vm_allocate/deallocate. */
struct task_vm_allocate_request {
	uint64_t	tva_size;
	uint32_t	tva_prot;
	uint32_t	tva_pad;
};

struct task_vm_allocate_reply {
	uint64_t	tvar_address;
	int32_t		tvar_status;
	uint32_t	tvar_pad;
};

struct task_vm_deallocate_request {
	uint64_t	tvd_address;
	uint64_t	tvd_size;
};

struct task_vm_deallocate_reply {
	int32_t		tvdr_status;
	uint32_t	tvdr_pad;
};

/* TASK_OP_GET_SPECIAL_PORT which-index (mirrors mach/port.h). */
#define	TASK_SPECIAL_HOST		2
#define	TASK_SPECIAL_BOOTSTRAP		4

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct task_special_port_request {
	uint32_t	tsp_which;
	uint32_t	tsp_pad;
};

/* WIRE FORMAT.  Mirrors mach/bootstrap.h. */
struct bootstrap_lookup_request {
	char	blr_name[BOOTSTRAP_NAME_MAX];
};

/* WIRE FORMAT.  Mirrors mach/bootstrap.h.  (De)register reply body. */
struct bootstrap_status_reply {
	int32_t		bsr_status;
	uint32_t	bsr_pad;
};

/* ---- Mach wrappers ------------------------------------------------- */

mach_port_name_t mach_port_allocate(uint8_t rights);
int		mach_port_deallocate(mach_port_name_t name);
int		mach_msg_send(const struct mach_msg_header *msg);
int		mach_msg_send_timed(const struct mach_msg_header *msg,
		    uint64_t timeout_ms);
int		mach_msg_recv(mach_port_name_t name,
		    struct mach_msg_header *buf, size_t buf_size);
int		mach_msg_recv_timed(mach_port_name_t name,
		    struct mach_msg_header *buf, size_t buf_size,
		    uint64_t timeout_ms);
int		mach_msg_rpc(struct mach_msg_header *req,
		    struct mach_msg_header *reply, size_t reply_size,
		    uint64_t timeout_ms);

/*
 * mach_port_mod_refs: drop one right kind from a name that carries
 * several (say RECEIVE and SEND), keeping the rest.  Returns MACH_MSG_OK
 * or a MACH_E_*.
 */
int		mach_port_mod_refs(mach_port_name_t name, uint8_t right);

/*
 * Port sets: insert ports whose RECEIVE right you hold, then
 * mach_msg_recv on the set name to take the next message from any
 * member.
 */
mach_port_name_t mach_port_set_allocate(void);
int		mach_port_set_insert(mach_port_name_t set_name,
		    mach_port_name_t port_name);
int		mach_port_set_remove(mach_port_name_t set_name,
		    mach_port_name_t port_name);

/*
 * mach_port_set_extract: the name of the set `port_name' belongs to, or
 * MACH_PORT_NULL if none (or the name is invalid or lacks RECEIVE).
 */
mach_port_name_t mach_port_set_extract(mach_port_name_t port_name);

/*
 * mach_port_request_notification: have the kernel post a MACH_NOTIFY_<TYPE>
 * message to `notify_port' when `name' reaches that event.  Caller holds:
 *	NO_SENDERS -- RECEIVE on `name', SEND on `notify_port'
 *	DEAD_NAME  -- SEND on `name',    SEND on `notify_port'
 * `notify_msgid' comes back as nh_msgid.  One-shot: re-arm after each
 * firing.
 */
int		mach_port_request_notification(mach_port_name_t name,
		    uint32_t notify_type, mach_port_name_t notify_port,
		    uint32_t notify_msgid);

/*
 * spawn_with_port: spawn(), also giving the child a SEND right to
 * `source_name' (which the caller must hold SEND on) at MACH_PORT_PARENT,
 * installed before the child runs.  The child tells by sending there:
 * MACH_E_RIGHT means the slot is empty.  Returns task_id or a negative
 * SYS_E_*.
 */
long	spawn_with_port(const char *name, mach_port_name_t source_name);

/*
 * task_kill: terminate the task whose task-self port `target_port' names
 * (SEND held).  task_kill(MACH_PORT_TASK_SELF) kills the caller; another
 * task needs its task-self port handed over first.  Returns MACH_MSG_OK
 * when accepted, MACH_E_RIGHT without SEND, MACH_E_INVAL if the port is
 * not a task-self port.  Asynchronous: it returns before the target has
 * retired.
 */
int	task_kill(mach_port_name_t target_port);

/*
 * spawn_returns_taskport: spawn(), also writing to *out_taskport a SEND
 * right on the child's task-self port -- what task_kill() needs.
 * Returns task_id, or a negative SYS_E_* with *out_taskport untouched.
 */
long	spawn_returns_taskport(const char *name, mach_port_name_t *out_taskport);

/*
 * spawn_args: spawn_returns_taskport with a command line.  argv[0..argc-1]
 * are copied into the child, which gets them as main(argc, argv);
 * argv[0] is conventionally the program name.  `out_taskport' is
 * required.  Returns as spawn_returns_taskport; argc 0 behaves like it.
 * sh starts every job this way.
 */
long	spawn_args(const char *name, int argc, char *const argv[],
	    mach_port_name_t *out_taskport);

/*
 * cons_feed: load `len' bytes as a scripted console session for an
 * interactive Darwin shell, read back through its real read(2) path.  A
 * new feed replaces the old; running out of script is end-of-input, so
 * the shell exits.  Returns the bytes accepted (the kernel caps them) or
 * a negative SYS_E_*.
 */
long	cons_feed(const char *buf, unsigned long len);

/*
 * task_set_exception_port: task_set_exception_ports(EXC_MASK_ALL,
 * notify_port), kept for older callers.  Returns MACH_MSG_OK or a
 * MACH_E_*.
 */
int	task_set_exception_port(mach_port_name_t notify_port);

/*
 * task_set_exception_ports: set the calling task's exception port for
 * every EXC_TYPE in `types_mask' to `notify_port' (one SEND ref per slot),
 * or clear them with MACH_PORT_NULL.  The previous refs are released.
 * Returns MACH_MSG_OK or a MACH_E_*.
 */
int	task_set_exception_ports(uint32_t types_mask,
	    mach_port_name_t notify_port);

/*
 * thread_set_exception_ports: the same for the calling thread.  A
 * thread-level port takes precedence over the task's for the same type.
 */
int	thread_set_exception_ports(uint32_t types_mask,
	    mach_port_name_t notify_port);

/*
 * task_get_port_snapshot: one entry per populated slot of the task's port
 * space, at most MACH_PORT_SNAPSHOT_MAX.  Only task_id 0 (the caller) is
 * accepted; anything else is SYS_E_INVAL.  Returns the count written or a
 * negative SYS_E_*.  Used by lsmp.
 */
long	task_get_port_snapshot(uint64_t task_id,
	    struct mach_port_snapshot_entry *out, size_t max_entries);

/*
 * task_get_vm_regions: likewise, one entry per vm_map entry, at most
 * MACH_VM_REGION_MAX, task_id 0 only.  Used by vmmap.
 */
long	task_get_vm_regions(uint64_t task_id,
	    struct mach_vm_region_entry *out, size_t max_entries);

/*
 * Look `service' up with bootstrap.  Returns a SEND name in the caller's
 * space, which the caller deallocates, or MACH_PORT_NULL if it is not
 * registered or anything failed.
 */
mach_port_name_t bootstrap_lookup(const char *service);
mach_port_name_t bootstrap_check_in(const char *service);

/*
 * Publish `port' (a SEND right; the caller keeps its own) as `service'.
 * Later lookups by any task get a fresh SEND to the same port.  Returns
 * MACH_MSG_OK, MACH_E_RIGHT if `port' is not a SEND right, MACH_E_NOSPACE
 * if the registry is full, MACH_E_INVAL for a bad or duplicate name, or
 * the RPC's MACH_E_*.
 */
int		 bootstrap_register_service(const char *service,
		    mach_port_name_t port);

/*
 * Remove `service' and drop the SEND right behind it.  Returns
 * MACH_MSG_OK, or MACH_E_INVAL if it was not registered.  Unchecked: any
 * task can deregister any name, the kernel's own services included.
 */
int		 bootstrap_deregister_service(const char *service);

/* ---- dev/NAME generic-driver protocol (mirrors dev/dev_proto.h) --- */

#define	DEV_OP_INFO		1
#define	DEV_OP_OPEN_STREAM	2
#define	DEV_OP_WRITE		3

#define	DEV_KIND_NONE		0
#define	DEV_KIND_STREAM_RX	1
#define	DEV_KIND_STREAM_TX	2
#define	DEV_KIND_CHAR		3
#define	DEV_KIND_BLOCK		4

#define	DEV_F_READABLE		0x01u
#define	DEV_F_WRITABLE		0x02u
#define	DEV_F_STREAM		0x04u

#define	DEV_NAME_MAX		16
#define	DEV_WRITE_MAX		256

/* WIRE FORMAT.  Mirrors dev/dev_proto.h. */
struct dev_info_reply {
	char		dir_name[DEV_NAME_MAX];
	uint32_t	dir_kind;
	uint32_t	dir_flags;
};

/* WIRE FORMAT.  Mirrors dev/dev_proto.h. */
struct dev_write_request {
	uint32_t	dwr_len;
	uint32_t	dwr_pad;
	uint8_t		dwr_data[DEV_WRITE_MAX];
};

/* WIRE FORMAT.  Mirrors dev/dev_proto.h. */
struct dev_write_reply {
	int32_t		dwr_rv;
	uint32_t	dwr_written;
};

/*
 * dev_open_stream: DEV_OP_OPEN_STREAM to "dev/<short_name>".  Returns a
 * SEND right to the driver's stream port, from which the caller
 * mach_msg_recv()s bytes and which it deallocates when done, or
 * MACH_PORT_NULL on any failure.
 */
mach_port_name_t dev_open_stream(const char *short_name);

/*
 * dev_info: the driver's kind and capability flags into *out.  Returns
 * MACH_MSG_OK or a MACH_E_*.
 */
int		 dev_info(const char *short_name, struct dev_info_reply *out);

/*
 * dev_write: DEV_OP_WRITE of up to DEV_WRITE_MAX bytes (more is
 * truncated).  Returns the bytes the driver acknowledged or a MACH_E_*.
 */
ssize_t		 dev_write(const char *short_name,
		    const void *buf, size_t len);

/* ---- kernel-side services (mirrors mach/services.h) --------------- */

#define	SVC_CLOCK_NAME		"clock"
#define	CLOCK_OP_GET		1

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_clock_reply {
	uint64_t	cr_uptime_ms;
	uint64_t	cr_uptime_us;
	uint64_t	cr_ticks;
};

#define	SVC_STATS_NAME		"stats"
#define	STATS_OP_GET		1

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_stats_reply {
	uint64_t	sr_pmm_used_pages;
	uint64_t	sr_kmem_cached_pages;
	uint64_t	sr_kernel_inuse;
	uint64_t	sr_task_count;
	uint64_t	sr_thread_count;
	uint64_t	sr_ctx_switches;
	uint64_t	sr_pmm_total_pages;
};

#define	SVC_TASKS_NAME		"tasks"
#define	TASKS_OP_LIST		1
#define	SVC_TASKS_MAX		16
#define	SVC_TASKS_NAME_MAX	24

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_tasks_entry {
	uint64_t	te_task_id;
	uint32_t	te_nthreads;
	uint32_t	te_nports;	/* names in t_port_space        */
	uint32_t	te_nvm_regions;	/* live entries in t_map        */
	uint32_t	te_pad;
	char		te_name[SVC_TASKS_NAME_MAX];
};

_Static_assert(sizeof(struct svc_tasks_entry) == 48,
    "svc_tasks_entry must be 48 bytes (wire format)");

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_tasks_reply {
	uint32_t		tr_count;
	uint32_t		tr_pad;
	struct svc_tasks_entry	tr_entries[SVC_TASKS_MAX];
};

_Static_assert(sizeof(struct svc_tasks_reply) ==
    8 + SVC_TASKS_MAX * sizeof(struct svc_tasks_entry),
    "svc_tasks_reply layout pinned");

/*
 * "progreg" -- what can be spawned.  Names arrive packed, NUL-separated,
 * to keep the reply well under svc_reply_inline's 1024 bytes.  pr_count
 * is how many fit, pr_total how many exist; a caller that sees them
 * differ should say so rather than print a short list that looks
 * complete.
 *
 * WIRE FORMAT.  Mirrors mach/services.h.
 */
#define	SVC_PROGREG_NAME	"progreg"
#define	PROGREG_OP_LIST		1
#define	SVC_PROGREG_BYTES	768

struct svc_progreg_reply {
	uint32_t	pr_count;
	uint32_t	pr_total;
	/*
	 * One bit per packed name, in packing order: set for a Mach-O
	 * image, a genuine Darwin binary rather than a native ELF.
	 */
	uint64_t	pr_macho;
	char		pr_names[SVC_PROGREG_BYTES];
};

_Static_assert(sizeof(struct svc_progreg_reply) == 16 + SVC_PROGREG_BYTES,
    "svc_progreg_reply layout pinned");

#define	SVC_ECHOOL_NAME		"echool"
#define	ECHOOL_OP_CHECKSUM	1

/* ---- host port (mirrors mach/host.h) ----------------------------- */

#define	SVC_HOST_NAME		"host"
#define	HOST_OP_PAGE_SIZE	1
#define	HOST_OP_INFO		2

#define	HOST_CPU_TYPE_X86_64		0x01000007
#define	HOST_CPU_SUBTYPE_X86_64_ALL	3

/* WIRE FORMAT.  Mirrors mach/host.h. */
struct svc_host_page_size_reply {
	uint32_t	hps_page_size;
	uint32_t	hps_pad;
};

/* WIRE FORMAT.  Mirrors mach/host.h. */
struct svc_host_info_reply {
	uint32_t	hi_max_cpus;
	uint32_t	hi_avail_cpus;
	uint64_t	hi_memory_size;
	uint32_t	hi_cpu_type;
	uint32_t	hi_cpu_subtype;
	uint64_t	hi_memory_free;
};

/*
 * Host-port client helpers (lib/style9_mach.c).
 *
 *	mach_host_self	-- bootstrap_lookup("host"); a SEND name to
 *			   deallocate when done, or MACH_PORT_NULL.
 *	host_page_size	-- HOST_OP_PAGE_SIZE into *page_size_out.
 *	host_info	-- HOST_OP_INFO: CPU count and type, total and
 *			   free memory, into *out.
 *
 * The two RPC helpers return MACH_MSG_OK or a MACH_E_* code.
 */
mach_port_name_t mach_host_self(void);
int		host_page_size(mach_port_name_t host, uint32_t *page_size_out);
int		host_info(mach_port_name_t host, struct svc_host_info_reply *out);

/*
 * Task-port client helpers (lib/style9_mach.c): vm_allocate and
 * vm_deallocate as RPCs to a task port (MACH_PORT_TASK_SELF for the
 * caller), the interface a Darwin binary uses rather than SYS_VM_*.
 * They return MACH_MSG_OK or a MACH_E_*; task_vm_allocate writes the VA
 * through *addr_out.  task_get_special_port takes a TASK_SPECIAL_* index
 * and returns a SEND name to deallocate.
 */
int		task_vm_allocate(mach_port_name_t task, uint64_t size,
		    uint32_t prot, uint64_t *addr_out);
int		task_vm_deallocate(mach_port_name_t task, uint64_t addr,
		    uint64_t size);
int		task_get_special_port(mach_port_name_t task, uint32_t which,
		    mach_port_name_t *port_out);

/* launchd service.  Mirrors mach/services.h. */
#define	SVC_LAUNCHD_NAME	"launchd"

#define	LAUNCHCTL_OP_LIST	1
#define	LAUNCHCTL_OP_LOAD	2
#define	LAUNCHCTL_OP_UNLOAD	3
#define	LAUNCHCTL_OP_STOP	4
#define	LAUNCHCTL_OP_START	5

#define	LAUNCHD_MAX_SERVICES	8
#define	LAUNCHD_NAME_MAX	24
#define	LAUNCHD_PROGRAM_MAX	24

#define	LAUNCHD_LOAD_FLAG_KEEPALIVE	0x1u
#define	LAUNCHD_LOAD_FLAG_MACHSERVICE	0x2u

#define	LAUNCHD_STATE_RUNNING	0
#define	LAUNCHD_STATE_EXITED	1
#define	LAUNCHD_STATE_FAILED	2
#define	LAUNCHD_STATE_STOPPED	3
#define	LAUNCHD_STATE_THROTTLED	4

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_load_req {
	char		lr_name[LAUNCHD_NAME_MAX];
	char		lr_program[LAUNCHD_PROGRAM_MAX];
	uint32_t	lr_flags;
	uint32_t	lr_pad;
};

_Static_assert(sizeof(struct svc_launchctl_load_req) == 56,
    "svc_launchctl_load_req must be 56 bytes (wire format)");

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_byname_req {
	char		lr_name[LAUNCHD_NAME_MAX];
};

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_status_reply {
	int32_t		ls_status;
	uint32_t	ls_state;
	uint64_t	ls_task_id;
	uint32_t	ls_taskport;	/* SEND on child task-self port  */
					/* in caller's space (LOAD ok)   */
	uint32_t	ls_pad;
};

_Static_assert(sizeof(struct svc_launchctl_status_reply) == 24,
    "svc_launchctl_status_reply must be 24 bytes (wire format)");

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_entry {
	char		le_name[LAUNCHD_NAME_MAX];
	char		le_program[LAUNCHD_PROGRAM_MAX];
	uint32_t	le_state;
	uint32_t	le_pad;
	uint64_t	le_task_id;
};

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_list_reply {
	uint32_t			ll_count;
	uint32_t			ll_pad;
	struct svc_launchctl_entry	ll_entries[LAUNCHD_MAX_SERVICES];
};

#define	SVC_MAN_NAME		"man"
#define	MAN_OP_GET		1
#define	MAN_NOT_FOUND		0xFFFFFFFFu
#define	MAN_NAME_MAX		32

/*
 * man_fetch: the rendered text of the named page ("port" for port.9),
 * delivered as an OOL anonymous range in the caller's map.  Returns
 * MACH_MSG_OK with *out_text and *out_len set, MACH_E_NAME if there is
 * no such page, or the RPC's MACH_E_*.  Every successful fetch needs a
 * man_release, or the mapping leaks.
 */
int		 man_fetch(const char *name, const char **out_text,
		    size_t *out_len);

/*
 * man_release: vm_deallocate of what man_fetch handed out, taking its
 * (text, len) and page-rounding.  Returns 0 or a negative SYS_E_*.
 */
int		 man_release(const char *text, size_t len);

#endif /* !_STYLE9_H_ */
