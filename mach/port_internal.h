/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACH_PORT_INTERNAL_H_
#define	_MACH_PORT_INTERNAL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "port.h"
#include "spinlock.h"

/*
 * Internal layout for the Mach IPC subsystem.  This header is private
 * to mach-tree .c files; carries the struct definitions the three
 * sibling .c files (port_object, port_space, port_msg) share plus the
 * cross-file static helper declarations.
 *
 * Public API stays in port.h; nothing outside mach/ should include
 * this file.
 */

/* ---- types ----------------------------------------------------------- */

struct port_set;
struct port_notify_node;

struct port {
	struct spinlock	 p_lock;
	uint64_t	 p_id;			/* (c) printable global id   */
	uint32_t	 p_refs;		/* (p) all refs combined     */
	uint32_t	 p_send_count;		/* (p) send rights extant    */
	uint32_t	 p_send_once_count;	/* (p) send-once outstanding */
	bool		 p_has_receive;		/* (p) receive right exists  */
	bool		 p_dead;		/* (p) no longer deliverable */
	size_t		 p_qlen;		/* (p) messages queued       */
	size_t		 p_qmax;		/* (c) bound on qlen         */
	struct port_msg	*p_qhead;		/* (p) FIFO head             */
	struct port_msg	*p_qtail;		/* (p) FIFO tail             */
	struct thread	*p_waiters_head;	/* (p) threads in recv_block */
	struct thread	*p_waiters_tail;	/* (p)                       */
	struct thread	*p_send_waiters_head;	/* (p) blocked on full queue */
	struct thread	*p_send_waiters_tail;	/* (p)                       */
	struct port_set	*p_set;			/* (p) set this port belongs */
	struct port	*p_set_link;		/* (p) next in set members   */
	uint8_t		 p_special;		/* (c) PORT_SPECIAL_* tag    */
	void		*p_special_arg;		/* (c) SERVICE fn / TASK_SELF id */

	/*
	 * Inline-reply stash (see mach_msg_rpc + the send fast path in
	 * port_msg.c).  All three under p_lock.
	 */
	struct mach_msg_header *p_stash_buf;
	size_t		 p_stash_size;
	int		 p_stash_rv;

	/*
	 * Notification slots.
	 *
	 *	NO_SENDERS	registered by the receiver of THIS port; fires
	 *			when the last send-bearing right drops while
	 *			RECEIVE is still held.  Single-slot (one
	 *			receiver per port): a second registration
	 *			replaces the first.
	 *	DEAD_NAME	registered by SEND holders of THIS port; fires
	 *			when RECEIVE is dropped and the port goes dead
	 *			(each watcher's SEND name in their own space
	 *			becomes a dead name).  Multi-registrant: a
	 *			singly-linked list of p_notify_dead_name nodes,
	 *			one per distinct notify target, so every holder
	 *			that armed a watch is notified -- not just the
	 *			last to register.
	 *
	 * The kernel holds a SEND ref on each notify port to keep it
	 * alive until the notification fires or the source's RECV
	 * drops (whichever first); the NO_SENDERS ref is released through
	 * port_deref's notify-cleanup branch, the DEAD_NAME refs as the
	 * list is walked on death.  All under p_lock.
	 */
	struct port		*p_notify_no_senders;
	uint32_t		 p_notify_no_senders_id;
	struct port_notify_node	*p_notify_dead_name;
};

struct port_set {
	struct spinlock	 ps_lock;
	uint64_t	 ps_id;			/* (c)                       */
	uint32_t	 ps_refs;		/* (p) name-table refs       */
	bool		 ps_dead;		/* (p)                       */
	size_t		 ps_member_count;	/* (p)                       */
	struct port	*ps_members_head;	/* (p) SLL via p_set_link    */
	struct thread	*ps_waiters_head;	/* (p) recv-blocked threads  */
	struct thread	*ps_waiters_tail;	/* (p)                       */
};

/*
 * One armed DEAD_NAME watch.  Linked off port->p_notify_dead_name; the
 * list is built as SEND holders register and walked (fired + freed) when
 * the port dies.  nn_port carries one kernel-held SEND ref on the notify
 * target, released as the node is freed.  Deduped on nn_port so re-arming
 * the same target updates its tag rather than queueing a second message.
 */
struct port_notify_node {
	struct port_notify_node	*nn_next;
	struct port		*nn_port;	/* notify target, holds 1 SEND */
	uint32_t		 nn_tag;	/* user msgid handed back      */
	uint32_t		 nn_pad;
};

/*
 * Per-descriptor kernel state while a message is in flight.  Tagged
 * union: pd_type selects which fields are live.
 *
 *	MACH_MSG_PORT_DESCRIPTOR	pd_port + pd_disposition carry the
 *					kernel ref taken at send time.
 *	MACH_MSG_OOL_DESCRIPTOR		pd_ool_pages + pd_ool_npages carry
 *					the payload's FRAMES, owned by the
 *					message until it is delivered or
 *					destroyed.
 *
 * An in-flight OOL payload used to be a kmalloc'd copy of the sender's bytes,
 * which meant every transfer paid for two: once into the staging buffer on
 * send, once out of it into the receiver's fresh frames on recv.  It is now
 * the frames themselves, captured at send time -- shared with the sender
 * where that is legal, copied where it is not (vm/vm.h) -- so delivery is a
 * page-table operation and the second copy is gone along with the buffer.
 *
 * Whoever holds the array owns the frames.  Delivery hands them to the
 * receiver; every other exit releases them.
 */
struct port_pending_desc {
	uint8_t		 pd_type;
	uint8_t		 pd_disposition;	/* PORT */
	uint8_t		 pd_ool_copy;		/* OOL: MACH_MSG_*_COPY       */
	uint8_t		 pd_pad;
	uint32_t	 pd_ool_size;		/* OOL: payload bytes         */
	struct port	*pd_port;		/* PORT                       */
	uint64_t	*pd_ool_pages;		/* OOL: owned frames          */
	size_t		 pd_ool_npages;		/* OOL: entries above         */
};

struct port_msg {
	struct port_msg		*m_next;
	size_t			 m_size;	/* bytes in m_buf            */
	size_t			 m_ndescs;
	struct port_pending_desc *m_descs;	/* NULL when ndescs == 0     */
	uint8_t			 m_buf[];	/* raw msg, header + body    */
};

struct port_entry {
	struct port	*pe_port;	/* non-NULL when this is a port    */
	struct port_set	*pe_set;	/* non-NULL when this is a set     */
	uint8_t		 pe_rights;	/* MACH_PORT_RIGHT_* mask          */
	uint8_t		 pe_dead;	/* dead-name tombstone (port died) */
	uint8_t		 pe_pad[2];
};

struct port_space {
	struct spinlock	 ps_lock;
	uint64_t	 ps_id;			/* (c) printable space id     */
	struct port_entry *ps_table;		/* (p) dynamic, kmalloc'd     */
	size_t		 ps_capacity;		/* (p) slots in ps_table      */
	size_t		 ps_inuse;		/* (p) populated entries      */
	mach_port_name_t ps_hint;		/* (p) next-fit search start  */
};

/* ---- tunables -------------------------------------------------------- */

#define	DEFAULT_QMAX		1024
#define	INITIAL_SPACE_CAP	16
#define	MAX_MSG_BYTES		4096

/* ---- shared module state -------------------------------------------- */

extern uint64_t		 next_port_id;	/* (port_global_lock) */
extern uint64_t		 next_space_id;	/* (port_global_lock) */
extern struct spinlock	 port_global_lock;

/* ---- cross-file helpers --------------------------------------------- */

/* port_object.c */
struct port	*port_create(void);
void		 port_free(struct port *);
void		 port_ref(struct port *, uint8_t rights);
void		 port_deref(struct port *, uint8_t rights);

/*
 * Link a DEAD_NAME watcher onto `watched`.  Caller hands in a port that
 * already carries one fresh SEND ref (`notify`) and a pre-allocated node
 * (so no kmalloc happens under p_lock).  On a fresh registration the node
 * and ref are consumed; if a node for the same target already exists its
 * tag is updated and *was_dup is set true so the caller frees the spare
 * node and drops the spare ref.  Returns MACH_E_DEAD if `watched` has
 * already died (the event can no longer be observed).
 */
int		 port_dead_name_link(struct port *watched, struct port *notify,
		    struct port_notify_node *node, uint32_t tag,
		    bool *was_dup);

struct port_set	*port_set_create(void);
void		 port_set_free(struct port_set *);
void		 port_set_ref(struct port_set *);
void		 port_set_deref(struct port_set *);

/* port_space.c */
int		 space_install(struct port_space *, struct port *p,
		    uint8_t rights, mach_port_name_t *name_out);
int		 space_install_no_ref(struct port_space *, struct port *p,
		    uint8_t rights, mach_port_name_t *name_out);
struct port	*space_lookup(struct port_space *, mach_port_name_t name,
		    uint8_t need_right, uint8_t *rights_out);
struct port_set	*space_lookup_set(struct port_space *, mach_port_name_t);
bool		 space_name_is_dead(struct port_space *, mach_port_name_t);
int		 space_drop_one_right(struct port_space *,
		    mach_port_name_t name, uint8_t right);
int		 space_unbind_no_deref(struct port_space *,
		    mach_port_name_t name, uint8_t right);

/*
 * port_msg.c -- synthesise + enqueue a kernel-originated notification
 * message on `notify_port`.  Caller passes in the opcode (msgh_id) and
 * the user tag the receiver gets in nh_msgid.  Best-effort: dropped if
 * the queue is full or the port is dead.  Caller still owns the SEND
 * ref it held -- this routine does not deref.
 */
int		 port_notify_enqueue(struct port *notify_port,
		    uint32_t notify_id, uint32_t user_tag);

#endif /* !_MACH_PORT_INTERNAL_H_ */
