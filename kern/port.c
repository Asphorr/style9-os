/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "sched.h"
#include "spinlock.h"
#include "thread.h"

/* ---- private types ----------------------------------------------------- */

/*
 * Port object.  Lock key:
 *	(a) atomic; mutated with __atomic primitives, may be read lockless
 *	(p) protected by p_lock
 *	(c) const after port_create
 */
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
};

/*
 * Port set: aggregates many ports' RECEIVE ends.  A name in a space
 * carrying PORT_SET right resolves to one of these.  recv-on-set
 * scans members for a non-empty queue; sends targeting a port that
 * is a member wake the set's waiters instead of the port's.
 */
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
 * Pending descriptor: the kernel's view of an in-flight port
 * descriptor inside a queued message.  Holds the actual port pointer
 * and the right that will be granted to the receiver.  On recv, the
 * kernel allocates a name in the receiver's space, fills in the
 * descriptor's name field in the message buffer, and forgets about
 * this entry.
 */
struct port_pending_desc {
	struct port	*pd_port;
	uint8_t		 pd_disposition;
};

/*
 * Queued message.  Two allocations: this struct (with the raw m_buf
 * flexible array trailing the header), plus m_descs (allocated only
 * when ndescs > 0).  Keeps the hot path -- queue head pointer -- in
 * one cache line.
 */
struct port_msg {
	struct port_msg		*m_next;
	size_t			 m_size;	/* bytes in m_buf            */
	size_t			 m_ndescs;
	struct port_pending_desc *m_descs;	/* NULL when ndescs == 0     */
	uint8_t			 m_buf[];	/* raw msg, header + body    */
};

/*
 * Per-space name table entry.  pe_port == NULL marks an empty slot.
 */
struct port_entry {
	struct port	*pe_port;	/* non-NULL when this is a port    */
	struct port_set	*pe_set;	/* non-NULL when this is a set     */
	uint8_t		 pe_rights;	/* MACH_PORT_RIGHT_* mask          */
	uint8_t		 pe_pad[3];
};

/*
 * Port space (per-task name table).  Lock key:
 *	(p) protected by ps_lock
 *	(c) const after port_space_new
 */
struct port_space {
	struct spinlock	 ps_lock;
	uint64_t	 ps_id;			/* (c) printable space id     */
	struct port_entry *ps_table;		/* (p) dynamic, kmalloc'd     */
	size_t		 ps_capacity;		/* (p) slots in ps_table      */
	size_t		 ps_inuse;		/* (p) populated entries      */
	mach_port_name_t ps_hint;		/* (p) next-fit search start  */
};

/* ---- module state ------------------------------------------------------ */

struct port_space	*kernel_space;

/* Monotonic ids for debug logging. */
static uint64_t		next_port_id;
static uint64_t		next_space_id;
static struct spinlock	port_global_lock = SPINLOCK_INIT("port-global");

/*
 * Tuning constants.
 *
 * DEFAULT_QMAX gates how many in-flight messages can pile up on a
 * single port queue.  Set deliberately above what a single quantum
 * of CPU-bound producer activity can fill, so a busy sender doesn't
 * trip MACH_E_NOSPACE before the receiver gets its first time slice.
 * Real Mach blocks on full queues (or honours a timeout); we'll grow
 * into that the day a stress test demonstrates backpressure matters.
 */
#define	DEFAULT_QMAX		1024
#define	INITIAL_SPACE_CAP	16
#define	MAX_MSG_BYTES		4096

/* ---- forward decls ----------------------------------------------------- */

static struct port	*port_create(void);
static void		 port_free(struct port *);
static void		 port_ref(struct port *, uint8_t rights);
static void		 port_deref(struct port *, uint8_t rights);

static int		 space_grow_locked(struct port_space *,
			    size_t new_capacity);
static int		 space_install(struct port_space *,
			    struct port *p, uint8_t rights,
			    mach_port_name_t *name_out);
static struct port	*space_lookup(struct port_space *,
			    mach_port_name_t name, uint8_t need_right,
			    uint8_t *rights_out);
static int		 space_drop(struct port_space *,
			    mach_port_name_t name);
static int		 space_drop_one_right(struct port_space *,
			    mach_port_name_t name, uint8_t right);
static int		 space_unbind_no_deref(struct port_space *,
			    mach_port_name_t name, uint8_t right);
static int		 space_install_no_ref(struct port_space *,
			    struct port *p, uint8_t rights,
			    mach_port_name_t *name_out);

static int		 msg_validate(const struct mach_msg_header *);
static int		 msg_enqueue(struct port *, struct port_msg *,
			    struct thread **waiter_out);
static struct port_msg	*msg_dequeue(struct port *,
			    struct thread **send_waiter_out);

static void		 free_pending_descs(struct port_pending_desc *,
			    size_t n);

/* ---- subsystem init ---------------------------------------------------- */

void
port_subsystem_init(void)
{

	next_port_id  = 1;
	next_space_id = 1;

	kernel_space = port_space_new();
	if (kernel_space == NULL)
		panic("port_subsystem_init: kernel_space allocation failed");

	kprintf("port: kernel_space id=%llu, cap=%zu, qmax=%u, "
	    "max_msg=%u\n",
	    (unsigned long long)kernel_space->ps_id,
	    kernel_space->ps_capacity,
	    (unsigned)DEFAULT_QMAX,
	    (unsigned)MAX_MSG_BYTES);
}

/* ---- port object ------------------------------------------------------- */

static struct port *
port_create(void)
{
	struct port	*p;

	p = (struct port *)kmalloc(sizeof(*p));
	if (p == NULL)
		return (NULL);

	spin_init(&p->p_lock, "port");
	spin_lock(&port_global_lock);
	p->p_id = next_port_id++;
	spin_unlock(&port_global_lock);

	p->p_refs            = 0;
	p->p_send_count      = 0;
	p->p_send_once_count = 0;
	p->p_has_receive     = false;
	p->p_dead        = false;
	p->p_qlen        = 0;
	p->p_qmax        = DEFAULT_QMAX;
	p->p_qhead       = NULL;
	p->p_qtail       = NULL;
	p->p_waiters_head = NULL;
	p->p_waiters_tail = NULL;
	p->p_send_waiters_head = NULL;
	p->p_send_waiters_tail = NULL;
	p->p_set          = NULL;
	p->p_set_link     = NULL;
	return (p);
}

static void
port_free(struct port *p)
{
	struct port_msg	*m, *next;

	KASSERT(p->p_refs == 0, "port_free: refs != 0");
	KASSERT(p->p_send_count == 0, "port_free: send_count != 0");
	KASSERT(p->p_send_once_count == 0,
	    "port_free: send_once_count != 0");
	KASSERT(!p->p_has_receive, "port_free: receive right still held");
	KASSERT(p->p_waiters_head == NULL,
	    "port_free: recv waiters still parked");
	KASSERT(p->p_send_waiters_head == NULL,
	    "port_free: send waiters still parked");

	/*
	 * Drain any messages that were queued but never consumed; each
	 * one may carry port descriptors holding refs on other ports.
	 */
	m = p->p_qhead;
	while (m != NULL) {
		next = m->m_next;
		free_pending_descs(m->m_descs, m->m_ndescs);
		kfree(m);
		m = next;
	}
	kfree(p);
}

static void
port_ref(struct port *p, uint8_t rights)
{

	spin_lock(&p->p_lock);
	if (rights & MACH_PORT_RIGHT_SEND) {
		p->p_send_count++;
		p->p_refs++;
	}
	if (rights & MACH_PORT_RIGHT_SEND_ONCE) {
		p->p_send_once_count++;
		p->p_refs++;
	}
	if (rights & MACH_PORT_RIGHT_RECEIVE) {
		KASSERT(!p->p_has_receive,
		    "port_ref: duplicate RECEIVE right");
		p->p_has_receive = true;
		p->p_refs++;
	}
	spin_unlock(&p->p_lock);
}

static void
port_deref(struct port *p, uint8_t rights)
{
	bool	last;
	struct port_msg *drain_head = NULL;
	struct thread	*wake_head = NULL;
	struct thread	*send_wake_head = NULL;

	spin_lock(&p->p_lock);

	if (rights & MACH_PORT_RIGHT_SEND) {
		KASSERT(p->p_send_count > 0,
		    "port_deref: SEND underflow");
		p->p_send_count--;
		KASSERT(p->p_refs > 0, "port_deref: refs underflow");
		p->p_refs--;
	}
	if (rights & MACH_PORT_RIGHT_SEND_ONCE) {
		KASSERT(p->p_send_once_count > 0,
		    "port_deref: SEND_ONCE underflow");
		p->p_send_once_count--;
		KASSERT(p->p_refs > 0, "port_deref: refs underflow");
		p->p_refs--;
	}
	struct port_set *member_of = NULL;
	if (rights & MACH_PORT_RIGHT_RECEIVE) {
		KASSERT(p->p_has_receive,
		    "port_deref: RECEIVE not held");
		p->p_has_receive = false;
		p->p_dead = true;
		KASSERT(p->p_refs > 0, "port_deref: refs underflow");
		p->p_refs--;

		drain_head = p->p_qhead;
		p->p_qhead = p->p_qtail = NULL;
		p->p_qlen = 0;

		wake_head = p->p_waiters_head;
		p->p_waiters_head = p->p_waiters_tail = NULL;

		send_wake_head = p->p_send_waiters_head;
		p->p_send_waiters_head = p->p_send_waiters_tail = NULL;

		member_of = p->p_set;
		p->p_set = NULL;
	}

	/*
	 * No-senders detection.  If dropping a send-bearing right took
	 * the per-port sender count to zero while the receive end is
	 * still held and the queue is empty, any thread blocked in
	 * recv on this port is stranded -- no future message can ever
	 * arrive.  Mark the port dead and hand the waiters out for a
	 * post-unlock wake so they can return MACH_E_DEAD.
	 *
	 * Set-waiters are not woken here: the set as a whole stays
	 * alive as long as any other member still has senders, and
	 * we have no cheap way to certify that condition here.
	 */
	if ((rights & (MACH_PORT_RIGHT_SEND |
	    MACH_PORT_RIGHT_SEND_ONCE)) != 0 &&
	    p->p_send_count == 0 && p->p_send_once_count == 0 &&
	    p->p_has_receive && !p->p_dead &&
	    p->p_qlen == 0 && p->p_waiters_head != NULL) {
		p->p_dead = true;
		wake_head = p->p_waiters_head;
		p->p_waiters_head = p->p_waiters_tail = NULL;
	}

	last = (p->p_refs == 0);
	spin_unlock(&p->p_lock);

	while (drain_head != NULL) {
		struct port_msg *next = drain_head->m_next;
		free_pending_descs(drain_head->m_descs,
		    drain_head->m_ndescs);
		kfree(drain_head);
		drain_head = next;
	}

	while (wake_head != NULL) {
		struct thread *next = wake_head->th_runq_link;
		wake_head->th_runq_link = NULL;
		thread_wake(wake_head);
		wake_head = next;
	}

	while (send_wake_head != NULL) {
		struct thread *next = send_wake_head->th_runq_link;
		send_wake_head->th_runq_link = NULL;
		thread_wake(send_wake_head);
		send_wake_head = next;
	}

	/*
	 * If we were a member of a set, splice ourselves out so the
	 * set doesn't dangle a pointer at us once we're freed.
	 */
	if (member_of != NULL) {
		spin_lock(&member_of->ps_lock);
		struct port *cur, *prev = NULL;
		for (cur = member_of->ps_members_head; cur != NULL;
		    cur = cur->p_set_link) {
			if (cur == p) {
				if (prev == NULL)
					member_of->ps_members_head =
					    cur->p_set_link;
				else
					prev->p_set_link = cur->p_set_link;
				member_of->ps_member_count--;
				break;
			}
			prev = cur;
		}
		spin_unlock(&member_of->ps_lock);
		p->p_set_link = NULL;
	}

	if (last)
		port_free(p);
}

/* ---- port_set ---------------------------------------------------------- */

static struct port_set *
port_set_create(void)
{
	struct port_set	*ps;

	ps = (struct port_set *)kmalloc(sizeof(*ps));
	if (ps == NULL)
		return (NULL);
	spin_init(&ps->ps_lock, "port_set");
	spin_lock(&port_global_lock);
	ps->ps_id = next_port_id++;
	spin_unlock(&port_global_lock);
	ps->ps_refs         = 0;
	ps->ps_dead         = false;
	ps->ps_member_count = 0;
	ps->ps_members_head = NULL;
	ps->ps_waiters_head = NULL;
	ps->ps_waiters_tail = NULL;
	return (ps);
}

static void
port_set_free(struct port_set *ps)
{

	KASSERT(ps->ps_refs == 0, "port_set_free: refs != 0");
	KASSERT(ps->ps_member_count == 0,
	    "port_set_free: members still attached");
	kfree(ps);
}

static void
port_set_ref(struct port_set *ps)
{

	spin_lock(&ps->ps_lock);
	ps->ps_refs++;
	spin_unlock(&ps->ps_lock);
}

static void
port_set_deref(struct port_set *ps)
{
	bool		 last;
	struct port	*detach_head = NULL;
	struct thread	*wake_head = NULL;

	spin_lock(&ps->ps_lock);
	KASSERT(ps->ps_refs > 0, "port_set_deref: underflow");
	ps->ps_refs--;
	last = (ps->ps_refs == 0);
	if (last) {
		ps->ps_dead = true;
		detach_head = ps->ps_members_head;
		ps->ps_members_head = NULL;
		ps->ps_member_count = 0;
		wake_head = ps->ps_waiters_head;
		ps->ps_waiters_head = ps->ps_waiters_tail = NULL;
	}
	spin_unlock(&ps->ps_lock);

	/* Drop the set pointer on every member port. */
	while (detach_head != NULL) {
		struct port *p = detach_head;
		spin_lock(&p->p_lock);
		struct port *next = p->p_set_link;
		p->p_set = NULL;
		p->p_set_link = NULL;
		spin_unlock(&p->p_lock);
		detach_head = next;
	}

	while (wake_head != NULL) {
		struct thread *next = wake_head->th_runq_link;
		wake_head->th_runq_link = NULL;
		thread_wake(wake_head);
		wake_head = next;
	}

	if (last)
		port_set_free(ps);
}

/* ---- port_space -------------------------------------------------------- */

struct port_space *
port_space_new(void)
{
	struct port_space	*ps;

	ps = (struct port_space *)kmalloc(sizeof(*ps));
	if (ps == NULL)
		return (NULL);

	spin_init(&ps->ps_lock, "port-space");
	ps->ps_table = (struct port_entry *)kcalloc(INITIAL_SPACE_CAP,
	    sizeof(struct port_entry));
	if (ps->ps_table == NULL) {
		kfree(ps);
		return (NULL);
	}
	ps->ps_capacity = INITIAL_SPACE_CAP;
	ps->ps_inuse    = 0;
	ps->ps_hint     = 1;	/* skip MACH_PORT_NULL */

	spin_lock(&port_global_lock);
	ps->ps_id = next_space_id++;
	spin_unlock(&port_global_lock);

	return (ps);
}

void
port_space_destroy(struct port_space *ps)
{
	size_t	i;

	if (ps == NULL)
		return;

	spin_lock(&ps->ps_lock);
	for (i = 1; i < ps->ps_capacity; i++) {
		struct port	*p   = ps->ps_table[i].pe_port;
		struct port_set	*set = ps->ps_table[i].pe_set;
		uint8_t		 r   = ps->ps_table[i].pe_rights;

		if (p == NULL && set == NULL)
			continue;

		ps->ps_table[i].pe_port   = NULL;
		ps->ps_table[i].pe_set    = NULL;
		ps->ps_table[i].pe_rights = 0;
		spin_unlock(&ps->ps_lock);
		if (p != NULL)
			port_deref(p, r);
		if (set != NULL)
			port_set_deref(set);
		spin_lock(&ps->ps_lock);
	}
	spin_unlock(&ps->ps_lock);

	kfree(ps->ps_table);
	kfree(ps);
}

static int
space_grow_locked(struct port_space *ps, size_t new_capacity)
{
	struct port_entry	*new_tab;
	size_t			 i;

	new_tab = (struct port_entry *)kcalloc(new_capacity,
	    sizeof(struct port_entry));
	if (new_tab == NULL)
		return (-1);

	for (i = 0; i < ps->ps_capacity && i < new_capacity; i++)
		new_tab[i] = ps->ps_table[i];

	kfree(ps->ps_table);
	ps->ps_table    = new_tab;
	ps->ps_capacity = new_capacity;
	return (0);
}

/*
 * Slot allocator shared by space_install (port) and space_install_set
 * (port_set).  Finds an empty slot, growing the table if needed.
 * Caller fills in the object pointer + rights after the slot is
 * returned.  ps->ps_lock is held on entry and released on exit.
 */
static int
space_alloc_slot_locked(struct port_space *ps, mach_port_name_t *name_out)
{
	mach_port_name_t	n;
	size_t			i, start;
	bool			grew = false;

retry:
	start = ps->ps_hint == 0 ? 1 : ps->ps_hint;
	for (i = start; i < ps->ps_capacity; i++) {
		if (ps->ps_table[i].pe_port == NULL &&
		    ps->ps_table[i].pe_set == NULL) {
			n = (mach_port_name_t)i;
			goto found;
		}
	}
	for (i = 1; i < start && i < ps->ps_capacity; i++) {
		if (ps->ps_table[i].pe_port == NULL &&
		    ps->ps_table[i].pe_set == NULL) {
			n = (mach_port_name_t)i;
			goto found;
		}
	}

	if (grew)
		return (MACH_E_NOSPACE);
	if (space_grow_locked(ps, ps->ps_capacity * 2) != 0)
		return (MACH_E_NOMEM);
	grew = true;
	goto retry;

found:
	ps->ps_inuse++;
	ps->ps_hint = (n + 1 < ps->ps_capacity) ? (n + 1) : 1;
	*name_out = n;
	return (MACH_MSG_OK);
}

static int
space_install(struct port_space *ps, struct port *p, uint8_t rights,
    mach_port_name_t *name_out)
{
	mach_port_name_t	n;
	int			rv;

	if (rights == 0 || p == NULL)
		return (MACH_E_INVAL);

	spin_lock(&ps->ps_lock);
	rv = space_alloc_slot_locked(ps, &n);
	if (rv != MACH_MSG_OK) {
		spin_unlock(&ps->ps_lock);
		return (rv);
	}
	ps->ps_table[n].pe_port   = p;
	ps->ps_table[n].pe_set    = NULL;
	ps->ps_table[n].pe_rights = rights;
	spin_unlock(&ps->ps_lock);

	port_ref(p, rights);
	*name_out = n;
	return (MACH_MSG_OK);
}

static int
space_install_set(struct port_space *ps, struct port_set *set,
    mach_port_name_t *name_out)
{
	mach_port_name_t	n;
	int			rv;

	if (set == NULL)
		return (MACH_E_INVAL);

	spin_lock(&ps->ps_lock);
	rv = space_alloc_slot_locked(ps, &n);
	if (rv != MACH_MSG_OK) {
		spin_unlock(&ps->ps_lock);
		return (rv);
	}
	ps->ps_table[n].pe_port   = NULL;
	ps->ps_table[n].pe_set    = set;
	ps->ps_table[n].pe_rights = MACH_PORT_RIGHT_PORT_SET;
	spin_unlock(&ps->ps_lock);

	port_set_ref(set);
	*name_out = n;
	return (MACH_MSG_OK);
}

static struct port *
space_lookup(struct port_space *ps, mach_port_name_t name,
    uint8_t need_right, uint8_t *rights_out)
{
	struct port	*p;
	uint8_t		 r;

	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD)
		return (NULL);

	spin_lock(&ps->ps_lock);
	if (name >= ps->ps_capacity) {
		spin_unlock(&ps->ps_lock);
		return (NULL);
	}
	p = ps->ps_table[name].pe_port;
	r = ps->ps_table[name].pe_rights;
	if (p == NULL) {
		spin_unlock(&ps->ps_lock);
		return (NULL);
	}
	if ((r & need_right) != need_right) {
		spin_unlock(&ps->ps_lock);
		return (NULL);
	}
	spin_unlock(&ps->ps_lock);

	if (rights_out != NULL)
		*rights_out = r;
	return (p);
}

static struct port_set *
space_lookup_set(struct port_space *ps, mach_port_name_t name)
{
	struct port_set	*set;

	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD)
		return (NULL);

	spin_lock(&ps->ps_lock);
	if (name >= ps->ps_capacity) {
		spin_unlock(&ps->ps_lock);
		return (NULL);
	}
	set = ps->ps_table[name].pe_set;
	spin_unlock(&ps->ps_lock);
	return (set);
}

static int
space_drop(struct port_space *ps, mach_port_name_t name)
{
	struct port	*p;
	struct port_set	*set;
	uint8_t		 r;

	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD)
		return (MACH_E_INVAL);

	spin_lock(&ps->ps_lock);
	if (name >= ps->ps_capacity ||
	    (ps->ps_table[name].pe_port == NULL &&
	     ps->ps_table[name].pe_set == NULL)) {
		spin_unlock(&ps->ps_lock);
		return (MACH_E_NAME);
	}
	p   = ps->ps_table[name].pe_port;
	set = ps->ps_table[name].pe_set;
	r   = ps->ps_table[name].pe_rights;
	ps->ps_table[name].pe_port   = NULL;
	ps->ps_table[name].pe_set    = NULL;
	ps->ps_table[name].pe_rights = 0;
	ps->ps_inuse--;
	if (name < ps->ps_hint)
		ps->ps_hint = name;
	spin_unlock(&ps->ps_lock);

	if (p != NULL)
		port_deref(p, r);
	if (set != NULL)
		port_set_deref(set);
	return (MACH_MSG_OK);
}

/*
 * Drop ONE specific right kind from a name entry, leaving the rest in
 * place.  Used by MOVE_SEND so a name carrying both RECV and SEND
 * keeps its RECV.  If after the drop the entry is empty, remove it.
 */
static int
space_drop_one_right(struct port_space *ps, mach_port_name_t name,
    uint8_t right)
{
	struct port	*p;
	bool		 removed = false;

	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD)
		return (MACH_E_INVAL);

	spin_lock(&ps->ps_lock);
	if (name >= ps->ps_capacity ||
	    ps->ps_table[name].pe_port == NULL ||
	    (ps->ps_table[name].pe_rights & right) == 0) {
		spin_unlock(&ps->ps_lock);
		return (MACH_E_NAME);
	}
	p = ps->ps_table[name].pe_port;
	ps->ps_table[name].pe_rights =
	    (uint8_t)(ps->ps_table[name].pe_rights & ~right);
	if (ps->ps_table[name].pe_rights == 0) {
		ps->ps_table[name].pe_port = NULL;
		ps->ps_inuse--;
		if (name < ps->ps_hint)
			ps->ps_hint = name;
		removed = true;
	}
	spin_unlock(&ps->ps_lock);

	(void)removed;
	port_deref(p, right);
	return (MACH_MSG_OK);
}

/*
 * Remove a right from a name entry WITHOUT calling port_deref on the
 * port -- the caller is transferring the right elsewhere (the canonical
 * use is MOVE_RECEIVE, which moves the receive right into an in-flight
 * message rather than destroying it).  If the entry's rights mask
 * becomes empty the slot is freed.
 *
 * Conservation: the port object's p_has_receive / p_send_count /
 * p_send_once_count are NOT touched here; the right lives on,
 * temporarily owned by whatever the caller stuffed it into.
 */
static int
space_unbind_no_deref(struct port_space *ps, mach_port_name_t name,
    uint8_t right)
{

	if (name == MACH_PORT_NULL || name == MACH_PORT_DEAD)
		return (MACH_E_INVAL);

	spin_lock(&ps->ps_lock);
	if (name >= ps->ps_capacity ||
	    ps->ps_table[name].pe_port == NULL ||
	    (ps->ps_table[name].pe_rights & right) == 0) {
		spin_unlock(&ps->ps_lock);
		return (MACH_E_NAME);
	}
	ps->ps_table[name].pe_rights =
	    (uint8_t)(ps->ps_table[name].pe_rights & ~right);
	if (ps->ps_table[name].pe_rights == 0) {
		ps->ps_table[name].pe_port = NULL;
		ps->ps_inuse--;
		if (name < ps->ps_hint)
			ps->ps_hint = name;
	}
	spin_unlock(&ps->ps_lock);
	return (MACH_MSG_OK);
}

/*
 * Install a port at a fresh name WITHOUT calling port_ref -- the caller
 * is delivering a right that was already "checked out" of some other
 * space (the MOVE_RECEIVE counterpart of space_unbind_no_deref).
 */
static int
space_install_no_ref(struct port_space *ps, struct port *p, uint8_t rights,
    mach_port_name_t *name_out)
{
	mach_port_name_t	n;
	int			rv;

	if (rights == 0 || p == NULL)
		return (MACH_E_INVAL);

	spin_lock(&ps->ps_lock);
	rv = space_alloc_slot_locked(ps, &n);
	if (rv != MACH_MSG_OK) {
		spin_unlock(&ps->ps_lock);
		return (rv);
	}
	ps->ps_table[n].pe_port   = p;
	ps->ps_table[n].pe_set    = NULL;
	ps->ps_table[n].pe_rights = rights;
	spin_unlock(&ps->ps_lock);

	*name_out = n;
	return (MACH_MSG_OK);
}

/* ---- public space-level operations ------------------------------------ */

mach_port_name_t
port_allocate(struct port_space *ps, uint8_t rights)
{
	struct port		*p;
	mach_port_name_t	 n;
	int			 rv;

	if (rights == 0 ||
	    (rights & ~(MACH_PORT_RIGHT_RECEIVE |
	                MACH_PORT_RIGHT_SEND |
	                MACH_PORT_RIGHT_SEND_ONCE)) != 0)
		return (MACH_PORT_NULL);

	p = port_create();
	if (p == NULL)
		return (MACH_PORT_NULL);

	rv = space_install(ps, p, rights, &n);
	if (rv != MACH_MSG_OK) {
		port_free(p);
		return (MACH_PORT_NULL);
	}
	return (n);
}

int
port_deallocate(struct port_space *ps, mach_port_name_t name)
{

	return (space_drop(ps, name));
}

int
port_mod_refs(struct port_space *ps, mach_port_name_t name, uint8_t right)
{

	return (space_drop_one_right(ps, name, right));
}

/* ---- port_set public ops ----------------------------------------------- */

mach_port_name_t
port_set_allocate(struct port_space *ps)
{
	struct port_set		*set;
	mach_port_name_t	 n;
	int			 rv;

	set = port_set_create();
	if (set == NULL)
		return (MACH_PORT_NULL);

	rv = space_install_set(ps, set, &n);
	if (rv != MACH_MSG_OK) {
		port_set_free(set);
		return (MACH_PORT_NULL);
	}
	return (n);
}

int
port_set_insert(struct port_space *ps, mach_port_name_t set_name,
    mach_port_name_t port_name)
{
	struct port_set	*set;
	struct port	*p;
	uint8_t		 dummy;

	set = space_lookup_set(ps, set_name);
	if (set == NULL)
		return (MACH_E_NAME);

	p = space_lookup(ps, port_name, MACH_PORT_RIGHT_RECEIVE, &dummy);
	if (p == NULL)
		return (MACH_E_RIGHT);

	spin_lock(&p->p_lock);
	if (p->p_set != NULL) {
		spin_unlock(&p->p_lock);
		return (MACH_E_INVAL);	/* port already in a set */
	}
	p->p_set = set;
	spin_unlock(&p->p_lock);

	spin_lock(&set->ps_lock);
	p->p_set_link = set->ps_members_head;
	set->ps_members_head = p;
	set->ps_member_count++;
	spin_unlock(&set->ps_lock);

	return (MACH_MSG_OK);
}

int
port_space_inject_send(struct port_space *src, mach_port_name_t src_name,
    struct port_space *dst, mach_port_name_t *dst_name_out)
{
	struct port		*p;
	mach_port_name_t	 n;
	uint8_t			 rights;
	int			 rv;

	if (src == NULL || dst == NULL || dst_name_out == NULL)
		return (MACH_E_INVAL);

	/*
	 * Look up the source name with SEND right; space_lookup
	 * does NOT take a ref, so the caller is implicitly relying
	 * on `src_name` staying valid for the duration of this call.
	 * The new entry's port_ref taken inside space_install bumps
	 * the SEND count.
	 */
	p = space_lookup(src, src_name, MACH_PORT_RIGHT_SEND, &rights);
	if (p == NULL)
		return (MACH_E_RIGHT);

	rv = space_install(dst, p, MACH_PORT_RIGHT_SEND, &n);
	if (rv != MACH_MSG_OK)
		return (rv);

	*dst_name_out = n;
	return (MACH_MSG_OK);
}

int
port_set_remove(struct port_space *ps, mach_port_name_t set_name,
    mach_port_name_t port_name)
{
	struct port_set	*set;
	struct port	*p, *cur, *prev;
	uint8_t		 dummy;

	set = space_lookup_set(ps, set_name);
	if (set == NULL)
		return (MACH_E_NAME);

	p = space_lookup(ps, port_name, MACH_PORT_RIGHT_RECEIVE, &dummy);
	if (p == NULL)
		return (MACH_E_RIGHT);

	spin_lock(&set->ps_lock);
	prev = NULL;
	for (cur = set->ps_members_head; cur != NULL; cur = cur->p_set_link) {
		if (cur == p) {
			if (prev == NULL)
				set->ps_members_head = cur->p_set_link;
			else
				prev->p_set_link = cur->p_set_link;
			set->ps_member_count--;
			break;
		}
		prev = cur;
	}
	spin_unlock(&set->ps_lock);

	if (cur == NULL)
		return (MACH_E_INVAL);

	spin_lock(&p->p_lock);
	p->p_set      = NULL;
	p->p_set_link = NULL;
	spin_unlock(&p->p_lock);

	return (MACH_MSG_OK);
}

/* ---- message queue helpers -------------------------------------------- */

static int
msg_validate(const struct mach_msg_header *h)
{

	if (h == NULL)
		return (MACH_E_INVAL);
	if (h->msgh_size < sizeof(*h))
		return (MACH_E_INVAL);
	if (h->msgh_size > MAX_MSG_BYTES)
		return (MACH_E_INVAL);
	return (MACH_MSG_OK);
}

/*
 * Append a message; if anyone is waiting in recv_block on this port
 * OR on the port set this port is a member of, hand one waiter out
 * via *waiter_out so the caller can wake it AFTER dropping our
 * locks (thread_wake takes sched_lock; lock order is port -> sched).
 *
 * Set-waiters take precedence: the canonical pattern is one server
 * thread parked on a set serving many member ports, so we'd rather
 * wake the server than a stray port-direct waiter.
 */
static int
msg_enqueue(struct port *p, struct port_msg *m, struct thread **waiter_out)
{
	struct thread	*w = NULL;
	struct port_set	*set;

	*waiter_out = NULL;

	spin_lock(&p->p_lock);
	if (p->p_dead) {
		spin_unlock(&p->p_lock);
		return (MACH_E_DEAD);
	}
	if (p->p_qlen >= p->p_qmax) {
		spin_unlock(&p->p_lock);
		return (MACH_E_NOSPACE);
	}
	m->m_next = NULL;
	if (p->p_qtail == NULL) {
		p->p_qhead = m;
		p->p_qtail = m;
	} else {
		p->p_qtail->m_next = m;
		p->p_qtail = m;
	}
	p->p_qlen++;
	set = p->p_set;
	spin_unlock(&p->p_lock);

	if (set != NULL) {
		spin_lock(&set->ps_lock);
		w = set->ps_waiters_head;
		if (w != NULL) {
			set->ps_waiters_head = w->th_runq_link;
			if (set->ps_waiters_head == NULL)
				set->ps_waiters_tail = NULL;
			w->th_runq_link = NULL;
		}
		spin_unlock(&set->ps_lock);
	}

	if (w == NULL) {
		spin_lock(&p->p_lock);
		w = p->p_waiters_head;
		if (w != NULL) {
			p->p_waiters_head = w->th_runq_link;
			if (p->p_waiters_head == NULL)
				p->p_waiters_tail = NULL;
			w->th_runq_link = NULL;
		}
		spin_unlock(&p->p_lock);
	}

	*waiter_out = w;
	return (MACH_MSG_OK);
}

/*
 * Pop one thread off the port's send-waiter FIFO, if any.  Caller
 * holds p_lock; caller wakes the returned thread (if non-NULL) *after*
 * dropping the lock (thread_wake takes sched_lock, and our lock order
 * is port -> sched).
 */
static struct thread *
port_extract_send_waiter_locked(struct port *p)
{
	struct thread	*w;

	w = p->p_send_waiters_head;
	if (w != NULL) {
		p->p_send_waiters_head = w->th_runq_link;
		if (p->p_send_waiters_head == NULL)
			p->p_send_waiters_tail = NULL;
		w->th_runq_link = NULL;
	}
	return (w);
}

/*
 * Remove a specific thread from the port's recv-waiter list if it is
 * still on it.  Used by recv_timed cleanup: a thread woken because its
 * deadline expired (rather than because a sender extracted it) is
 * still on the list and must detach itself before returning E_TIMEOUT.
 * Caller holds p_lock.  Idempotent if the thread is already gone.
 */
static void
port_unbind_waiter_locked(struct port *p, struct thread *th)
{
	struct thread	**pp;

	pp = &p->p_waiters_head;
	while (*pp != NULL) {
		if (*pp == th) {
			*pp = th->th_runq_link;
			if (*pp == NULL)
				p->p_waiters_tail = NULL;
			th->th_runq_link = NULL;
			return;
		}
		pp = &(*pp)->th_runq_link;
	}
	/* Fall through silently if not present -- the sender wake path
	   may have already extracted us. */
}

static void
port_set_unbind_waiter_locked(struct port_set *set, struct thread *th)
{
	struct thread	**pp;

	pp = &set->ps_waiters_head;
	while (*pp != NULL) {
		if (*pp == th) {
			*pp = th->th_runq_link;
			if (*pp == NULL)
				set->ps_waiters_tail = NULL;
			th->th_runq_link = NULL;
			return;
		}
		pp = &(*pp)->th_runq_link;
	}
}

static struct port_msg *
msg_dequeue(struct port *p, struct thread **send_waiter_out)
{
	struct port_msg	*m;

	*send_waiter_out = NULL;
	spin_lock(&p->p_lock);
	m = p->p_qhead;
	if (m != NULL) {
		p->p_qhead = m->m_next;
		if (p->p_qhead == NULL)
			p->p_qtail = NULL;
		p->p_qlen--;
		*send_waiter_out = port_extract_send_waiter_locked(p);
	}
	spin_unlock(&p->p_lock);
	return (m);
}

static void
free_pending_descs(struct port_pending_desc *descs, size_t n)
{
	size_t	i;

	if (descs == NULL)
		return;

	for (i = 0; i < n; i++) {
		if (descs[i].pd_port != NULL) {
			port_deref(descs[i].pd_port,
			    descs[i].pd_disposition);
		}
	}
	kfree(descs);
}

/* ---- mach_msg_send --------------------------------------------------- */

/*
 * Resolve a port descriptor on send: translate the sender's name to
 * a port pointer, apply the disposition semantics (move/copy/make),
 * and stash {port, disposition} in the pending-descriptor slot.
 *
 * The pd_disposition we record here is what the RECEIVER will be
 * granted: SEND for the *_SEND family, SEND_ONCE for the *_SEND_ONCE
 * family.  We normalise away the move/copy/make distinction since
 * delivery semantics don't care -- the receiver always sees a fresh
 * name in their space carrying the right.
 */
static int
send_xlate_desc(struct port_space *from, mach_port_name_t name,
    uint8_t disposition, struct port_pending_desc *pd)
{
	struct port	*p;
	uint8_t		 rights;

	switch (disposition) {
	case MACH_MSG_TYPE_MOVE_RECEIVE:
		p = space_lookup(from, name, MACH_PORT_RIGHT_RECEIVE,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		/*
		 * Refuse if the port is currently a set member or has
		 * recv-blocked threads parked on it: moving the receive
		 * right out would either silently detach the set member
		 * or strand a thread on a right it no longer owns.  The
		 * sender must withdraw from the set / unblock the
		 * waiter first.
		 */
		spin_lock(&p->p_lock);
		if (p->p_set != NULL || p->p_waiters_head != NULL) {
			spin_unlock(&p->p_lock);
			return (MACH_E_INVAL);
		}
		spin_unlock(&p->p_lock);
		if (space_unbind_no_deref(from, name,
		    MACH_PORT_RIGHT_RECEIVE) != MACH_MSG_OK)
			return (MACH_E_NAME);
		pd->pd_port = p;
		pd->pd_disposition = MACH_PORT_RIGHT_RECEIVE;
		return (MACH_MSG_OK);

	case MACH_MSG_TYPE_MAKE_SEND:
		p = space_lookup(from, name, MACH_PORT_RIGHT_RECEIVE,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		port_ref(p, MACH_PORT_RIGHT_SEND);
		pd->pd_port = p;
		pd->pd_disposition = MACH_PORT_RIGHT_SEND;
		return (MACH_MSG_OK);

	case MACH_MSG_TYPE_COPY_SEND:
		p = space_lookup(from, name, MACH_PORT_RIGHT_SEND,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		port_ref(p, MACH_PORT_RIGHT_SEND);
		pd->pd_port = p;
		pd->pd_disposition = MACH_PORT_RIGHT_SEND;
		return (MACH_MSG_OK);

	case MACH_MSG_TYPE_MOVE_SEND:
		p = space_lookup(from, name, MACH_PORT_RIGHT_SEND,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		port_ref(p, MACH_PORT_RIGHT_SEND);
		if (space_drop_one_right(from, name,
		    MACH_PORT_RIGHT_SEND) != MACH_MSG_OK) {
			port_deref(p, MACH_PORT_RIGHT_SEND);
			return (MACH_E_NAME);
		}
		pd->pd_port = p;
		pd->pd_disposition = MACH_PORT_RIGHT_SEND;
		return (MACH_MSG_OK);

	case MACH_MSG_TYPE_MAKE_SEND_ONCE:
		p = space_lookup(from, name, MACH_PORT_RIGHT_RECEIVE,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		port_ref(p, MACH_PORT_RIGHT_SEND_ONCE);
		pd->pd_port = p;
		pd->pd_disposition = MACH_PORT_RIGHT_SEND_ONCE;
		return (MACH_MSG_OK);

	case MACH_MSG_TYPE_MOVE_SEND_ONCE:
		p = space_lookup(from, name, MACH_PORT_RIGHT_SEND_ONCE,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		port_ref(p, MACH_PORT_RIGHT_SEND_ONCE);
		if (space_drop_one_right(from, name,
		    MACH_PORT_RIGHT_SEND_ONCE) != MACH_MSG_OK) {
			port_deref(p, MACH_PORT_RIGHT_SEND_ONCE);
			return (MACH_E_NAME);
		}
		pd->pd_port = p;
		pd->pd_disposition = MACH_PORT_RIGHT_SEND_ONCE;
		return (MACH_MSG_OK);

	default:
		return (MACH_E_INVAL);
	}
}

int
mach_msg_send(struct port_space *from, const struct mach_msg_header *msg)
{
	int			 rv;
	struct port		*dest = NULL;
	struct port_msg		*m = NULL;
	struct port_pending_desc *descs = NULL;
	size_t			 ndescs = 0;
	size_t			 i, hdrs_off;
	uint8_t			 remote_disp, local_disp;
	bool			 has_local;
	bool			 complex;
	const uint8_t		*src;
	uint8_t			 dummy_rights;

	rv = msg_validate(msg);
	if (rv != MACH_MSG_OK)
		return (rv);

	remote_disp = MACH_MSGH_BITS_REMOTE(msg->msgh_bits);
	local_disp  = MACH_MSGH_BITS_LOCAL(msg->msgh_bits);
	complex     = (msg->msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
	has_local   = (msg->msgh_local != MACH_PORT_NULL) &&
	    (local_disp != 0);

	/*
	 * Pick the right kind the sender must hold for the remote.
	 * COPY_SEND and MOVE_SEND want SEND; MOVE_SEND_ONCE wants
	 * SEND_ONCE.  COPY_SEND_ONCE / MAKE_*_to-yourself are not
	 * legal as remote dispositions.
	 */
	uint8_t remote_right;
	switch (remote_disp) {
	case MACH_MSG_TYPE_COPY_SEND:
	case MACH_MSG_TYPE_MOVE_SEND:
		remote_right = MACH_PORT_RIGHT_SEND;
		break;
	case MACH_MSG_TYPE_MOVE_SEND_ONCE:
		remote_right = MACH_PORT_RIGHT_SEND_ONCE;
		break;
	default:
		return (MACH_E_INVAL);
	}

	dest = space_lookup(from, msg->msgh_remote, remote_right,
	    &dummy_rights);
	if (dest == NULL)
		return (MACH_E_RIGHT);

	/*
	 * Take an extra ref on dest of the same kind as the sender's
	 * right.  Released after enqueue regardless of MOVE/COPY.
	 */
	port_ref(dest, remote_right);

	/*
	 * For MOVE_* the sender relinquishes their right.  Any other
	 * rights under the same name (e.g. RECEIVE held simultaneously
	 * with SEND) survive the drop.
	 */
	if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
	    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
		(void)space_drop_one_right(from, msg->msgh_remote,
		    remote_right);
	}

	/* Allocate the queued message buffer (kmalloc-zeroed). */
	m = (struct port_msg *)kmalloc(sizeof(*m) + msg->msgh_size);
	if (m == NULL) {
		rv = MACH_E_NOMEM;
		goto fail;
	}
	m->m_next   = NULL;
	m->m_size   = msg->msgh_size;
	m->m_ndescs = 0;
	m->m_descs  = NULL;

	/* Copy the raw message bytes verbatim. */
	src = (const uint8_t *)msg;
	for (i = 0; i < msg->msgh_size; i++)
		m->m_buf[i] = src[i];

	hdrs_off = sizeof(struct mach_msg_header);

	if (complex) {
		const struct mach_msg_body *body;
		if (msg->msgh_size < hdrs_off +
		    sizeof(struct mach_msg_body)) {
			rv = MACH_E_INVAL;
			goto fail;
		}
		body = (const struct mach_msg_body *)
		    (m->m_buf + hdrs_off);
		ndescs = body->msgh_descriptor_count;
		if (ndescs > 0) {
			if (msg->msgh_size < hdrs_off +
			    sizeof(struct mach_msg_body) +
			    ndescs * sizeof(struct mach_msg_port_descriptor)) {
				rv = MACH_E_INVAL;
				goto fail;
			}
			descs = (struct port_pending_desc *)kcalloc(
			    ndescs, sizeof(struct port_pending_desc));
			if (descs == NULL) {
				rv = MACH_E_NOMEM;
				goto fail;
			}
		}
	}

	/*
	 * Implicit msgh_local descriptor: when sender supplies a reply
	 * port, treat it as a stealth port-descriptor with the local
	 * disposition.
	 */
	if (has_local) {
		struct port_pending_desc local_pd;
		rv = send_xlate_desc(from, msg->msgh_local, local_disp,
		    &local_pd);
		if (rv != MACH_MSG_OK)
			goto fail;
		/*
		 * Stash on the queued msg's m_descs[ndescs] slot --
		 * grow the array by 1 so recv code path doesn't
		 * special-case the header descriptor.
		 */
		struct port_pending_desc *bigger;
		bigger = (struct port_pending_desc *)kcalloc(ndescs + 1,
		    sizeof(struct port_pending_desc));
		if (bigger == NULL) {
			port_deref(local_pd.pd_port,
			    local_pd.pd_disposition);
			rv = MACH_E_NOMEM;
			goto fail;
		}
		for (i = 0; i < ndescs; i++)
			bigger[i] = descs[i];
		bigger[ndescs] = local_pd;
		if (descs != NULL)
			kfree(descs);
		descs = bigger;
		/*
		 * Zero out msgh_local in the buffered copy; recv will
		 * fill in the receiver-side name.
		 */
		struct mach_msg_header *hdr =
		    (struct mach_msg_header *)m->m_buf;
		hdr->msgh_local = MACH_PORT_NULL;
		ndescs++;
	}

	/* Body descriptors -- the explicit ones the user packaged. */
	if (complex) {
		struct mach_msg_port_descriptor *pds;
		size_t explicit_descs;

		pds = (struct mach_msg_port_descriptor *)
		    (m->m_buf + hdrs_off + sizeof(struct mach_msg_body));
		explicit_descs = has_local ? ndescs - 1 : ndescs;

		for (i = 0; i < explicit_descs; i++) {
			if (pds[i].type != MACH_MSG_PORT_DESCRIPTOR) {
				rv = MACH_E_INVAL;
				goto fail;
			}
			rv = send_xlate_desc(from, pds[i].name,
			    pds[i].disposition, &descs[i]);
			if (rv != MACH_MSG_OK)
				goto fail;
			/* Erase sender's name; recv will fill it in. */
			pds[i].name = MACH_PORT_NULL;
		}
	}

	m->m_ndescs = ndescs;
	m->m_descs  = descs;

	{
		struct thread	*waiter;
		struct thread	*self;

		/*
		 * Enqueue with backpressure: if the destination queue is
		 * full, park self on dest->p_send_waiters and wait for a
		 * recv to free a slot, then retry.  Wakes propagate via
		 * port_extract_send_waiter_locked in the recv paths.
		 *
		 * If the port dies while we are parked, port_deref's
		 * RECV-drop path drains p_send_waiters and the retry
		 * sees p_dead -> returns MACH_E_DEAD.
		 */
		for (;;) {
			rv = msg_enqueue(dest, m, &waiter);
			if (rv == MACH_MSG_OK)
				break;
			if (rv != MACH_E_NOSPACE)
				goto fail;

			spin_lock(&dest->p_lock);
			if (dest->p_dead) {
				spin_unlock(&dest->p_lock);
				rv = MACH_E_DEAD;
				goto fail;
			}
			if (dest->p_qlen < dest->p_qmax) {
				spin_unlock(&dest->p_lock);
				continue;
			}
			self = current_thread;
			self->th_runq_link = NULL;
			if (dest->p_send_waiters_tail == NULL) {
				dest->p_send_waiters_head = self;
				dest->p_send_waiters_tail = self;
			} else {
				dest->p_send_waiters_tail->th_runq_link =
				    self;
				dest->p_send_waiters_tail = self;
			}
			thread_block_release(THREAD_BLOCK_PORT, dest,
			    &dest->p_lock);
			/* Loop and retry. */
		}
		if (waiter != NULL)
			thread_wake(waiter);
	}

	/*
	 * Drop the extra ref we took right after lookup.  The kind
	 * matches the sender's right: SEND for COPY/MOVE_SEND,
	 * SEND_ONCE for MOVE_SEND_ONCE.
	 */
	port_deref(dest, remote_right);
	return (MACH_MSG_OK);

fail:
	if (descs != NULL) {
		/* Roll back per-descriptor refs of whatever kind they are. */
		for (i = 0; i < ndescs; i++) {
			if (descs[i].pd_port != NULL)
				port_deref(descs[i].pd_port,
				    descs[i].pd_disposition);
		}
		kfree(descs);
	}
	if (m != NULL)
		kfree(m);
	if (dest != NULL)
		port_deref(dest, remote_right);
	return (rv);
}

/* ---- mach_msg_recv --------------------------------------------------- */

static int
deliver_msg(struct port_space *to, struct port *p, struct port_msg *m,
    struct mach_msg_header *buf, size_t buf_size)
{
	struct mach_msg_header	*hdr;
	struct mach_msg_port_descriptor *pds = NULL;
	size_t			 hdrs_off, explicit_descs;
	size_t			 i;
	int			 rv;
	bool			 has_local;

	if (buf_size < m->m_size) {
		/* Buffer too small: put message back at queue head. */
		spin_lock(&p->p_lock);
		m->m_next = p->p_qhead;
		p->p_qhead = m;
		if (p->p_qtail == NULL)
			p->p_qtail = m;
		p->p_qlen++;
		spin_unlock(&p->p_lock);
		return (MACH_E_TOOSMALL);
	}

	hdr = (struct mach_msg_header *)m->m_buf;
	hdrs_off = sizeof(struct mach_msg_header);
	has_local = (MACH_MSGH_BITS_LOCAL(hdr->msgh_bits) != 0);

	if (hdr->msgh_bits & MACH_MSGH_BITS_COMPLEX) {
		struct mach_msg_body *body =
		    (struct mach_msg_body *)(m->m_buf + hdrs_off);
		explicit_descs = body->msgh_descriptor_count;
		pds = (struct mach_msg_port_descriptor *)
		    (m->m_buf + hdrs_off +
		     sizeof(struct mach_msg_body));
	} else {
		explicit_descs = 0;
	}

	for (i = 0; i < explicit_descs; i++) {
		mach_port_name_t n;
		uint8_t kind = m->m_descs[i].pd_disposition;
		/*
		 * RECEIVE descriptors carry the right itself rather than
		 * a pending ref -- install without a fresh port_ref, and
		 * skip the matching port_deref below.  See send_xlate_desc
		 * MOVE_RECEIVE case for the symmetry.
		 */
		if (kind == MACH_PORT_RIGHT_RECEIVE)
			rv = space_install_no_ref(to,
			    m->m_descs[i].pd_port, kind, &n);
		else
			rv = space_install(to,
			    m->m_descs[i].pd_port, kind, &n);
		if (rv != MACH_MSG_OK) {
			while (i > 0) {
				i--;
				(void)space_drop(to, pds[i].name);
				pds[i].name = MACH_PORT_NULL;
			}
			for (i = 0; i < m->m_ndescs; i++) {
				if (m->m_descs[i].pd_port != NULL)
					port_deref(m->m_descs[i].pd_port,
					    m->m_descs[i].pd_disposition);
			}
			if (m->m_descs != NULL)
				kfree(m->m_descs);
			kfree(m);
			return (rv);
		}
		if (kind != MACH_PORT_RIGHT_RECEIVE)
			port_deref(m->m_descs[i].pd_port, kind);
		m->m_descs[i].pd_port = NULL;
		pds[i].name = n;
	}

	if (has_local) {
		size_t li = m->m_ndescs - 1;
		mach_port_name_t n;
		uint8_t kind = m->m_descs[li].pd_disposition;
		if (kind == MACH_PORT_RIGHT_RECEIVE)
			rv = space_install_no_ref(to,
			    m->m_descs[li].pd_port, kind, &n);
		else
			rv = space_install(to,
			    m->m_descs[li].pd_port, kind, &n);
		if (rv != MACH_MSG_OK) {
			for (i = 0; i < explicit_descs; i++) {
				if (pds[i].name != MACH_PORT_NULL)
					(void)space_drop(to, pds[i].name);
			}
			port_deref(m->m_descs[li].pd_port, kind);
			if (m->m_descs != NULL)
				kfree(m->m_descs);
			kfree(m);
			return (rv);
		}
		if (kind != MACH_PORT_RIGHT_RECEIVE)
			port_deref(m->m_descs[li].pd_port, kind);
		m->m_descs[li].pd_port = NULL;
		hdr->msgh_local = n;
	}

	{
		uint8_t *dst = (uint8_t *)buf;
		for (i = 0; i < m->m_size; i++)
			dst[i] = m->m_buf[i];
	}

	if (m->m_descs != NULL)
		kfree(m->m_descs);
	kfree(m);
	return (MACH_MSG_OK);
}

int
mach_msg_recv(struct port_space *to, mach_port_name_t recv_name,
    struct mach_msg_header *buf, size_t buf_size)
{
	struct port	*p;
	struct port_msg	*m;
	struct thread	*send_waiter;
	uint8_t		 dummy;
	int		 rv;

	if (buf == NULL || buf_size < sizeof(struct mach_msg_header))
		return (MACH_E_INVAL);

	p = space_lookup(to, recv_name, MACH_PORT_RIGHT_RECEIVE, &dummy);
	if (p == NULL)
		return (MACH_E_RIGHT);

	m = msg_dequeue(p, &send_waiter);
	if (m == NULL)
		return (MACH_E_NOMSG);

	rv = deliver_msg(to, p, m, buf, buf_size);
	if (send_waiter != NULL)
		thread_wake(send_waiter);
	return (rv);
}

int
mach_msg_recv_block(struct port_space *to, mach_port_name_t recv_name,
    struct mach_msg_header *buf, size_t buf_size)
{

	return (mach_msg_recv_timed(to, recv_name, buf, buf_size,
	    MACH_TIMEOUT_FOREVER));
}

int
mach_msg_recv_timed(struct port_space *to, mach_port_name_t recv_name,
    struct mach_msg_header *buf, size_t buf_size, uint64_t timeout_ms)
{
	struct port	*p;
	struct port_set	*set;
	struct port_msg	*m;
	struct thread	*self;
	uint64_t	 deadline;
	uint8_t		 dummy;

	if (buf == NULL || buf_size < sizeof(struct mach_msg_header))
		return (MACH_E_INVAL);

	/*
	 * Resolve the deadline once.  For FOREVER and NONE the actual
	 * value is unused -- those flags drive different control paths
	 * (skip the timed-waiters dance vs. return E_NOMSG on empty
	 * without ever blocking).
	 */
	deadline = 0;
	if (timeout_ms != MACH_TIMEOUT_FOREVER &&
	    timeout_ms != MACH_TIMEOUT_NONE)
		deadline = clock_uptime_ms() + timeout_ms;

	/*
	 * Two flavours: name refers to a port (RECEIVE right) -> wait
	 * on that port; OR name refers to a port_set -> wait on the
	 * set and serve whichever member has the next message.
	 */
	set = space_lookup_set(to, recv_name);
	if (set == NULL) {
		p = space_lookup(to, recv_name,
		    MACH_PORT_RIGHT_RECEIVE, &dummy);
		if (p == NULL)
			return (MACH_E_RIGHT);

		for (;;) {
			struct thread *send_waiter;
			int rv;

			spin_lock(&p->p_lock);

			if (p->p_dead) {
				spin_unlock(&p->p_lock);
				return (MACH_E_DEAD);
			}

			if (p->p_qhead != NULL) {
				m = p->p_qhead;
				p->p_qhead = m->m_next;
				if (p->p_qhead == NULL)
					p->p_qtail = NULL;
				p->p_qlen--;
				send_waiter =
				    port_extract_send_waiter_locked(p);
				spin_unlock(&p->p_lock);
				rv = deliver_msg(to, p, m, buf, buf_size);
				if (send_waiter != NULL)
					thread_wake(send_waiter);
				return (rv);
			}

			if (timeout_ms == MACH_TIMEOUT_NONE) {
				spin_unlock(&p->p_lock);
				return (MACH_E_NOMSG);
			}

			self = current_thread;
			self->th_runq_link = NULL;
			self->th_timed_out = 0;
			if (p->p_waiters_tail == NULL) {
				p->p_waiters_head = self;
				p->p_waiters_tail = self;
			} else {
				p->p_waiters_tail->th_runq_link = self;
				p->p_waiters_tail = self;
			}

			if (timeout_ms != MACH_TIMEOUT_FOREVER) {
				self->th_wake_deadline_ms = deadline;
				sched_add_timed_waiter(self);
			}

			thread_block_release(THREAD_BLOCK_PORT, p,
			    &p->p_lock);

			/*
			 * Woken.  Detach from the timed-waiters list
			 * unconditionally; the call is idempotent if the
			 * PIT already pulled us off.
			 */
			sched_remove_timed_waiter(self);

			if (self->th_timed_out) {
				self->th_timed_out = 0;
				/*
				 * The PIT woke us, not a sender, so we are
				 * still on p->p_waiters.  Unbind, then check
				 * the queue one last time in case a sender
				 * raced in between the PIT wake and our
				 * reacquire of p_lock.
				 */
				spin_lock(&p->p_lock);
				port_unbind_waiter_locked(p, self);
				if (p->p_qhead != NULL) {
					m = p->p_qhead;
					p->p_qhead = m->m_next;
					if (p->p_qhead == NULL)
						p->p_qtail = NULL;
					p->p_qlen--;
					send_waiter =
					    port_extract_send_waiter_locked(p);
					spin_unlock(&p->p_lock);
					rv = deliver_msg(to, p, m, buf,
					    buf_size);
					if (send_waiter != NULL)
						thread_wake(send_waiter);
					return (rv);
				}
				spin_unlock(&p->p_lock);
				return (MACH_E_TIMEOUT);
			}
			/* Real wake from a sender; loop to dequeue. */
		}
	}

	/* Port-set recv. */
	for (;;) {
		spin_lock(&set->ps_lock);

		if (set->ps_dead) {
			spin_unlock(&set->ps_lock);
			return (MACH_E_DEAD);
		}

		/* Scan members for the first non-empty queue. */
		for (p = set->ps_members_head; p != NULL;
		    p = p->p_set_link) {
			struct thread *send_waiter;
			int rv;

			spin_lock(&p->p_lock);
			if (p->p_qhead != NULL) {
				m = p->p_qhead;
				p->p_qhead = m->m_next;
				if (p->p_qhead == NULL)
					p->p_qtail = NULL;
				p->p_qlen--;
				send_waiter =
				    port_extract_send_waiter_locked(p);
				spin_unlock(&p->p_lock);
				spin_unlock(&set->ps_lock);
				rv = deliver_msg(to, p, m, buf, buf_size);
				if (send_waiter != NULL)
					thread_wake(send_waiter);
				return (rv);
			}
			spin_unlock(&p->p_lock);
		}

		if (timeout_ms == MACH_TIMEOUT_NONE) {
			spin_unlock(&set->ps_lock);
			return (MACH_E_NOMSG);
		}

		/* No member has a message -- block on set's waiter list. */
		self = current_thread;
		self->th_runq_link = NULL;
		self->th_timed_out = 0;
		if (set->ps_waiters_tail == NULL) {
			set->ps_waiters_head = self;
			set->ps_waiters_tail = self;
		} else {
			set->ps_waiters_tail->th_runq_link = self;
			set->ps_waiters_tail = self;
		}

		if (timeout_ms != MACH_TIMEOUT_FOREVER) {
			self->th_wake_deadline_ms = deadline;
			sched_add_timed_waiter(self);
		}

		thread_block_release(THREAD_BLOCK_PORT, set,
		    &set->ps_lock);

		sched_remove_timed_waiter(self);

		if (self->th_timed_out) {
			self->th_timed_out = 0;
			spin_lock(&set->ps_lock);
			port_set_unbind_waiter_locked(set, self);
			spin_unlock(&set->ps_lock);
			/*
			 * Loop back: a sender may have placed a message
			 * on one of the members in the race window.  The
			 * scan at the top of the loop will pick it up; if
			 * the queues are still empty, we'll either re-park
			 * (still within the original deadline, which has
			 * elapsed -> immediately re-time-out) or return
			 * E_TIMEOUT through the recomputed deadline path.
			 * Simpler: just check elapsed time and return.
			 */
			return (MACH_E_TIMEOUT);
		}
	}
}

/*
 * mach_msg_rpc: send-then-recv with an autogenerated reply port.
 *
 * The reply port is created in `space` carrying RECEIVE + SEND.  We
 * splice it into req->msgh_local with disposition MAKE_SEND so the
 * receiver receives a SEND right under their own name; they reply by
 * msgh_remote = (that name) and the message lands back in our recv on
 * the same reply port.  On any error -- send failure, recv failure,
 * timeout -- the reply port is deallocated before return.
 *
 * `req->msgh_bits` is updated to encode the local disposition; the
 * caller's remote disposition (COPY_SEND, MOVE_SEND_ONCE, ...) is
 * preserved as-is.
 */
int
mach_msg_rpc(struct port_space *space, struct mach_msg_header *req,
    struct mach_msg_header *reply_buf, size_t reply_buf_size,
    uint64_t timeout_ms)
{
	mach_port_name_t	reply_name;
	uint32_t		remote_disp;
	int			rv;

	if (space == NULL || req == NULL || reply_buf == NULL)
		return (MACH_E_INVAL);
	if (reply_buf_size < sizeof(struct mach_msg_header))
		return (MACH_E_INVAL);

	reply_name = port_allocate(space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (reply_name == MACH_PORT_NULL)
		return (MACH_E_NOMEM);

	remote_disp = MACH_MSGH_BITS_REMOTE(req->msgh_bits);
	req->msgh_bits  = MACH_MSGH_BITS(remote_disp,
	    MACH_MSG_TYPE_MAKE_SEND);
	req->msgh_local = reply_name;

	rv = mach_msg_send(space, req);
	if (rv != MACH_MSG_OK) {
		(void)port_deallocate(space, reply_name);
		return (rv);
	}

	rv = mach_msg_recv_timed(space, reply_name, reply_buf,
	    reply_buf_size, timeout_ms);
	(void)port_deallocate(space, reply_name);
	return (rv);
}

/* ---- introspection ---------------------------------------------------- */

size_t
port_space_inuse(struct port_space *ps)
{
	size_t	v;

	spin_lock(&ps->ps_lock);
	v = ps->ps_inuse;
	spin_unlock(&ps->ps_lock);
	return (v);
}

size_t
port_queue_len(struct port_space *ps, mach_port_name_t name)
{
	struct port	*p;
	uint8_t		 dummy;
	size_t		 v;

	p = space_lookup(ps, name, 0, &dummy);
	if (p == NULL)
		return (0);
	spin_lock(&p->p_lock);
	v = p->p_qlen;
	spin_unlock(&p->p_lock);
	return (v);
}

void
port_space_print(struct port_space *ps)
{
	size_t	i;

	spin_lock(&ps->ps_lock);
	kprintf("port_space id=%llu cap=%zu inuse=%zu:\n",
	    (unsigned long long)ps->ps_id,
	    ps->ps_capacity, ps->ps_inuse);
	for (i = 1; i < ps->ps_capacity; i++) {
		struct port *p = ps->ps_table[i].pe_port;
		struct port_set *set = ps->ps_table[i].pe_set;
		uint8_t r = ps->ps_table[i].pe_rights;

		if (p == NULL && set == NULL)
			continue;

		if (set != NULL) {
			spin_lock(&set->ps_lock);
			kprintf("  name=%-5zu SET id=%llu members=%zu "
			    "refs=%u%s\n",
			    i,
			    (unsigned long long)set->ps_id,
			    set->ps_member_count,
			    set->ps_refs,
			    set->ps_dead ? " DEAD" : "");
			spin_unlock(&set->ps_lock);
			continue;
		}

		spin_lock(&p->p_lock);
		kprintf("  name=%-5zu port_id=%llu rights=%s%s%s "
		    "qlen=%zu/%zu refs=%u send=%u so=%u%s%s\n",
		    i,
		    (unsigned long long)p->p_id,
		    (r & MACH_PORT_RIGHT_RECEIVE)   ? "R" : "-",
		    (r & MACH_PORT_RIGHT_SEND)      ? "S" : "-",
		    (r & MACH_PORT_RIGHT_SEND_ONCE) ? "O" : "-",
		    p->p_qlen, p->p_qmax,
		    p->p_refs, p->p_send_count, p->p_send_once_count,
		    p->p_dead   ? " DEAD"   : "",
		    p->p_set    ? " (set)" : "");
		spin_unlock(&p->p_lock);
	}
	spin_unlock(&ps->ps_lock);
}

const char *
mach_msg_strerror(int code)
{

	switch (code) {
	case MACH_MSG_OK:	return ("ok");
	case MACH_E_INVAL:	return ("invalid argument");
	case MACH_E_NAME:	return ("name not in space");
	case MACH_E_RIGHT:	return ("missing port right");
	case MACH_E_DEAD:	return ("port is dead");
	case MACH_E_NOSPACE:	return ("queue / table full");
	case MACH_E_NOMSG:	return ("no message available");
	case MACH_E_TOOSMALL:	return ("receive buffer too small");
	case MACH_E_NOMEM:	return ("out of memory");
	case MACH_E_TIMEOUT:	return ("recv timed out");
	default:		return ("?");
	}
}
