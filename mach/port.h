/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_PORT_H_
#define	_SYS_PORT_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Mach-flavoured port subsystem.
 *
 * A port is a kernel object with a FIFO of in-flight messages, at most
 * one RECEIVE right, and any number of SEND and SEND_ONCE rights.  A
 * port space is a per-task table mapping integer names to rights; names
 * are local to a space.  The kernel's own space is kernel_space.
 *
 * The header wire format is Mach's, and kernel-internal messaging uses
 * the same layout as the SYS_MSG_* syscalls.  Wire structs (marked
 * WIRE FORMAT) are ABI-stable: fields keep their offsets, new fields
 * append, sizes are pinned by _Static_assert.  Reordering would make
 * anything built against the old layout (lib/style9_mach.o) misread it.
 *
 *   24-byte mach_msg_header
 *	   { msgh_bits, msgh_size, msgh_remote, msgh_local, voucher, id }
 *   if MACH_MSGH_BITS_COMPLEX:
 *     4-byte body { ndescs }
 *     ndescs descriptors, each one of:
 *	   8-byte  port_descriptor  { type=0, disposition, name }
 *	   16-byte ool_descriptor   { type=1, copy, size, address }
 *     then inline payload, up to msgh_size bytes in all.
 *
 * The descriptor area is variable-stride: byte 0 of each descriptor is
 * its type tag, which gives its size.
 *
 * Port descriptors move or copy one right from the sender's space into
 * the receiver's: name -> port on send, port -> fresh name on receive;
 * a message in flight holds port refs directly.
 *
 * OOL descriptors name a sender VA range.  On send the kernel captures
 * the payload's frames, sharing the sender's pages copy-on-write where it
 * can (vm/vm.h); on receive it maps them into the receiver and rewrites
 * `address` to the receiver VA.
 *
 * A send to a full queue blocks until a receive frees a slot;
 * mach_msg_recv_block and mach_msg_recv_timed block on an empty one.
 */

typedef uint32_t	mach_port_name_t;

#define	MACH_PORT_NULL		((mach_port_name_t)0)
#define	MACH_PORT_DEAD		((mach_port_name_t)0xFFFFFFFFu)

/* Right bits (used as a mask in the name table). */
#define	MACH_PORT_RIGHT_RECEIVE		0x01u
#define	MACH_PORT_RIGHT_SEND		0x02u
#define	MACH_PORT_RIGHT_SEND_ONCE	0x04u
#define	MACH_PORT_RIGHT_PORT_SET	0x08u	/* recv-on-many aggregator */

/*
 * Returned by mach_port_type(), never stored as a right: the name held
 * SEND / SEND_ONCE and its port has died, so it is now a dead name.
 * Distinct from the right bits and from 0 (no such name), so a holder
 * can tell dead from wrong right from absent without sending.
 */
#define	MACH_PORT_TYPE_DEAD_NAME	0x10u

/*
 * msgh_bits layout (same as Mach):
 *   bits  0..7   disposition for msgh_remote_port
 *   bits  8..15  disposition for msgh_local_port
 *   bit   31     MACH_MSGH_BITS_COMPLEX -- body+descriptors follow header
 */
#define	MACH_MSGH_BITS_REMOTE(b)	((uint8_t)((b) & 0xFF))
#define	MACH_MSGH_BITS_LOCAL(b)		((uint8_t)(((b) >> 8) & 0xFF))
#define	MACH_MSGH_BITS_COMPLEX		0x80000000u

#define	MACH_MSGH_BITS(remote, local)	\
	(((uint32_t)(remote) & 0xFFu) | (((uint32_t)(local) & 0xFFu) << 8))

/*
 * The msgh_bits the kernel consumes: both disposition bytes (0..15) and
 * COMPLEX (31).  The rest are reserved and must be zero on send;
 * mach_msg_send rejects them with MACH_E_INVAL.
 */
#define	MACH_MSGH_BITS_USED_MASK	\
	((uint32_t)MACH_MSGH_BITS_COMPLEX | (uint32_t)0xFFFFu)

/*
 * Dispositions: what the kernel does with the right named in a slot.
 * Numbers match Mach.
 */
#define	MACH_MSG_TYPE_MOVE_RECEIVE	16	/* sender transfers RECV      */
#define	MACH_MSG_TYPE_MOVE_SEND		17	/* sender transfers SEND      */
#define	MACH_MSG_TYPE_MOVE_SEND_ONCE	18	/* sender transfers SEND_ONCE */
#define	MACH_MSG_TYPE_COPY_SEND		19	/* sender keeps SEND          */
#define	MACH_MSG_TYPE_MAKE_SEND		20	/* sender holds RECV -> SEND  */
#define	MACH_MSG_TYPE_MAKE_SEND_ONCE	21	/* sender holds RECV -> SO    */

/*
 * Notifications.  A notify port is registered for an event with
 * port_request_notification; when the event fires the kernel posts a
 * message with msgh_id = MACH_NOTIFY_<EVENT> to it.  One-shot: firing
 * consumes the registration.  Numbers match Mach (MACH_NOTIFY_FIRST =
 * 64).
 *
 *	MACH_NOTIFY_NO_SENDERS	the last SEND / SEND_ONCE right on the
 *				port was dropped while RECEIVE is still
 *				held: the client has gone away.
 *
 *	MACH_NOTIFY_SEND_ONCE	a send-once right was destroyed unused.
 *				Not registered: the kernel posts it to the
 *				right's own target, so a client awaiting a
 *				reply that will never come is unblocked.
 *
 *	MACH_NOTIFY_DEAD_NAME	a watched port died; the holder's SEND name
 *				is now a dead name (mach_port_type reports
 *				MACH_PORT_TYPE_DEAD_NAME).  Multi-registrant:
 *				every watcher is notified, each on its own
 *				notify port with its own tag.
 *
 *	MACH_NOTIFY_PORT_DESTROYED
 *				the receive right was about to be destroyed
 *				(deallocated, its task died, a message holding
 *				it was dropped).  It comes to the notify port
 *				instead, in the message, so the port does not
 *				die: its queue, its senders and their names
 *				stay as they were.  Registered by the receiver;
 *				a supervisor gets back a crashed server's port.
 *				If the notify port is dead, the port dies.
 */
#define	MACH_NOTIFY_FIRST		64
#define	MACH_NOTIFY_PORT_DESTROYED	(MACH_NOTIFY_FIRST + 5)
#define	MACH_NOTIFY_NO_SENDERS		(MACH_NOTIFY_FIRST + 6)
#define	MACH_NOTIFY_SEND_ONCE		(MACH_NOTIFY_FIRST + 7)
#define	MACH_NOTIFY_DEAD_NAME		(MACH_NOTIFY_FIRST + 8)

/*
 * Exception messages.  On a ring-3 fault the kernel posts a
 * mach_exception_header (msgh_id MACH_EXC_FAULT) to the task's exception
 * port for the fault's type (SYS_TASK_SET_EXC_PORT / _PORTS) -- typically
 * a debugger or crash reporter in another task.  Numbered above the
 * notify range.
 */
#define	MACH_EXC_FAULT			100

/*
 * Exception types: indices into the task's t_exc_ports[], chosen by
 * exc_type_from_trapno from the x86 vector.  The EXC_MASK_* forms select
 * several slots in one SYS_TASK_SET_EXC_PORTS call.
 *
 *	BAD_ACCESS       #PF, #GP, #SS, #NP and anything unclassified
 *	BAD_INSTRUCTION  #UD, #NM (illegal opcode / device-not-available)
 *	ARITHMETIC       #DE, #MF, #XM (divide, x87, SSE)
 *	BREAKPOINT       #BP (INT3)
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

/*
 * Exception-port flags, in the high 16 bits of SYS_TASK_SET_EXC_PORTS's
 * mask argument (types in the low 16).
 *
 *	EXC_FLAG_RESUMABLE   opt into the reply protocol: user_fault_die
 *			    parks the faulting thread on a kernel-owned
 *			    reply port (msgh_local of the exception
 *			    message) until the watcher's
 *			    mach_exception_reply arrives.  KILL retires
 *			    the thread; RESUME advances tf_rip by
 *			    er_rip_advance and returns to user mode.  No
 *			    reply within EXC_REPLY_TIMEOUT_MS is KILL.
 *
 *			    Clear (the default): the thread is retired
 *			    right after the exception is posted.
 *
 * Unknown flag bits (outside EXC_FLAGS_VALID) fail with MACH_E_INVAL.
 */
#define	EXC_FLAG_RESUMABLE		0x00010000u
#define	EXC_FLAGS_VALID			EXC_FLAG_RESUMABLE

/*
 * Exception reply msgh_id and the verdicts in its body.  RESUME advances
 * RIP by er_rip_advance and returns to user mode; KILL retires the
 * thread; STEP (single-step via RFLAGS.TF) is reserved and unimplemented.
 */
#define	MACH_EXC_REPLY			101

#define	EXC_VERDICT_KILL		0u
#define	EXC_VERDICT_RESUME		1u
#define	EXC_VERDICT_STEP		2u

/*
 * How long a resumable faulting thread waits for a verdict, so a dead
 * watcher cannot pin it; expiry means KILL.
 */
#define	EXC_REPLY_TIMEOUT_MS		500u

/*
 * Descriptor type tags, byte 0 of every descriptor.  A PORT descriptor
 * is 8 bytes, an OOL descriptor 16.
 */
#define	MACH_MSG_PORT_DESCRIPTOR	0
#define	MACH_MSG_OOL_DESCRIPTOR		1

/*
 * OOL copy flavours, as in Mach.  Both are accepted and treated alike:
 * the kernel itself decides which pages to share copy-on-write and which
 * to copy, and the receiver always sees PHYSICAL_COPY.  Either way the
 * receiver gets the bytes as they were at the send.
 */
#define	MACH_MSG_VIRTUAL_COPY		0
#define	MACH_MSG_PHYSICAL_COPY		1

/*
 * Upper bound on one OOL descriptor's size, capping the frames a sender
 * can make the kernel capture.  Raise as needed; one page is the floor.
 */
#define	MACH_MSG_OOL_MAX_BYTES		(1u << 20)	/* 1 MiB           */

/*
 * WIRE FORMAT.  ABI-stable, and byte-exact with Darwin's mach_msg_header_t:
 *	msgh_bits @0  msgh_size @4  msgh_remote(_port) @8  msgh_local(_port) @12
 *	msgh_voucher(_port) @16  msgh_id @20  (24 bytes, asserted below).
 * So a TASK_PERSONALITY_DARWIN binary's own headers pass through the
 * mach_msg path unchanged (kern/darwin.c).
 */
struct mach_msg_header {
	uint32_t		msgh_bits;
	uint32_t		msgh_size;	/* total bytes incl header */
	mach_port_name_t	msgh_remote;	/* dest port name (sender NS) */
	mach_port_name_t	msgh_local;	/* reply port name (sender NS) */
	uint32_t		msgh_voucher;	/* reserved, set 0          */
	uint32_t		msgh_id;	/* caller's protocol id     */
};

/* WIRE FORMAT.  ABI-stable. */
struct mach_msg_body {
	uint32_t		msgh_descriptor_count;
};

/* Port-right descriptor.  Eight bytes, type tag at byte 0. */
/* WIRE FORMAT.  ABI-stable. */
struct mach_msg_port_descriptor {
	uint8_t			type;		/* == MACH_MSG_PORT_DESCRIPTOR */
	uint8_t			disposition;
	uint8_t			pad1;
	uint8_t			pad2;
	mach_port_name_t	name;
};

/*
 * Out-of-line memory descriptor.  Sixteen bytes, type tag at byte 0.  On
 * send `address` is a sender VA; on receive it is rewritten to the
 * receiver VA where the payload was mapped.
 *
 * `deallocate` = 1 asks the kernel to vm_map_release the sender's
 * page-rounded range once the payload is captured.  Best-effort: a range
 * with a hole or a non-anonymous entry is left alone.  Ignored for
 * kernel senders (kernel_task, trusted sends), whose addresses are
 * kernel VA.
 *
 * Packed: aligned to 8, the struct would leave a 4-byte gap after the
 * 4-byte mach_msg_body in a containing struct, and those zero bytes
 * parse as a port descriptor (type 0), so the send fails with
 * MACH_E_INVAL.  x86_64 tolerates the misaligned uint64_t.
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_msg_ool_descriptor {
	uint8_t			type;		/* == MACH_MSG_OOL_DESCRIPTOR */
	uint8_t			copy;		/* MACH_MSG_*_COPY            */
	uint8_t			deallocate;	/* 1: release source after send */
	uint8_t			pad;
	uint32_t		size;		/* bytes to ferry              */
	uint64_t		address;	/* sender VA / receiver VA     */
} __attribute__((packed));

_Static_assert(sizeof(struct mach_msg_header) == 24,
    "mach_msg_header must be 24 bytes");
_Static_assert(sizeof(struct mach_msg_port_descriptor) == 8,
    "mach_msg_port_descriptor must be 8 bytes (wire format)");
_Static_assert(sizeof(struct mach_msg_ool_descriptor) == 16,
    "mach_msg_ool_descriptor must be 16 bytes (wire format)");

/*
 * A kernel notification.  msgh_id is the MACH_NOTIFY_* code; nh_msgid is
 * the tag given at registration, telling apart sources that share one
 * notify port.
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_notify_header {
	struct mach_msg_header	hdr;
	uint32_t		nh_msgid;
	uint32_t		nh_pad;
};

_Static_assert(sizeof(struct mach_notify_header) == 32,
    "mach_notify_header must be 32 bytes (wire format)");

/*
 * MACH_NOTIFY_PORT_DESTROYED: complex, its one descriptor the receive
 * right (disposition MOVE_RECEIVE), under a new name in the receiver's
 * space; nd_msgid the registration's tag.
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_port_destroyed_notification {
	struct mach_msg_header		hdr;
	struct mach_msg_body		body;
	struct mach_msg_port_descriptor	not_port;
	uint32_t			nd_msgid;
	uint32_t			nd_pad;
};

_Static_assert(sizeof(struct mach_port_destroyed_notification) == 44,
    "mach_port_destroyed_notification must be 44 bytes (wire format)");

/*
 * An exception message (msgh_id MACH_EXC_FAULT), posted when a ring-3
 * thread faults.  Registers are the trapframe's at the fault; eh_cr2 is
 * the faulting VA for #PF, else 0; eh_task_id names the faulting task.
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_exception_header {
	struct mach_msg_header	hdr;
	uint32_t		eh_trapno;	/* x86 vector              */
	uint32_t		eh_err;		/* CPU-supplied error code */
	uint64_t		eh_rip;
	uint64_t		eh_rsp;
	uint64_t		eh_rflags;
	uint64_t		eh_cr2;		/* #PF faulting VA, else 0 */
	uint64_t		eh_task_id;
};

_Static_assert(sizeof(struct mach_exception_header) == 72,
    "mach_exception_header must be 72 bytes (wire format)");

/*
 * The watcher's verdict under EXC_FLAG_RESUMABLE.  msgh_id must be
 * MACH_EXC_REPLY; a mismatch or an unknown verdict is KILL.
 * er_rip_advance, used only for RESUME, is the byte count to skip past
 * the faulting instruction (2 for ud2, 1 for INT3, ...).
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_exception_reply {
	struct mach_msg_header	hdr;		/* msgh_id = MACH_EXC_REPLY */
	uint32_t		er_verdict;	/* EXC_VERDICT_*            */
	uint32_t		er_rip_advance;	/* bytes past faulting insn */
};

_Static_assert(sizeof(struct mach_exception_reply) == 32,
    "mach_exception_reply must be 32 bytes (wire format)");

/*
 * One populated slot of a port_space, as returned in arrays by
 * SYS_TASK_GET_PORT_SNAPSHOT.
 *
 *	mpse_name		the name in the snapshotted space.
 *	mpse_kind		PORT_SNAPSHOT_KIND_*, which says which
 *				fields are valid:
 *	  PORT			mpse_object_id is p_id; qlen, qmax, refs,
 *				send_count, send_once_count filled;
 *				member_count 0.
 *	  SET			mpse_object_id is ps_id; refs and
 *				member_count filled; queue fields 0.
 *	mpse_rights		MACH_PORT_RIGHT_* mask held under the name.
 *	mpse_special		PORT_SPECIAL_* of the port (NONE for sets).
 *	mpse_flags		PORT_SNAPSHOT_FLAG_DEAD if the object is dead.
 */
#define	PORT_SNAPSHOT_KIND_PORT		1u
#define	PORT_SNAPSHOT_KIND_SET		2u

#define	PORT_SNAPSHOT_FLAG_DEAD		0x01u

/* WIRE FORMAT.  ABI-stable. */
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

_Static_assert(sizeof(struct mach_port_snapshot_entry) == 40,
    "mach_port_snapshot_entry must be 40 bytes (wire format)");

/*
 * Most entries one snapshot syscall writes: enough for a typical task
 * (task_self, bootstrap, parent, a handful of ports, exception ports),
 * small enough for the kernel's staging buffer to live on the stack.
 */
#define	MACH_PORT_SNAPSHOT_MAX		64

/* Result codes of the mach_msg_* and port calls (0 is success). */
#define	MACH_MSG_OK		0
#define	MACH_E_INVAL		1
#define	MACH_E_NAME		2	/* name not in space               */
#define	MACH_E_RIGHT		3	/* name in space but wrong right   */
#define	MACH_E_DEAD		4	/* port is dead                    */
#define	MACH_E_NOSPACE		5	/* queue full / table full         */
#define	MACH_E_NOMSG		6	/* recv on empty queue             */
#define	MACH_E_TOOSMALL		7	/* caller's recv buf too small     */
#define	MACH_E_NOMEM		8	/* kmalloc failed                  */
#define	MACH_E_TIMEOUT		9	/* a timed recv or send's deadline */
#define	MACH_E_INTR		10	/* the waiter's task was killed    */

/*
 * Special timeout values for mach_msg_recv_timed and mach_msg_send_timed.
 *	NONE	 -- never wait: MACH_E_NOMSG on an empty queue, MACH_E_TIMEOUT
 *		    on a full one
 *	FOREVER	 -- wait without a bound
 */
#define	MACH_TIMEOUT_NONE	((uint64_t)0)
#define	MACH_TIMEOUT_FOREVER	((uint64_t)~0ull)

/*
 * Special ports are kernel-implemented objects.  A message to a port
 * with a nonzero p_special is dispatched synchronously in the send path:
 * no server thread, no queueing, and the dispatcher sends any reply to
 * msgh_local itself.  Mach does the same for mach_task_self,
 * mach_host_self and the like.
 *
 *	NONE		an ordinary port
 *	TASK_SELF	p_special_arg is the task's id (uint64_t as
 *			uintptr_t), not a struct task *: the port can
 *			outlive the task, so dispatch resolves the id with
 *			task_lookup_ref and fails safe once it is reaped
 *	BOOTSTRAP	the global service registry; no arg
 *	SERVICE		p_special_arg is a port_service_fn: the kernel
 *			services, host port and driver control ports
 */
#define	PORT_SPECIAL_NONE		0
#define	PORT_SPECIAL_TASK_SELF		1
#define	PORT_SPECIAL_BOOTSTRAP		2
#define	PORT_SPECIAL_SERVICE		3

struct port_space;	/* fwd decl for the typedef below */

/*
 * A PORT_SPECIAL_SERVICE dispatcher, kept in p_special_arg.  Called from
 * mach_msg_send with the request and the sender's space; it sends any
 * reply to req->msgh_local, and its return value is the sender's
 * mach_msg_send result.
 */
typedef int (*port_service_fn)(const struct mach_msg_header *req,
		struct port_space *from);

/*
 * Every task's space holds SEND rights to its own task_self port and to
 * the bootstrap port at these names, installed by task_create before its
 * first thread runs.
 */
#define	MACH_PORT_TASK_SELF		((mach_port_name_t)1)
#define	MACH_PORT_BOOTSTRAP		((mach_port_name_t)2)

/*
 * The name of a SEND right the parent injected at spawn
 * (SYS_SPAWN_WITH_PORT).  Without one the slot is empty and a send to
 * it fails with MACH_E_RIGHT.  It is the next free name after TASK_SELF
 * and BOOTSTRAP; usermode_elf_launcher panics if the install lands
 * anywhere else.
 */
#define	MACH_PORT_PARENT		((mach_port_name_t)3)

/* msgh_id of requests to a task_self port; payloads follow. */
#define	TASK_OP_GET_INFO		1
#define	TASK_OP_VM_ALLOCATE		2
#define	TASK_OP_VM_DEALLOCATE		3
#define	TASK_OP_GET_SPECIAL_PORT	4

/* TASK_OP_GET_INFO reply payload, right after the header. */
/* WIRE FORMAT.  ABI-stable. */
struct task_info_reply {
	uint64_t	tir_task_id;
	uint32_t	tir_nthreads;
	uint32_t	tir_pad;
	char		tir_name[32];
};

_Static_assert(sizeof(struct task_info_reply) == 48,
    "task_info_reply must be 48 bytes (wire format)");

/*
 * TASK_OP_VM_ALLOCATE / TASK_OP_VM_DEALLOCATE: the SYS_VM_ALLOCATE
 * allocator reached through the task port, as a Darwin binary reaches
 * mach_vm_allocate.  Requests are inline after the header; replies carry
 * an in-band status, since the allocation can fail when the message did
 * not.
 */
/* WIRE FORMAT.  ABI-stable. */
struct task_vm_allocate_request {
	uint64_t	tva_size;	/* bytes; kernel page-rounds  */
	uint32_t	tva_prot;	/* VM_PROT_* (READ/WRITE/EXEC) */
	uint32_t	tva_pad;
};

_Static_assert(sizeof(struct task_vm_allocate_request) == 16,
    "task_vm_allocate_request must be 16 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable. */
struct task_vm_allocate_reply {
	uint64_t	tvar_address;	/* allocated VA, 0 on failure */
	int32_t		tvar_status;	/* MACH_MSG_OK or a MACH_E_*   */
	uint32_t	tvar_pad;
};

_Static_assert(sizeof(struct task_vm_allocate_reply) == 16,
    "task_vm_allocate_reply must be 16 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable. */
struct task_vm_deallocate_request {
	uint64_t	tvd_address;	/* base VA to release          */
	uint64_t	tvd_size;	/* byte length (page-rounded)  */
};

_Static_assert(sizeof(struct task_vm_deallocate_request) == 16,
    "task_vm_deallocate_request must be 16 bytes (wire format)");

/* WIRE FORMAT.  ABI-stable. */
struct task_vm_deallocate_reply {
	int32_t		tvdr_status;	/* MACH_MSG_OK or a MACH_E_*   */
	uint32_t	tvdr_pad;
};

_Static_assert(sizeof(struct task_vm_deallocate_reply) == 8,
    "task_vm_deallocate_reply must be 8 bytes (wire format)");

/*
 * TASK_OP_GET_SPECIAL_PORT: a SEND right to the special port named by
 * `tsp_which`, as one port descriptor in a COMPLEX reply; a non-complex
 * reply means unknown or unavailable.  Indices follow XNU's
 * task_get_special_port(); 1 and 3 (kernel, name) are not provided.
 */
#define	TASK_SPECIAL_HOST		2	/* the host port      */
#define	TASK_SPECIAL_BOOTSTRAP		4	/* the bootstrap port */

/* WIRE FORMAT.  ABI-stable. */
struct task_special_port_request {
	uint32_t	tsp_which;	/* TASK_SPECIAL_* */
	uint32_t	tsp_pad;
};

_Static_assert(sizeof(struct task_special_port_request) == 8,
    "task_special_port_request must be 8 bytes (wire format)");

/* Opaque kernel objects. */
struct port;
struct port_space;
struct task;

/* The kernel's own port name space. */
extern struct port_space	*kernel_space;

/* Subsystem bring-up; safe to call once kmem is initialised. */
void	port_subsystem_init(void);

/*
 * port_space_new creates an empty name table; port_space_destroy drops
 * every right it still holds.  Each task owns one.
 */
struct port_space	*port_space_new(void);
void			 port_space_destroy(struct port_space *);

/*
 * Create a port and grant `space` the rights in `right_mask` under one
 * new name; RECEIVE | SEND gives a name to both serve and send to.
 * Returns MACH_PORT_NULL on failure.
 */
mach_port_name_t	 port_allocate(struct port_space *, uint8_t right_mask);

/*
 * Drop one name from `space`, releasing every right under it (or
 * freeing a dead name).  A SEND_ONCE right dropped this way fires
 * MACH_NOTIFY_SEND_ONCE.  Dropping RECEIVE kills the port and drains its
 * queue; it is freed with its last reference.
 */
int			 port_deallocate(struct port_space *,
			    mach_port_name_t);

/*
 * Report what `name` refers to in `space`, without sending anything:
 *	0				name is unallocated.
 *	a MACH_PORT_RIGHT_* mask		the rights held.
 *	MACH_PORT_TYPE_DEAD_NAME		the port died; the name is dead.
 * Resolved lazily: the first query to find a SEND / SEND_ONCE name whose
 * port has died turns the entry into a dead name, releasing its ref on
 * the port.  A name holding RECEIVE, or a set, is always reported by its
 * rights.  port_deallocate frees a dead name like any other.
 */
uint32_t		 mach_port_type(struct port_space *,
			    mach_port_name_t);

/*
 * Port sets: many ports' receive queues behind one name, so one thread
 * can serve them all with one recv.
 *
 *	port_set_allocate -- new set (MACH_PORT_RIGHT_PORT_SET) in `space`
 *	port_set_insert   -- add the port `port_name` (RECEIVE needed)
 *	port_set_remove   -- make it standalone again
 *
 * A port is in at most one set.  A set cannot be sent to, only received
 * on; a recv on it delivers from any member.
 */
mach_port_name_t	 port_set_allocate(struct port_space *);
int			 port_set_insert(struct port_space *,
			    mach_port_name_t set_name,
			    mach_port_name_t port_name);
int			 port_set_remove(struct port_space *,
			    mach_port_name_t set_name,
			    mach_port_name_t port_name);

/*
 * The name in `space` of the set `port_name` belongs to, or
 * MACH_PORT_NULL if it is standalone.  Read-only.  Assumes the set is
 * named in the same space, as nothing can share a set across spaces.
 */
mach_port_name_t	 port_set_extract(struct port_space *space,
			    mach_port_name_t port_name);

/*
 * Drop one right kind from a name, keeping the other rights under it;
 * the name goes when its last right does.  MACH_E_NAME if `name` does
 * not carry `right`.  Lets a test model "the last sender left while the
 * receiver is parked" within one space (SYS_PORT_MOD_REFS).
 */
int			 port_mod_refs(struct port_space *,
			    mach_port_name_t name, uint8_t right);

/*
 * Arrange for a notification to be posted to `notify_port_name` when the
 * port at `name` reaches `notify_type`: MACH_NOTIFY_NO_SENDERS or
 * MACH_NOTIFY_PORT_DESTROYED (caller holds RECEIVE on `name`), or
 * MACH_NOTIFY_DEAD_NAME (caller holds SEND).  Any other type, or a port
 * watching itself, is MACH_E_INVAL.  The caller needs SEND on the notify
 * port.  `notify_msgid` comes back in nh_msgid (nd_msgid), to tell
 * sources sharing one notify port apart.
 *
 * NO_SENDERS and PORT_DESTROYED have one slot each: registering again
 * replaces the target.  DEAD_NAME has one watch per notify target, so
 * every SEND holder can be told; re-arming a target only updates its tag.
 * *prev_out is always MACH_PORT_NULL -- tracking a replaced target is the
 * caller's job.  The kernel holds a SEND ref on the notify port until the
 * notification fires or the source's RECEIVE is released.
 */
int			 port_request_notification(struct port_space *space,
			    mach_port_name_t name, uint32_t notify_type,
			    mach_port_name_t notify_port_name,
			    uint32_t notify_msgid,
			    mach_port_name_t *prev_out);

/*
 * DEAD_NAME arming on port objects, for in-kernel watchers that hold
 * pointers (the launchd keep_alive worker): no names, no rights check.
 * Posts MACH_NOTIFY_DEAD_NAME with `tag` to `notify` when `watched`'s
 * RECEIVE is released, holding a SEND ref on `notify` until then.
 * MACH_E_DEAD if `watched` is already dead.
 */
int			 port_arm_dead_name_object(struct port *watched,
			    struct port *notify, uint32_t tag);

/*
 * Post a MACH_EXC_FAULT message to an exception port, for the trap
 * handler.  The caller holds a SEND ref on `port` and keeps it.  Fields
 * rather than a trapframe keep mach/ free of arch/.  Best-effort: a dead
 * port or full queue drops the message and returns the error.
 *
 * With a `reply_port`, the message carries it as msgh_local (MAKE_SEND),
 * and the watcher answers with a mach_exception_reply to that name while
 * the faulting thread waits on `reply_port`.  Without one the watcher
 * only observes, and the caller retires the thread.
 */
int			 port_exception_post(struct port *port,
			    uint32_t trapno, uint32_t err,
			    uint64_t rip, uint64_t rsp, uint64_t rflags,
			    uint64_t cr2, uint64_t task_id,
			    struct port *reply_port);

/*
 * Copy the SEND right `src_name` in `src` to a new name in `dst`,
 * returned in *dst_name_out; `src_name` keeps its right.  Lets a parent
 * wire a child up before it runs, where Mach would go through task ports
 * and the bootstrap server.
 */
int			 port_space_inject_send(struct port_space *src,
			    mach_port_name_t src_name,
			    struct port_space *dst,
			    mach_port_name_t *dst_name_out);

/*
 * mach_msg_send: queue a copy of `msg` on the port named msgh_remote in
 * `from`, carrying rights and OOL payloads per the descriptors.  The
 * caller's buffer is only read.  Blocks while the queue is full;
 * MACH_E_DEAD if the port dies meanwhile.  A message through a send-once
 * right (MOVE_SEND_ONCE, or MAKE_SEND_ONCE by the receiver) never waits:
 * see KERNEL_QMAX.
 */
int			 mach_msg_send(struct port_space *from,
			    const struct mach_msg_header *msg);

/*
 * mach_msg_send with the wait for queue room bounded by `timeout_ms`, as
 * for mach_msg_recv_timed: MACH_E_TIMEOUT at the deadline, at once for
 * MACH_TIMEOUT_NONE.  The wait comes before anything of the message's
 * moves, so a send that times out leaves the sender's rights as they
 * were.  MACH_E_INTR if the sender's task is killed while it waits.
 */
int			 mach_msg_send_timed(struct port_space *from,
			    const struct mach_msg_header *msg,
			    uint64_t timeout_ms);

/*
 * mach_msg_send with the current thread marked a trusted kernel sender:
 * OOL addresses skip the user-VA check, so a kernel service can ship
 * .rodata to a user caller (the "man" service).  Kernel callers only.
 */
int			 mach_msg_send_trusted(struct port_space *from,
			    const struct mach_msg_header *msg);

/*
 * mach_msg_recv: dequeue the next message from `recv_name` (RECEIVE in
 * `to`) into `buf`, translating port descriptors and msgh_local into
 * names in `to` and mapping OOL payloads.  MACH_E_NOMSG if the queue is
 * empty; MACH_E_TOOSMALL leaves the message queued.
 */
int			 mach_msg_recv(struct port_space *to,
			    mach_port_name_t recv_name,
			    struct mach_msg_header *buf, size_t buf_size);

/*
 * Blocking variant: sleep until a message arrives, or MACH_E_DEAD if
 * the port dies.  `recv_name` may also name a port set.  Requires a
 * running scheduler.
 */
int			 mach_msg_recv_block(struct port_space *to,
			    mach_port_name_t recv_name,
			    struct mach_msg_header *buf, size_t buf_size);

/*
 * mach_msg_recv_timed: recv_block with the wait bounded by `timeout_ms`
 * of clock_uptime_ms().
 *	MACH_TIMEOUT_NONE	poll: MACH_E_NOMSG if empty
 *	MACH_TIMEOUT_FOREVER	no bound (== recv_block)
 *	any other value		MACH_E_TIMEOUT at the deadline
 * On timeout `buf` is untouched.
 */
int			 mach_msg_recv_timed(struct port_space *to,
			    mach_port_name_t recv_name,
			    struct mach_msg_header *buf, size_t buf_size,
			    uint64_t timeout_ms);

/*
 * mach_msg_rpc: send `req`, then wait for one reply in `reply_buf`.  A
 * reply port is allocated in `space` and written into req->msgh_local as
 * MAKE_SEND, so the server gets a SEND right to answer on; it is
 * deallocated before return, whatever the outcome.  `timeout_ms` bounds
 * the wait as for mach_msg_recv_timed.
 */
int			 mach_msg_rpc(struct port_space *space,
			    struct mach_msg_header *req,
			    struct mach_msg_header *reply_buf,
			    size_t reply_buf_size,
			    uint64_t timeout_ms);

/*
 * Create `t`'s task_self port (PORT_SPECIAL_TASK_SELF, dispatched to
 * task_self_dispatch), keep its RECEIVE in the kernel, and install a
 * SEND right at MACH_PORT_TASK_SELF in t->t_port_space, which must be
 * empty -- any other name panics.  A no-op once installed.  On failure
 * nothing is left behind.
 */
int			 port_install_task_self(struct task *);

/*
 * Release the kernel's RECEIVE right on `t`'s task_self port, at task
 * destruction after its space has dropped the SEND.
 */
void			 port_release_task_self(struct task *);

/*
 * Install a SEND right to the global bootstrap port (mach/bootstrap.c)
 * at MACH_PORT_BOOTSTRAP in t->t_port_space.  MACH_E_DEAD before
 * bootstrap_init.  Call once per task, right after
 * port_install_task_self: the right must land at name 2, the next free
 * one, and any other name panics.
 */
int			 port_install_bootstrap(struct task *);

/* Introspection and debugging. */
void			 port_space_print(struct port_space *);
size_t			 port_space_inuse(struct port_space *);
size_t			 port_queue_len(struct port_space *,
			    mach_port_name_t);
const char		*mach_msg_strerror(int code);

/*
 * Write one mach_port_snapshot_entry per populated name in `ps`, up to
 * `max_entries`, and return the count (0: the space is empty).  Backs
 * the `lsmp` tool.  The table is walked under ps_lock, and each object's
 * counters are copied under its own lock, so a row is consistent but
 * counters can move between rows.  Dead names are not reported.
 */
size_t			 port_space_snapshot(struct port_space *ps,
			    struct mach_port_snapshot_entry *out,
			    size_t max_entries);

/*
 * Boot selftest: a port's waiter list is empty once every waiter has
 * left, by timeout, by a park that never slept, by dying on the list, or
 * by being killed mid-park.  Each case is arranged, not raced for.
 */
void			 port_wait_selftest(void);

/*
 * Boot selftest: a full queue times a sender out without taking its
 * rights, lets send-once, notification and kernel-reply messages past,
 * admits a parked sender when a slot frees, and gives back all a sender
 * killed mid-wait held.
 */
void			 port_send_selftest(void);

/*
 * Boot selftest: a receive right destroyed with PORT_DESTROYED armed --
 * deallocated, or with its task -- reaches the notify port with the
 * queue and the senders' names intact; once only; and a port whose
 * notify port is dead dies as before.
 */
void			 port_destroyed_selftest(void);

#endif /* !_SYS_PORT_H_ */
