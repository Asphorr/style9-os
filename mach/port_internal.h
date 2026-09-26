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
 * Private to mach/: the structures and helpers port_object.c,
 * port_space.c and port_msg.c share.  The public API is port.h.
 *
 * Field annotations: (c) constant after creation, (p) the object's own
 * lock (p_lock, or ps_lock for a set or space).
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
	 * Inline-reply stash (mach_msg_rpc and the send fast path in
	 * port_msg.c), under p_lock.  p_stash_buf is a VA in the arming
	 * thread's address space, so only that thread may fill it.
	 */
	struct mach_msg_header *p_stash_buf;
	struct thread	*p_stash_thread;
	size_t		 p_stash_size;
	int		 p_stash_rv;

	/*
	 * Notification registrations, under p_lock.
	 *
	 *	NO_SENDERS	set by this port's receiver; fires when the
	 *			last SEND / SEND_ONCE right drops while RECEIVE
	 *			is held.  One slot: registering again replaces.
	 *	DEAD_NAME	set by SEND holders; fires when RECEIVE is
	 *			dropped.  A list with one node per notify
	 *			target, so every watcher is told.
	 *
	 * Each registration holds a SEND ref on its notify port until it
	 * fires or RECEIVE drops, released by port_deref.
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
 * One armed DEAD_NAME watch on port->p_notify_dead_name, fired and freed
 * when the port dies.  nn_port holds one SEND ref, released with the
 * node.  One node per nn_port: re-arming a target updates its tag.
 */
struct port_notify_node {
	struct port_notify_node	*nn_next;
	struct port		*nn_port;	/* notify target, holds 1 SEND */
	uint32_t		 nn_tag;	/* user msgid handed back      */
	uint32_t		 nn_pad;
};

/*
 * Kernel state of one descriptor in flight; pd_type says which fields
 * are live.
 *
 *	MACH_MSG_PORT_DESCRIPTOR	pd_port + pd_disposition: the right
 *					(a ref, or RECEIVE itself) taken at
 *					send time.
 *	MACH_MSG_OOL_DESCRIPTOR		pd_ool_pages + pd_ool_npages: the
 *					payload's frames, captured at send
 *					(shared with the sender or copied,
 *					vm/vm.h).
 *
 * Whoever holds the array owns the frames: delivery maps them into the
 * receiver, every other exit releases them.
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
 * Link a DEAD_NAME watch onto `watched`.  The caller passes `notify`
 * with a fresh SEND ref and a pre-allocated node (no kmalloc under
 * p_lock).  A new watch consumes both.  If `notify` is already watching,
 * its tag is updated and *was_dup set: the caller frees the node and
 * drops the ref.  MACH_E_DEAD if `watched` has already died; the caller
 * then releases both too.
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
 * port_msg.c: queue a kernel notification on `notify_port`, msgh_id
 * `notify_id` and nh_msgid `user_tag`.  Best-effort: dropped if the
 * queue is full or the port dead.  The caller keeps the ref it holds.
 */
int		 port_notify_enqueue(struct port *notify_port,
		    uint32_t notify_id, uint32_t user_tag);

#endif /* !_MACH_PORT_INTERNAL_H_ */
