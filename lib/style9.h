/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _STYLE9_H_
#define	_STYLE9_H_

/*
 * libstyle9 -- user-side runtime library.
 *
 * Every ring-3 program in this tree links against libstyle9.  It pulls
 * in the bare-types, the syscall stubs, the small handful of libc-shaped
 * helpers (string, memory, bump-allocator, printf), and the Mach IPC
 * wrappers that turn an integer syscall return into something a C
 * program wants to deal with.
 *
 * Layout mirrors the kernel's BSD-flavoured headers: one header per
 * library, all declarations centralised here so a user program writes
 * `#include <style9.h>` and gets the full surface.  Implementations are
 * sliced across lib/style9_*.c by topic.
 *
 * Calling convention:  the `_start` glue in crt0.S calls `main()`, then
 * `exit(rv)` with the return value.  Programs implement `int main(void)`.
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
#define	SYS_SPAWN		9	/* phase 1b */
#define	SYS_TASK_ALIVE		10	/* phase 2  */
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

#define	SYS_E_NOSYS		(-1)
#define	SYS_E_FAULT		(-2)
#define	SYS_E_INVAL		(-3)
#define	SYS_E_NOMEM		(-4)

/*
 * Raw `syscall` instruction wrappers.  Each variant takes the syscall
 * number plus the right number of arguments and returns the kernel's
 * rax value verbatim.  Higher-level wrappers (`write`, `mach_msg_send`,
 * ...) live below and turn these into something C-typed.
 */
long	syscall0(long nr);
long	syscall1(long nr, long a0);
long	syscall2(long nr, long a0, long a1);
long	syscall3(long nr, long a0, long a1, long a2);
long	syscall4(long nr, long a0, long a1, long a2, long a3);

/* ---- process ------------------------------------------------------- */

void	exit(int code) __attribute__((noreturn));
long	yield(void);

/*
 * Spawn the named program (looks up in the kernel's progreg).  Returns
 * the new task's id on success, or a negative SYS_E_* code.  Fire-and-
 * forget: combine with task_alive() to wait for the child.
 */
long	spawn(const char *name);

/*
 * spawn_with_port: prototype lives below mach_port_name_t's typedef
 * (declared with the Mach ABI block further down).
 */

/*
 * task_alive: 1 if a task with this id is still running, 0 otherwise.
 * yield-spin around this to wait for a spawned child without baking in
 * a real notification path.
 */
int	task_alive(uint64_t task_id);

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
 * Bump-allocator malloc.  Backed by a fixed-size .bss arena (4 KiB
 * per program by default; raise STYLE9_HEAP_BYTES at compile time to
 * extend it).  free() is a NOP -- callers that need real reuse should
 * track lifetimes themselves until libstyle9 grows a freeing allocator.
 */
void	*malloc(size_t n);
void	free(void *p);

/* ---- vm ------------------------------------------------------------ */

#define	VM_PROT_READ	0x01u
#define	VM_PROT_WRITE	0x02u
#define	VM_PROT_EXEC	0x04u

/*
 * vm_allocate: request a page-aligned anonymous range from the kernel.
 * `bytes` is rounded up to a multiple of 4 KiB; `prot` is any OR of
 * VM_PROT_READ / WRITE / EXEC.  Returns the new VA on success, NULL on
 * failure (out of address space or out of frames).  The range is
 * zero-filled.
 */
void	*vm_allocate(size_t bytes, uint32_t prot);

/*
 * vm_deallocate: release a range previously returned by vm_allocate.
 * (va, bytes) MUST mirror the exact tuple vm_allocate handed out; partial
 * deallocate is rejected in v1.  Returns 0 on success, negative SYS_E_*
 * otherwise.
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
 * Notification opcodes.  Mirror mach/port.h.  Passed to
 * mach_port_request_notification, and the kernel posts a
 * mach_notify_header back to the caller's notify port carrying
 * msgh_id = MACH_NOTIFY_<TYPE>.
 */
#define	MACH_NOTIFY_FIRST		64
#define	MACH_NOTIFY_NO_SENDERS		(MACH_NOTIFY_FIRST + 6)
#define	MACH_NOTIFY_DEAD_NAME		(MACH_NOTIFY_FIRST + 8)
#define	MACH_EXC_FAULT			100

/*
 * Exception types.  Mirrors mach/port.h.  Use EXC_MASK_* bitmasks with
 * task_set_exception_ports; the kernel maps an x86 trap vector to one
 * of these and dispatches to the corresponding port slot.
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

#define	MACH_TIMEOUT_NONE		((uint64_t)0)
#define	MACH_TIMEOUT_FOREVER		((uint64_t)~0ull)

/*
 * Wire structs below mirror the kernel-side declarations in mach/port.h,
 * mach/bootstrap.h, dev/dev_proto.h, and mach/services.h.  They MUST stay
 * byte-identical: a field offset that disagrees between this header and
 * the kernel declaration silently misparses the on-wire layout, because
 * the compiled lib/style9_*.o objects read the field at THIS file's
 * offset.  The 2026-05-28 OOL bring-up bug was exactly this drift -- a
 * descriptor field reordered kernel-side, lib/style9_mach.o left stale
 * by missing Makefile dep tracking, userspace reading the wrong byte.
 *
 * Edit kernel-side first, edit here second, verify _Static_assert sizes
 * match across the boundary.  Each struct is preceded by a WIRE FORMAT
 * banner for grep-ability.
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
	uint8_t			type;		/* == MACH_MSG_PORT_DESCRIPTOR */
	uint8_t			disposition;
	uint8_t			pad1;
	uint8_t			pad2;
	mach_port_name_t	name;
};

/*
 * Packed to keep alignment at 4 instead of the 8 the uint64_t address
 * field would force; otherwise a 4-byte gap appears between body and
 * an OOL descriptor in a wire struct, which the kernel walker would
 * read as a bogus port descriptor.  See mach/port.h for the full
 * rationale.
 *
 * `deallocate = 1` asks the kernel to vm_release the source range
 * after the copy (mirrors a vm_allocate'd anonymous entry).  Best-
 * effort; non-matching ranges leave the sender's VM untouched.
 */
/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_msg_ool_descriptor {
	uint8_t			type;		/* == MACH_MSG_OOL_DESCRIPTOR */
	uint8_t			copy;		/* MACH_MSG_PHYSICAL_COPY     */
	uint8_t			deallocate;	/* 1: vm_release src post-copy */
	uint8_t			pad;
	uint32_t		size;		/* bytes to ferry              */
	uint64_t		address;	/* sender VA / receiver VA     */
} __attribute__((packed));

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_notify_header {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_NOTIFY_*    */
	uint32_t		nh_msgid;	/* user tag from request call */
	uint32_t		nh_pad;
};

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct mach_exception_header {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_EXC_FAULT    */
	uint32_t		eh_trapno;
	uint32_t		eh_err;
	uint64_t		eh_rip;
	uint64_t		eh_rsp;
	uint64_t		eh_rflags;
	uint64_t		eh_cr2;
	uint64_t		eh_task_id;
};

/* WIRE FORMAT.  Mirrors mach/port.h.  Verdict reply from a RESUMABLE watcher. */
struct mach_exception_reply {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_EXC_REPLY    */
	uint32_t		er_verdict;
	uint32_t		er_rip_advance;
};

/*
 * Port-snapshot tags.  PORT_SPECIAL_NONE matches the kernel's
 * PORT_SPECIAL_NONE; the rest of the PORT_SPECIAL_* set is included
 * so lsmp can decode kernel-tagged ports (task_self / bootstrap /
 * service) without re-reading port.h.
 */
#define	PORT_SPECIAL_NONE		0
#define	PORT_SPECIAL_TASK_SELF		1
#define	PORT_SPECIAL_BOOTSTRAP		2
#define	PORT_SPECIAL_SERVICE		3

#define	PORT_SNAPSHOT_KIND_PORT		1u
#define	PORT_SNAPSHOT_KIND_SET		2u

#define	PORT_SNAPSHOT_FLAG_DEAD		0x01u

#define	MACH_PORT_SNAPSHOT_MAX		64

/*
 * vm_map region snapshot.  Mirrors kern/vm.h.  vmmap(1) reads arrays
 * of these from SYS_TASK_GET_VM_REGIONS.
 */
#define	MACH_VM_REGION_MAX		64

#define	VME_F_ANON			0x01u	/* anonymous (pmm) backing  */
#define	VME_F_COW			0x02u	/* future: copy-on-write    */

/* WIRE FORMAT.  Mirrors kern/vm.h. */
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
#define	BOOTSTRAP_OP_LOOKUP		1
#define	BOOTSTRAP_OP_REGISTER		2
#define	BOOTSTRAP_OP_DEREGISTER		3
#define	BOOTSTRAP_REPLY_NOT_FOUND	0xFFFFFFFFu
#define	BOOTSTRAP_NAME_MAX		32

/* WIRE FORMAT.  Mirrors mach/port.h. */
struct task_info_reply {
	uint64_t	tir_task_id;
	uint32_t	tir_nthreads;
	uint32_t	tir_pad;
	char		tir_name[32];
};

/* WIRE FORMAT.  Mirrors mach/bootstrap.h. */
struct bootstrap_lookup_request {
	char	blr_name[BOOTSTRAP_NAME_MAX];
};

/* WIRE FORMAT.  Mirrors mach/bootstrap.h.  Reply body for register / deregister. */
struct bootstrap_status_reply {
	int32_t		bsr_status;
	uint32_t	bsr_pad;
};

/* ---- Mach wrappers ------------------------------------------------- */

mach_port_name_t mach_port_allocate(uint8_t rights);
int		mach_port_deallocate(mach_port_name_t name);
int		mach_msg_send(const struct mach_msg_header *msg);
int		mach_msg_recv(mach_port_name_t name,
		    struct mach_msg_header *buf, size_t buf_size);
int		mach_msg_recv_timed(mach_port_name_t name,
		    struct mach_msg_header *buf, size_t buf_size,
		    uint64_t timeout_ms);
int		mach_msg_rpc(struct mach_msg_header *req,
		    struct mach_msg_header *reply, size_t reply_size,
		    uint64_t timeout_ms);

/*
 * mach_port_mod_refs: drop ONE specific right kind from a name without
 * tearing the whole slot down.  Use when a name carries both RECV and
 * SEND and the caller wants to relinquish only one.  Returns MACH_MSG_OK
 * or a MACH_E_*.
 */
int		mach_port_mod_refs(mach_port_name_t name, uint8_t right);

/*
 * Port-set wrappers.  A port set lets one mach_msg_recv serve any of
 * several member ports.  Allocate a set, insert ports holding RECEIVE,
 * then recv on the set name -- whichever member has the next message
 * gets delivered (set members are not consumed by membership).
 */
mach_port_name_t mach_port_set_allocate(void);
int		mach_port_set_insert(mach_port_name_t set_name,
		    mach_port_name_t port_name);
int		mach_port_set_remove(mach_port_name_t set_name,
		    mach_port_name_t port_name);

/*
 * mach_port_set_extract: query which port set `port_name` belongs to.
 * Returns the set's name in the calling task's space, or MACH_PORT_NULL
 * when the port is standalone (also when the name is invalid or lacks
 * RECEIVE).  Symmetric introspection counterpart to insert / remove.
 */
mach_port_name_t mach_port_set_extract(mach_port_name_t port_name);

/*
 * mach_port_request_notification: ask the kernel to post a
 * MACH_NOTIFY_<TYPE> message to `notify_port` when the source `name`
 * reaches the matching event.  Caller must hold:
 *	NO_SENDERS -- RECEIVE on `name`, SEND on `notify_port`
 *	DEAD_NAME  -- SEND on `name`,    SEND on `notify_port`
 * `notify_msgid` is the user tag carried back in the notification's
 * nh_msgid field.  One-shot: re-arming requires another call after
 * each firing.
 */
int		mach_port_request_notification(mach_port_name_t name,
		    uint32_t notify_type, mach_port_name_t notify_port,
		    uint32_t notify_msgid);

/*
 * spawn_with_port: variant of spawn() that also hands the child a SEND
 * right at the well-known MACH_PORT_PARENT slot.  Caller must hold
 * SEND on `source_name`; the kernel transfers that right into the
 * child's port_space atomically before the child starts running.
 * Child detects the gift by mach_msg_send'ing to MACH_PORT_PARENT and
 * observing MACH_MSG_OK versus MACH_E_RIGHT (empty slot).  Returns
 * task_id on success, or a negative SYS_E_*.
 */
long	spawn_with_port(const char *name, mach_port_name_t source_name);

/*
 * task_set_exception_port: install (or replace) the calling task's
 * exception port for every type.  Equivalent to
 * task_set_exception_ports(EXC_MASK_ALL, notify_port).  Kept for
 * A v1 source compatibility -- new code should use the per-type
 * form below.  Returns MACH_MSG_OK or a MACH_E_*.
 */
int	task_set_exception_port(mach_port_name_t notify_port);

/*
 * task_set_exception_ports: install (or clear) the calling task's
 * exception ports for every EXC_TYPE named in `types_mask`.  Each
 * matching slot is overwritten with `notify_port` (kernel takes one
 * SEND ref per slot), or cleared if `notify_port == MACH_PORT_NULL`.
 * Refs to whatever was in the slot before are released by the kernel.
 * Returns MACH_MSG_OK or a MACH_E_*.
 */
int	task_set_exception_ports(uint32_t types_mask,
	    mach_port_name_t notify_port);

/*
 * thread_set_exception_ports: install (or clear) the calling
 * thread's per-type exception ports.  Slots are checked BEFORE the
 * task-level slots in user_fault_die, so a thread-level entry takes
 * precedence over a task-level one for the same type.  Same arg
 * semantics + return as task_set_exception_ports.
 */
int	thread_set_exception_ports(uint32_t types_mask,
	    mach_port_name_t notify_port);

/*
 * task_get_port_snapshot: copy one mach_port_snapshot_entry per
 * populated slot of the named task's port_space into `out`.  v1
 * accepts only task_id == 0 (the calling task); other values
 * return SYS_E_INVAL.  Capped at MACH_PORT_SNAPSHOT_MAX entries.
 *
 * Returns the number of entries written on success, or a negative
 * SYS_E_*.  Powers the userspace `lsmp` tool.
 */
long	task_get_port_snapshot(uint64_t task_id,
	    struct mach_port_snapshot_entry *out, size_t max_entries);

/*
 * task_get_vm_regions: copy one mach_vm_region_entry per live
 * vm_map entry in the named task into `out`.  v1 accepts only
 * task_id == 0 (calling task).  Capped at MACH_VM_REGION_MAX.
 * Returns the number of entries written, or a negative SYS_E_*.
 * Powers the userspace `vmmap` tool.
 */
long	task_get_vm_regions(uint64_t task_id,
	    struct mach_vm_region_entry *out, size_t max_entries);

/*
 * One-shot bootstrap lookup helper.  Builds the request, RPCs it to
 * MACH_PORT_BOOTSTRAP, returns the SEND name the kernel handed back
 * in the caller's space.  Returns MACH_PORT_NULL if the service is
 * not registered (or any error occurred).  The caller must
 * mach_port_deallocate() the returned name when done.
 */
mach_port_name_t bootstrap_lookup(const char *service);

/*
 * Publish `port` (a SEND-bearing name in the caller's space) under
 * `service` in the global bootstrap registry.  Caller retains its own
 * SEND right (COPY_SEND semantics).  Subsequent lookups by any task
 * return a fresh SEND naming the same underlying port.
 *
 * Returns MACH_MSG_OK on success, MACH_E_RIGHT if `port` does not name
 * a SEND right, MACH_E_NOSPACE if the registry is full, MACH_E_INVAL on
 * a bad or duplicate service name, or any MACH_E_* propagated from the
 * underlying RPC.
 */
int		 bootstrap_register_service(const char *service,
		    mach_port_name_t port);

/*
 * Remove the registry entry for `service` and drop the kernel-side
 * SEND right that backed it.  Returns MACH_MSG_OK on success or
 * MACH_E_INVAL if `service` was not registered.  No authentication
 * in v1: any task can deregister any name -- including the kernel
 * services from services_init.  Use with care.
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
 * dev_open_stream: bootstrap_lookup("dev/<name>") + RPC DEV_OP_OPEN_STREAM
 * + extract the port_descriptor + deallocate the control port.  Returns
 * the SEND right naming the driver's stream port, or MACH_PORT_NULL on
 * any failure.  Caller mach_msg_recv()s on the returned name to pull
 * bytes from the driver.  Caller must mach_port_deallocate() when done.
 */
mach_port_name_t dev_open_stream(const char *short_name);

/*
 * dev_info: query the named driver for its kind + capability flags.
 * Returns MACH_MSG_OK on success and fills *out; any negative MACH_E_*
 * otherwise.  Lightweight probe before opening a stream.
 */
int		 dev_info(const char *short_name, struct dev_info_reply *out);

/*
 * dev_write: send DEV_OP_WRITE to the driver named "dev/<short_name>".
 * Up to DEV_WRITE_MAX bytes; returns the number of bytes the driver
 * acknowledged, or a negative MACH_E_*.  Used by future TX consumers
 * (e.g. a network logger speaking dev/uart).
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
	uint32_t	te_pad;
	char		te_name[SVC_TASKS_NAME_MAX];
};

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_tasks_reply {
	uint32_t		tr_count;
	uint32_t		tr_pad;
	struct svc_tasks_entry	tr_entries[SVC_TASKS_MAX];
};

#define	SVC_ECHOOL_NAME		"echool"
#define	ECHOOL_OP_CHECKSUM	1

/*
 * launchd service wire mirror.  Match mach/services.h exactly --
 * any size or field drift here silently misparses the on-wire shape.
 */
#define	SVC_LAUNCHD_NAME	"launchd"

#define	LAUNCHCTL_OP_LIST	1
#define	LAUNCHCTL_OP_LOAD	2
#define	LAUNCHCTL_OP_UNLOAD	3

#define	LAUNCHD_MAX_SERVICES	8
#define	LAUNCHD_NAME_MAX	24
#define	LAUNCHD_PROGRAM_MAX	24

#define	LAUNCHD_STATE_RUNNING	0
#define	LAUNCHD_STATE_EXITED	1
#define	LAUNCHD_STATE_FAILED	2

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_load_req {
	char		lr_name[LAUNCHD_NAME_MAX];
	char		lr_program[LAUNCHD_PROGRAM_MAX];
};

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_byname_req {
	char		lr_name[LAUNCHD_NAME_MAX];
};

/* WIRE FORMAT.  Mirrors mach/services.h. */
struct svc_launchctl_status_reply {
	int32_t		ls_status;
	uint32_t	ls_state;
	uint64_t	ls_task_id;
};

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
 * man_fetch: ask the kernel "man" service for the rendered text of the
 * named page (e.g. "port" for port.9).  On success returns MACH_MSG_OK
 * and writes the buffer address + length to *out_text and *out_len; the
 * buffer is an OOL-installed anonymous range in the caller's vm_map.
 * Pair every successful fetch with man_release to drop the backing
 * pages -- otherwise repeated fetches leak one mapping each.
 *
 * Returns MACH_E_NAME if no such page is registered, or any MACH_E_*
 * propagated from the underlying RPC.
 */
int		 man_fetch(const char *name, const char **out_text,
		    size_t *out_len);

/*
 * man_release: tear down a buffer returned by a successful man_fetch.
 * Equivalent to vm_deallocate but takes the (text, len) tuple the
 * fetch handed out -- size is page-rounded internally to match what
 * the kernel allocated.  Returns 0 on success, negative SYS_E_* on
 * failure (e.g. the range does not match a known allocation).
 */
int		 man_release(const char *text, size_t len);

#endif /* !_STYLE9_H_ */
