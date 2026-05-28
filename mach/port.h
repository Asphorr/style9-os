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
 * A *port* is a kernel object with an FIFO of in-flight messages, a
 * single optional RECEIVE right, and any number of SEND rights.  A
 * *port space* is a per-task table that maps integer NAMES to port
 * rights -- names are local to a space, so task A's name 5 and task
 * B's name 5 may refer to completely different ports.  Until tasks
 * exist there is just one global namespace (kernel_space).
 *
 * The header wire format is taken verbatim from Mach (the same layout
 * userspace receives through SYS_MSG_*), so kernel-internal message
 * passing already uses the ring-3 syscall ABI.
 *
 * Wire structs below are ABI-stable: existing fields keep their offsets,
 * new fields append, and the size is pinned by _Static_assert.  Reordering
 * an existing field is forbidden -- any consumer compiled against an
 * older layout (lib/style9_mach.o, future ring-3 mig stubs) silently
 * misparses the field it thought it was reading.  Marked with WIRE FORMAT
 * banners individually for grep-ability.
 *
 *   bits  24-byte mach_msg_header_t
 *	   { msgh_bits, msgh_size, msgh_remote, msgh_local, voucher, id }
 *   if MACH_MSGH_BITS_COMPLEX:
 *     4-byte body { ndescs }
 *     descriptor area -- ndescs entries, each one of:
 *	   8-byte  port_descriptor  { type=0, disposition, name }
 *	   16-byte ool_descriptor   { type=1, copy, size, address }
 *     followed by inline payload up to msgh_size bytes total.
 *
 * Descriptor area is variable-stride.  The first byte of every
 * descriptor is the `type` tag; the parser reads it, dispatches by
 * size (8 or 16), and advances.  Type-first layout keeps mixed port
 * + OOL messages well-defined without a separate types[] table.
 *
 * Capability passing: every port descriptor moves or copies one right
 * from the sender's space into the receiver's space.  The kernel
 * does the name -> port -> name translation on send and recv
 * respectively; messages in flight hold port refs directly.
 *
 * Bulk-data passing: every OOL descriptor names a sender VA range.
 * The kernel copies the bytes into a kmalloc'd staging buffer on
 * send; on recv it allocates fresh frames in the receiver, copies
 * the bytes in, installs them into the receiver's pmap, records the
 * landing in the receiver's vm_map, and patches the descriptor's
 * `address` field to the new receiver VA.  No VM-shared semantics
 * yet -- v1 OOL is physical copy only.
 *
 * No blocking yet (no scheduler).  port_send is non-blocking and
 * returns -E_NOSPACE if the destination queue is full; port_recv
 * returns -E_NOMSG if the queue is empty.
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
 * Bits the kernel currently consumes in msgh_bits: the two 8-bit
 * disposition lanes (0..15) plus the COMPLEX flag (31).  Anything
 * outside this mask is reserved for future revisions and MUST be
 * zero on send.  mach_msg_send rejects nonzero reserved bits with
 * MACH_E_INVAL so forward-incompatible callers fail loudly the
 * moment they touch a bit that has not been spec'd yet.
 */
#define	MACH_MSGH_BITS_USED_MASK	\
	((uint32_t)MACH_MSGH_BITS_COMPLEX | (uint32_t)0xFFFFu)

/*
 * Dispositions (what the kernel does with the right named in the slot).
 * Numbers match real Mach so the wire format is forward-compatible.
 */
#define	MACH_MSG_TYPE_MOVE_RECEIVE	16	/* sender transfers RECV      */
#define	MACH_MSG_TYPE_MOVE_SEND		17	/* sender transfers SEND      */
#define	MACH_MSG_TYPE_MOVE_SEND_ONCE	18	/* sender transfers SEND_ONCE */
#define	MACH_MSG_TYPE_COPY_SEND		19	/* sender keeps SEND          */
#define	MACH_MSG_TYPE_MAKE_SEND		20	/* sender holds RECV -> SEND  */
#define	MACH_MSG_TYPE_MAKE_SEND_ONCE	21	/* sender holds RECV -> SO    */

/*
 * Notifications.
 *
 * A port's receiver may register a "notify port" for state-change
 * events via mach_port_request_notification.  When the event fires the
 * kernel synthesises a Mach message with msgh_id = MACH_NOTIFY_<EVENT>
 * and posts it to the registered notify port.  Notifications are
 * one-shot: firing consumes the registration; re-arming requires a
 * fresh request.  Numbers match real Mach (MACH_NOTIFY_FIRST = 64).
 *
 *	MACH_NOTIFY_NO_SENDERS	last send-bearing right on the source
 *				port has been dropped while the
 *				receiver still holds RECEIVE.  Used by
 *				services to detect "client has gone
 *				away" without polling.  v1.
 *
 *	MACH_NOTIFY_DEAD_NAME	(v2, reserved) the port behind a name
 *				in this space has died, so the name now
 *				refers to MACH_PORT_DEAD.
 */
#define	MACH_NOTIFY_FIRST		64
#define	MACH_NOTIFY_NO_SENDERS		(MACH_NOTIFY_FIRST + 6)
#define	MACH_NOTIFY_DEAD_NAME		(MACH_NOTIFY_FIRST + 8)

/*
 * Exception messages.  When a ring-3 fault retires a thread the kernel
 * posts a mach_exception_header onto the task's t_exc_port (set via
 * SYS_TASK_SET_EXC_PORT) before the thread is finally killed.  The
 * receiver -- typically a debugger or crash reporter in a separate
 * task -- sees msgh_id = MACH_EXC_FAULT and reads thread state out of
 * the body to decide what to do.  Number chosen above the notify
 * range so opcodes never collide.
 */
#define	MACH_EXC_FAULT			100

/*
 * Exception type indices.  user_fault_die maps the x86 trap vector down
 * to one of these and dispatches to the corresponding slot in the
 * task's t_exc_ports[] array.  Numbered as small integers so they
 * index the array directly; EXC_MASK_* bitmask form is what
 * task_set_exception_ports / SYS_TASK_SET_EXC_PORTS use to address
 * multiple slots in one call.
 *
 *	BAD_ACCESS       #PF, #GP, #SS, #NP and any other fault not
 *	                 otherwise classified (the default bucket)
 *	BAD_INSTRUCTION  #UD, #NM (illegal opcode / device-not-available)
 *	ARITHMETIC       #DE, #MF, #XM (divide, x87, SSE)
 *	BREAKPOINT       #BP (INT3) -- the debugger bucket
 *
 * Setting a port for a type that is never triggered by the current
 * trapno mapping is legal; the slot simply never fires.  New trap
 * vectors are added by extending the switch in exc_type_from_trapno.
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
 * Exception-port behavior flags.  Packed into the high half of the
 * mask argument to SYS_TASK_SET_EXC_PORTS so a single syscall covers
 * both type selection (low bits) and per-task behavior (high bits).
 *
 *	EXC_FLAG_RESUMABLE   opt into the reply protocol.  When set,
 *			    user_fault_die parks the faulting thread on
 *			    a kernel-allocated reply port (named via
 *			    msgh_local in the exception message), waits
 *			    for the watcher to send a mach_exception_reply
 *			    back, and applies the verdict: KILL retires
 *			    the thread (A v1 default), RESUME advances
 *			    tf->tf_rip by er_rip_advance and returns to
 *			    user mode.  No reply within RESUMABLE_TIMEOUT_MS
 *			    is treated as KILL.
 *
 *			    When clear (the default), the kernel retires
 *			    the thread immediately after delivering the
 *			    exception -- the A v1 post-and-forget pattern.
 *
 * Future flags append in the same 16-bit field; types stay in the low
 * 16 bits.  EXC_FLAGS_VALID pins the consumed-flag set so callers
 * setting unknown bits fail loudly with MACH_E_INVAL.
 */
#define	EXC_FLAG_RESUMABLE		0x00010000u
#define	EXC_FLAGS_VALID			EXC_FLAG_RESUMABLE

/*
 * Exception reply opcode (msgh_id) + verdict codes that ride in the
 * mach_exception_reply body.  RESUME advances RIP by er_rip_advance
 * and returns to user mode; KILL retires the thread; STEP is reserved
 * for v3 (single-instruction trap re-arming via RFLAGS.TF).
 */
#define	MACH_EXC_REPLY			101

#define	EXC_VERDICT_KILL		0u
#define	EXC_VERDICT_RESUME		1u
#define	EXC_VERDICT_STEP		2u

/*
 * How long user_fault_die parks a resumable thread waiting for a
 * verdict.  Bounded so a misbehaving / crashed watcher can't pin a
 * faulting thread forever; on expiry the kernel falls back to KILL.
 */
#define	EXC_REPLY_TIMEOUT_MS		500u

/*
 * Descriptor type tags.  The first byte of every descriptor in the
 * message body is one of these.  Sizes (used by the parser to advance
 * to the next descriptor):
 *	PORT_DESCRIPTOR	 8 bytes
 *	OOL_DESCRIPTOR	16 bytes
 */
#define	MACH_MSG_PORT_DESCRIPTOR	0
#define	MACH_MSG_OOL_DESCRIPTOR		1

/*
 * Copy semantics for OOL descriptors.  Real Mach also defines
 * VIRTUAL_COPY (zero) which uses VM remapping / COW; v1 only
 * implements PHYSICAL_COPY (memcpy into fresh frames).  A virtual-
 * copy descriptor is upgraded to a physical copy at send time.
 */
#define	MACH_MSG_VIRTUAL_COPY		0
#define	MACH_MSG_PHYSICAL_COPY		1

/*
 * Upper bound on a single OOL descriptor's size (bytes).  Lets the
 * kernel cap a misbehaving sender before kmalloc starts shovelling
 * megabytes for an attacker.  Raise as needed; one page is the floor.
 */
#define	MACH_MSG_OOL_MAX_BYTES		(1u << 20)	/* 1 MiB           */

/* WIRE FORMAT.  ABI-stable. */
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

/*
 * Port-right descriptor.  Eight bytes; type-tag at byte 0 so the
 * variable-stride descriptor walker can dispatch generically.  Field
 * names match the v1 layout so all existing senders compile without
 * change -- only the in-memory layout differs.
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_msg_port_descriptor {
	uint8_t			type;		/* == MACH_MSG_PORT_DESCRIPTOR */
	uint8_t			disposition;
	uint8_t			pad1;
	uint8_t			pad2;
	mach_port_name_t	name;
};

/*
 * Bulk-memory descriptor.  Sixteen bytes; type-tag at byte 0.  On
 * send `address` is a sender VA; on recv the same field is rewritten
 * to a freshly-mapped receiver VA pointing at the copied bytes.
 *
 * `deallocate` is honoured: set 1 to ask the kernel to vm_map_release
 * the sender's source range after the copy is captured.  Best-effort
 * -- if the range does not mirror a single vm_allocate'd anonymous
 * entry exactly, the flag is silently ignored.  Skipped for
 * kernel-internal senders (kernel_task / trusted-send) since those
 * addresses are kernel-VA, not user-managed.
 *
 * Packed because the natural alignment of uint64_t forces the whole
 * struct to align(8), which then inserts a 4-byte gap between the
 * 4-byte mach_msg_body and the first OOL descriptor in a containing
 * struct.  Those zero bytes look exactly like a bogus port descriptor
 * to the kernel's variable-stride walker (byte 0 = 0 = PORT type),
 * so the send rejects with E_INVAL.  Packed removes the alignment
 * requirement; x86_64 absorbs the uint64_t misaligned read.
 */
/* WIRE FORMAT.  ABI-stable. */
struct mach_msg_ool_descriptor {
	uint8_t			type;		/* == MACH_MSG_OOL_DESCRIPTOR */
	uint8_t			copy;		/* MACH_MSG_PHYSICAL_COPY     */
	uint8_t			deallocate;	/* 1: vm_release source post-copy */
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
 * Wire layout of a notification message posted by the kernel to a
 * registered notify port.  msgh_id carries the MACH_NOTIFY_* opcode;
 * nh_msgid carries the user-supplied tag handed to
 * mach_port_request_notification so the receiver can disambiguate when
 * multiple sources share one notify port.
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
 * Wire layout of an exception message posted by the kernel when a
 * ring-3 thread retires on a fault.  msgh_id is MACH_EXC_FAULT.  All
 * register fields are sampled from the trapframe at fault time; cr2
 * is the faulting VA for #PF and 0 otherwise.  task_id lets a
 * receiver distinguish which child crashed when serving many faults.
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
 * Wire layout of the verdict the watcher sends back when RESUMABLE
 * was set.  msgh_id MUST be MACH_EXC_REPLY; the kernel parses the
 * body and applies er_verdict -- unrecognised verdicts and any
 * msgh_id mismatch are treated as KILL.  er_rip_advance is honored
 * only for RESUME and carries the byte count to skip past the
 * faulting instruction (2 for ud2, 1 for INT3, ...).
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
 * Snapshot of one populated slot in a port_space.  Returned in
 * arrays by SYS_TASK_GET_PORT_SNAPSHOT.  Wire layout is ABI-stable;
 * future kernel revisions may append new fields but never reorder.
 *
 *	mpse_name		integer name in the snapshotted space.
 *	mpse_kind		PORT_SNAPSHOT_KIND_* selector for which
 *				per-object fields carry valid data:
 *	  PORT			mpse_object_id is p_id, mpse_qlen/qmax/
 *				refs/send_count/send_once_count populated,
 *				mpse_member_count = 0.
 *	  SET			mpse_object_id is ps_id, mpse_member_count
 *				populated, queue + refs fields zeroed.
 *	mpse_rights		MACH_PORT_RIGHT_* mask carried by this name
 *				entry.
 *	mpse_special		PORT_SPECIAL_* tag of the underlying port
 *				(NONE for ordinary ports + every port_set).
 *	mpse_flags		PORT_SNAPSHOT_FLAG_* -- bit 0 set when the
 *				underlying port has gone dead.
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
 * Cap on entries the kernel writes in one snapshot syscall.  Sized
 * vs typical per-task usage (task_self + bootstrap + parent + a
 * handful of alloc'd ports + per-type exception ports); keeps the
 * kernel-side staging buffer on stack.
 */
#define	MACH_PORT_SNAPSHOT_MAX		64

/* Error returns from mach_msg_send / mach_msg_recv (negative). */
#define	MACH_MSG_OK		0
#define	MACH_E_INVAL		1
#define	MACH_E_NAME		2	/* name not in space               */
#define	MACH_E_RIGHT		3	/* name in space but wrong right   */
#define	MACH_E_DEAD		4	/* port is dead                    */
#define	MACH_E_NOSPACE		5	/* queue full / table full         */
#define	MACH_E_NOMSG		6	/* recv on empty queue             */
#define	MACH_E_TOOSMALL		7	/* caller's recv buf too small     */
#define	MACH_E_NOMEM		8	/* kmalloc failed                  */
#define	MACH_E_TIMEOUT		9	/* mach_msg_recv_timed deadline    */

/*
 * Special timeout values for mach_msg_recv_timed.
 *	NONE	 -- non-blocking poll; returns MACH_E_NOMSG if empty
 *	FOREVER	 -- behaviour identical to mach_msg_recv_block
 */
#define	MACH_TIMEOUT_NONE	((uint64_t)0)
#define	MACH_TIMEOUT_FOREVER	((uint64_t)~0ull)

/*
 * "Special" ports are kernel-implemented Mach objects.  A message sent
 * to a port whose p_special tag is non-zero is dispatched synchronously
 * in the send path -- there is no server thread, no queueing, and the
 * dispatcher itself is responsible for any reply via msgh_local.  This
 * is the same pattern real Mach uses for mach_task_self / mach_host_self
 * etc., where the kernel object behind the SEND is the kernel itself.
 *
 * Tag values:
 *	NONE		regular Mach port -- normal send/recv semantics
 *	TASK_SELF	p_special_arg is (struct task *), the target task
 *	BOOTSTRAP	the single global service registry; arg is unused
 *	SERVICE		p_special_arg is a (port_service_fn) -- generic
 *			kernel-side service hook, used by clock/stats/tasks
 */
#define	PORT_SPECIAL_NONE		0
#define	PORT_SPECIAL_TASK_SELF		1
#define	PORT_SPECIAL_BOOTSTRAP		2
#define	PORT_SPECIAL_SERVICE		3

struct port_space;	/* fwd decl for the typedef below */

/*
 * Signature of a PORT_SPECIAL_SERVICE dispatcher.  Stashed by the port
 * creator in p_special_arg; called from mach_msg_send's intercept with
 * the incoming request header + the caller's port_space.  Implementations
 * synthesize a reply and send it to req->msgh_local; the return value
 * propagates back to the original sender as the mach_msg_send result.
 */
typedef int (*port_service_fn)(const struct mach_msg_header *req,
		struct port_space *from);

/*
 * Every task's port_space carries a SEND right to its own task_self
 * port at this well-known name (set up by task_create) plus a SEND
 * right to the global bootstrap port at the next slot.  Both are
 * already populated by the time the task's first thread runs.
 */
#define	MACH_PORT_TASK_SELF		((mach_port_name_t)1)
#define	MACH_PORT_BOOTSTRAP		((mach_port_name_t)2)

/*
 * Well-known slot for a port the parent task injected at spawn time
 * via SYS_SPAWN_WITH_PORT.  Tasks spawned without an injection have an
 * empty slot here; the child program detects that by attempting a
 * mach_msg_send and observing MACH_E_RIGHT.  The slot is by
 * construction the next-free name after TASK_SELF and BOOTSTRAP, so
 * arch_spawn_user's install KASSERTs the chosen name == 3.
 */
#define	MACH_PORT_PARENT		((mach_port_name_t)3)

/*
 * Op codes used as msgh_id on messages sent to MACH_PORT_TASK_SELF.
 * Reply layouts are pinned by _Static_assert in this header.
 */
#define	TASK_OP_GET_INFO		1

/*
 * Reply payload for TASK_OP_GET_INFO.  Sits right after the
 * mach_msg_header in the reply message.
 */
/* WIRE FORMAT.  ABI-stable. */
struct task_info_reply {
	uint64_t	tir_task_id;
	uint32_t	tir_nthreads;
	uint32_t	tir_pad;
	char		tir_name[32];
};

_Static_assert(sizeof(struct task_info_reply) == 48,
    "task_info_reply must be 48 bytes (wire format)");

/* Opaque kernel objects. */
struct port;
struct port_space;
struct task;

/* The kernel's own port name space. */
extern struct port_space	*kernel_space;

/* Subsystem bring-up; safe to call once kmem is initialised. */
void	port_subsystem_init(void);

/*
 * Space lifecycle.  port_space_new creates an empty name table;
 * port_space_destroy drops every right it still holds (calling into
 * the port-side ref counts).  Tasks will own one space each.
 */
struct port_space	*port_space_new(void);
void			 port_space_destroy(struct port_space *);

/*
 * port_allocate: create a fresh port and grant the calling space the
 * requested combination of rights.  Typical use is
 *	mach_port_name_t n = port_allocate(space,
 *	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
 * which yields a name suitable for both servicing and self-send.
 *
 * Returns MACH_PORT_NULL on failure.
 */
mach_port_name_t	 port_allocate(struct port_space *, uint8_t right_mask);

/*
 * Drop one entry from `space`.  All rights in the entry are released
 * at once; if the receive right was the last reference the port is
 * destroyed and its queue is drained.
 */
int			 port_deallocate(struct port_space *,
			    mach_port_name_t);

/*
 * Port sets: aggregate multiple ports' receive queues behind one
 * name, so a single thread can serve many endpoints via one recv.
 *
 *	port_set_allocate -- new set with RECEIVE-style right in `space`
 *	port_set_insert   -- move `port_name`'s port into `set_name`'s set
 *	port_set_remove   -- detach a port back to standalone
 *
 * A port can be a member of at most one set; the set itself has no
 * SEND right (you can't send to a port set, only recv).  When a
 * message arrives at any member, recv on the set delivers it.
 */
mach_port_name_t	 port_set_allocate(struct port_space *);
int			 port_set_insert(struct port_space *,
			    mach_port_name_t set_name,
			    mach_port_name_t port_name);
int			 port_set_remove(struct port_space *,
			    mach_port_name_t set_name,
			    mach_port_name_t port_name);

/*
 * Read-only introspection: return the name of the port set `port_name`
 * currently belongs to (in `space`), or MACH_PORT_NULL when the port
 * is standalone.  Symmetric to insert/remove; useful for cleanup loops
 * that need to find a port's set without tracking it externally.
 *
 * Assumes set membership stays within one port_space -- the kernel
 * exposes no API to share a port_set across spaces today.
 */
mach_port_name_t	 port_set_extract(struct port_space *space,
			    mach_port_name_t port_name);

/*
 * Drop ONE specific right kind from a name without taking down the
 * other rights held under the same name (and without destroying the
 * port).  Returns MACH_E_NAME if `name` does not currently carry
 * `right`.  Used by stress tests that need to model "last sender went
 * away while receiver is still parked" without spawning extra spaces
 * just to split rights across them.
 */
int			 port_mod_refs(struct port_space *,
			    mach_port_name_t name, uint8_t right);

/*
 * mach_port_request_notification: arrange for the kernel to post a
 * notification message to `notify_port_name` when the source port at
 * `name` reaches the event named by `notify_type` (one of MACH_NOTIFY_*).
 *
 * The caller must hold RECEIVE on `name` (only the receiver may register
 * notifications) and SEND on `notify_port_name`.  `notify_msgid` is a
 * user-chosen tag the kernel will copy verbatim into the notification
 * message's nh_msgid field so callers serving many ports through one
 * notify port can distinguish them.
 *
 * If a notification of this type was already registered on `name`, the
 * previous notify-port name is returned via *prev_out so the caller can
 * mach_port_deallocate it; MACH_PORT_NULL is written otherwise.  The
 * kernel takes a SEND ref on the new notify port and drops it when the
 * notification fires (one-shot) or when the source port's RECV right
 * is finally released.
 *
 * v1 supports only MACH_NOTIFY_NO_SENDERS; other types return MACH_E_INVAL.
 */
int			 port_request_notification(struct port_space *space,
			    mach_port_name_t name, uint32_t notify_type,
			    mach_port_name_t notify_port_name,
			    uint32_t notify_msgid,
			    mach_port_name_t *prev_out);

/*
 * port_arm_dead_name_object: object-level DEAD_NAME arming for in-kernel
 * watchers that already hold port pointers (no name resolution / rights
 * check).  Posts MACH_NOTIFY_DEAD_NAME carrying `tag` in nh_msgid onto
 * `notify` when `watched`'s RECEIVE right is released.  Takes a SEND ref
 * on `notify` released on fire (one-shot).  MACH_E_DEAD if `watched` is
 * already dead.  Drives the launchd keep_alive worker.
 */
int			 port_arm_dead_name_object(struct port *watched,
			    struct port *notify, uint32_t tag);

/*
 * port_exception_post: kernel-side primitive used by the trap
 * dispatcher to deliver a MACH_EXC_FAULT message onto a task's
 * exception port.  Caller holds one SEND ref on `port` (typically
 * already in task->t_exc_ports[..]); this routine does not deref.
 * Individual fields rather than a trapframe pointer so mach/ stays
 * free of arch/ dependencies.  Best-effort: a dead port or full
 * queue silently drops.
 *
 * `reply_port` is optional: when non-NULL the message carries an
 * implicit msgh_local descriptor minting a SEND right on `reply_port`
 * into the watcher's space (msgh_bits LOCAL_disp = MAKE_SEND).  The
 * watcher recvs the exception, reads msgh_local, and sends back a
 * mach_exception_reply through that name -- the kernel parks the
 * faulting thread on `reply_port`'s recv queue until the verdict
 * arrives.  When NULL the message has no implicit local; the watcher
 * is observer-only and the caller retires the thread directly.
 */
int			 port_exception_post(struct port *port,
			    uint32_t trapno, uint32_t err,
			    uint64_t rip, uint64_t rsp, uint64_t rflags,
			    uint64_t cr2, uint64_t task_id,
			    struct port *reply_port);

/*
 * Bootstrap helper for inter-task IPC.
 *
 * Looks up `src_name` in `src` (with SEND right), takes a fresh SEND
 * reference, and installs it as a new name in `dst`.  This is the
 * primitive that lets a parent task hand a child task a way to talk
 * back to it: in real Mach the equivalent happens implicitly via
 * task ports + bootstrap server; the primitive is exposed directly
 * so a parent creating a child can wire up communication before the
 * child runs.
 *
 * Returns MACH_MSG_OK on success and writes the new name to
 * `dst_name_out`.  No effect on `src_name` (it keeps its SEND right).
 */
int			 port_space_inject_send(struct port_space *src,
			    mach_port_name_t src_name,
			    struct port_space *dst,
			    mach_port_name_t *dst_name_out);

/*
 * mach_msg_send: enqueue `msg` onto the port named msgh_remote in
 * `from`, taking refs on any port descriptors per their disposition.
 * The caller's buffer is consumed read-only; the kernel makes its own
 * copy in the queued message.
 */
int			 mach_msg_send(struct port_space *from,
			    const struct mach_msg_header *msg);

/*
 * Like mach_msg_send, but flags the current thread as a trusted kernel
 * sender for the duration of the call.  send_capture_ool then skips its
 * user-VA range validation, permitting kernel rodata addresses in OOL
 * descriptors.  Reserved for kernel-internal services that ship
 * kernel-resident bytes back to a userspace caller (the "man" service
 * is the canonical example).  Untrusted callers MUST use mach_msg_send.
 */
int			 mach_msg_send_trusted(struct port_space *from,
			    const struct mach_msg_header *msg);

/*
 * mach_msg_recv: dequeue the next message from the port named
 * `recv_name` (which must carry RECEIVE right in `to`) and copy it
 * into `buf` (capacity `buf_size`).  Translates port descriptors
 * (and msgh_local) into names in `to`.
 *
 * Returns MACH_MSG_OK on success; if MACH_E_NOMSG, the port is
 * empty.  If MACH_E_TOOSMALL, the message is left in the queue.
 */
int			 mach_msg_recv(struct port_space *to,
			    mach_port_name_t recv_name,
			    struct mach_msg_header *buf, size_t buf_size);

/*
 * Blocking variant: if the port's queue is empty, sleep until a
 * message arrives (or the port becomes dead, in which case
 * MACH_E_DEAD is returned).  Requires a running scheduler.
 */
int			 mach_msg_recv_block(struct port_space *to,
			    mach_port_name_t recv_name,
			    struct mach_msg_header *buf, size_t buf_size);

/*
 * mach_msg_recv_timed: like recv_block but bounds the wait by
 * `timeout_ms` measured in milliseconds against clock_uptime_ms().
 *	MACH_TIMEOUT_NONE	non-blocking poll
 *	MACH_TIMEOUT_FOREVER	wait indefinitely (== recv_block)
 *	any other value		return MACH_E_TIMEOUT when deadline elapses
 *
 * On timeout the caller's buf is untouched; on success it carries the
 * dequeued message exactly as for recv_block.
 */
int			 mach_msg_recv_timed(struct port_space *to,
			    mach_port_name_t recv_name,
			    struct mach_msg_header *buf, size_t buf_size,
			    uint64_t timeout_ms);

/*
 * mach_msg_rpc: send `req` to its destination, then wait for a single
 * reply on a freshly-allocated reply port.  The reply port is created
 * inside `space`, encoded into `req->msgh_local` with disposition
 * MAKE_SEND so the server receives a SEND right to talk back through,
 * and torn down before return regardless of outcome.
 *
 * `reply_buf` (capacity `reply_buf_size`) receives the reply message.
 * `timeout_ms` bounds the wait the same way mach_msg_recv_timed does.
 *
 * On any failure (send error, recv error, timeout) the reply port is
 * deallocated before this function returns, so the caller never has to
 * clean it up on the error path.
 */
int			 mach_msg_rpc(struct port_space *space,
			    struct mach_msg_header *req,
			    struct mach_msg_header *reply_buf,
			    size_t reply_buf_size,
			    uint64_t timeout_ms);

/*
 * Allocate the task_self port for `t`, install a SEND right at
 * MACH_PORT_TASK_SELF in t->t_port_space, and tag the port as
 * PORT_SPECIAL_TASK_SELF so subsequent sends to that name dispatch
 * synchronously to task_self_dispatch.  Idempotent across the kernel
 * bootstrap: call exactly once per task, after t->t_port_space is in
 * its final identity (see task_subsystem_init for the bootstrap order).
 *
 * Returns MACH_MSG_OK on success.  On failure the partial state is
 * cleaned up (no port leak); caller can retry.
 */
int			 port_install_task_self(struct task *);

/*
 * Release the kernel-side RECEIVE right on the task_self port -- the
 * counterpart to port_install_task_self.  Called from the task
 * destruction path after the per-task port_space has dropped its SEND
 * ref so the port can finally reach zero refs and be reclaimed.
 */
void			 port_release_task_self(struct task *);

/*
 * Install a SEND right to the global bootstrap port (see kern/bootstrap.c)
 * at MACH_PORT_BOOTSTRAP in t->t_port_space.  The bootstrap port itself
 * must already exist (bootstrap_init) when this is called.  Idempotent
 * across the call -- if the well-known slot is already populated for
 * this task, no-op return MACH_MSG_OK.
 *
 * Slot ABI: this MUST land at name MACH_PORT_BOOTSTRAP (=2).  Since the
 * function is invoked from task_create right after port_install_task_self
 * (which always lands at name 1), the next-free slot is name 2, which
 * matches.  port_install_bootstrap panics otherwise.
 */
int			 port_install_bootstrap(struct task *);

/*
 * Introspection / debugging.
 */
void			 port_space_print(struct port_space *);
size_t			 port_space_inuse(struct port_space *);
size_t			 port_queue_len(struct port_space *,
			    mach_port_name_t);
const char		*mach_msg_strerror(int code);

/*
 * Snapshot one entry per populated slot in `ps`.  Drives the userspace
 * `lsmp` tool ("list mach ports", same shape as Darwin's debugger
 * surface): the kernel walks the name table under ps->ps_lock, takes
 * each non-empty entry's per-object lock briefly to copy out counters,
 * and writes a wire-format mach_port_snapshot_entry per slot into the
 * caller's array (up to `max_entries`).  Returns the number of entries
 * actually written.  Name 0 is never populated by convention, so 0
 * means the space is empty.
 *
 * Best-effort: counts may shift between adjacent entries since the
 * outer lock is released after each populated row to bound contention.
 * Sized so a typical task (task_self + bootstrap + parent + a handful
 * of alloc'd ports + exception ports) fits within MACH_PORT_SNAPSHOT_MAX.
 */
size_t			 port_space_snapshot(struct port_space *ps,
			    struct mach_port_snapshot_entry *out,
			    size_t max_entries);

#endif /* !_SYS_PORT_H_ */
