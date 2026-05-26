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
 * userspace will eventually see), so kernel-internal message passing
 * already uses the future syscall ABI.
 *
 *   bits  24-byte mach_msg_header_t
 *	   { msgh_bits, msgh_size, msgh_remote, msgh_local, voucher, id }
 *   if MACH_MSGH_BITS_COMPLEX:
 *     4-byte body { ndescs }
 *     ndescs * 8-byte port_descriptor { name, disposition, type, pad }
 *   followed by inline payload up to msgh_size bytes total.
 *
 * Capability passing: every port descriptor in a message moves or
 * copies one right from the sender's space into the receiver's space.
 * The kernel does the name -> port -> name translation on send and
 * recv respectively; messages in flight hold port refs directly.
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
 * Dispositions (what the kernel does with the right named in the slot).
 * Numbers match real Mach so the wire format is forward-compatible.
 */
#define	MACH_MSG_TYPE_MOVE_RECEIVE	16	/* sender transfers RECV      */
#define	MACH_MSG_TYPE_MOVE_SEND		17	/* sender transfers SEND      */
#define	MACH_MSG_TYPE_MOVE_SEND_ONCE	18	/* sender transfers SEND_ONCE */
#define	MACH_MSG_TYPE_COPY_SEND		19	/* sender keeps SEND          */
#define	MACH_MSG_TYPE_MAKE_SEND		20	/* sender holds RECV -> SEND  */
#define	MACH_MSG_TYPE_MAKE_SEND_ONCE	21	/* sender holds RECV -> SO    */

/* Descriptor type tag (only port descriptors supported). */
#define	MACH_MSG_PORT_DESCRIPTOR	0

struct mach_msg_header {
	uint32_t		msgh_bits;
	uint32_t		msgh_size;	/* total bytes incl header */
	mach_port_name_t	msgh_remote;	/* dest port name (sender NS) */
	mach_port_name_t	msgh_local;	/* reply port name (sender NS) */
	uint32_t		msgh_voucher;	/* reserved, set 0          */
	uint32_t		msgh_id;	/* caller's protocol id     */
};

struct mach_msg_body {
	uint32_t		msgh_descriptor_count;
};

struct mach_msg_port_descriptor {
	mach_port_name_t	name;
	uint8_t			pad1;
	uint8_t			disposition;
	uint8_t			type;
	uint8_t			pad2;
};

_Static_assert(sizeof(struct mach_msg_header) == 24,
    "mach_msg_header must be 24 bytes");
_Static_assert(sizeof(struct mach_msg_port_descriptor) == 8,
    "mach_msg_port_descriptor must be 8 bytes");

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
 */
#define	PORT_SPECIAL_NONE		0
#define	PORT_SPECIAL_TASK_SELF		1

/*
 * Every task's port_space carries a SEND right to its own task_self
 * port at this well-known name.  Set up by task_create so the slot is
 * already populated by the time the first thread runs.
 */
#define	MACH_PORT_TASK_SELF		((mach_port_name_t)1)

/*
 * Op codes used as msgh_id on messages sent to MACH_PORT_TASK_SELF.
 * Reply layouts are pinned by _Static_assert in this header.
 */
#define	TASK_OP_GET_INFO		1

/*
 * Reply payload for TASK_OP_GET_INFO.  Sits right after the
 * mach_msg_header in the reply message.
 */
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
 * Bootstrap helper for inter-task IPC.
 *
 * Looks up `src_name` in `src` (with SEND right), takes a fresh SEND
 * reference, and installs it as a new name in `dst`.  This is the
 * primitive that lets a parent task hand a child task a way to talk
 * back to it: in real Mach the equivalent happens implicitly via
 * task ports + bootstrap server; we expose it directly so a parent
 * creating a child can wire up communication before the child runs.
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
 * Introspection / debugging.
 */
void			 port_space_print(struct port_space *);
size_t			 port_space_inuse(struct port_space *);
size_t			 port_queue_len(struct port_space *,
			    mach_port_name_t);
const char		*mach_msg_strerror(int code);

#endif /* !_SYS_PORT_H_ */
