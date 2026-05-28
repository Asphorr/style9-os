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
	void		*p_special_arg;		/* (c) per-tag context ptr   */

	/*
	 * Inline-reply stash (see mach_msg_rpc + the send fast path in
	 * port_msg.c).  All three under p_lock.
	 */
	struct mach_msg_header *p_stash_buf;
	size_t		 p_stash_size;
	int		 p_stash_rv;
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
 * Per-descriptor kernel state while a message is in flight.  Tagged
 * union: pd_type selects which fields are live.
 *
 *	MACH_MSG_PORT_DESCRIPTOR	pd_port + pd_disposition carry the
 *					kernel ref taken at send time.
 *	MACH_MSG_OOL_DESCRIPTOR		pd_ool_buf + pd_ool_size carry the
 *					staging copy of the sender's bytes,
 *					kfree'd in cleanup paths.
 */
struct port_pending_desc {
	uint8_t		 pd_type;
	uint8_t		 pd_disposition;	/* PORT */
	uint8_t		 pd_ool_copy;		/* OOL: MACH_MSG_PHYSICAL_COPY */
	uint8_t		 pd_pad;
	uint32_t	 pd_ool_size;		/* OOL: bytes in pd_ool_buf  */
	struct port	*pd_port;		/* PORT                       */
	void		*pd_ool_buf;		/* OOL: kmalloc'd staging buf */
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
	uint8_t		 pe_pad[3];
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
int		 space_drop_one_right(struct port_space *,
		    mach_port_name_t name, uint8_t right);
int		 space_unbind_no_deref(struct port_space *,
		    mach_port_name_t name, uint8_t right);

#endif /* !_MACH_PORT_INTERNAL_H_ */
