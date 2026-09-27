/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "bootstrap.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "port_internal.h"
#include "sched.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"

/*
 * Mach IPC object lifecycle: port and port_set creation, destruction and
 * ref counting, subsystem init (kernel_space, id counters), and the
 * special-port plumbing for kernel-implemented objects (task_self,
 * bootstrap, kernel-owned service ports).
 *
 * Messaging is in port_msg.c, name tables in port_space.c; cross-file
 * helpers are declared in port_internal.h.
 */

struct port_space	*kernel_space;

uint64_t		 next_port_id;
uint64_t		 next_space_id;
struct spinlock		 port_global_lock = SPINLOCK_INIT("port-global");

/* Provided by mach/port_space.c. */
extern struct port_space	*port_space_new(void);

/* ---- subsystem init -------------------------------------------------- */

void
port_subsystem_init(void)
{

	next_port_id  = 1;
	next_space_id = 1;

	kernel_space = port_space_new();
	if (kernel_space == NULL)
		panic("port_subsystem_init: kernel_space allocation failed");

	kprintf("port: kernel_space id=%llu, cap=%u, qmax=%u, "
	    "max_msg=%u\n",
	    (unsigned long long)kernel_space->ps_id,
	    (unsigned)INITIAL_SPACE_CAP,
	    (unsigned)DEFAULT_QMAX,
	    (unsigned)MAX_MSG_BYTES);
}

/* ---- port object ----------------------------------------------------- */

/* In port_msg.c; releases what an undelivered message still holds. */
extern void	free_pending_descs(struct port_pending_desc *, size_t n);

struct port *
port_create(void)
{
	struct port	*p;

	p = kmalloc(sizeof(*p));
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
	p->p_qresv       = 0;
	p->p_qmax        = DEFAULT_QMAX;
	p->p_qhead       = NULL;
	p->p_qtail       = NULL;
	p->p_waiters_head = NULL;
	p->p_waiters_tail = NULL;
	p->p_send_waiters_head = NULL;
	p->p_send_waiters_tail = NULL;
	p->p_set          = NULL;
	p->p_set_link     = NULL;
	p->p_special      = PORT_SPECIAL_NONE;
	p->p_special_arg  = NULL;
	p->p_stash_buf    = NULL;
	p->p_stash_thread = NULL;
	p->p_stash_size   = 0;
	p->p_stash_rv     = MACH_E_NOMSG;
	p->p_notify_no_senders     = NULL;
	p->p_notify_no_senders_id  = 0;
	p->p_notify_dead_name      = NULL;
	p->p_notify_port_destroyed    = NULL;
	p->p_notify_port_destroyed_id = 0;
	return (p);
}

void
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
	KASSERT(p->p_qresv == 0, "port_free: a sender still holds a slot");
	KASSERT(p->p_notify_port_destroyed == NULL,
	    "port_free: PORT_DESTROYED still armed");

	/*
	 * Drain undelivered messages; their descriptors hold port refs and
	 * OOL frames.
	 */
	m = p->p_qhead;
	while (m != NULL) {
		next = m->m_next;
		free_pending_descs(m->m_descs, m->m_ndescs);
		kfree(m);
		m = next;
	}

	/*
	 * Defensive: the RECEIVE drop normally fires and detaches every
	 * DEAD_NAME watch first.  Any left never fired; release them.
	 */
	while (p->p_notify_dead_name != NULL) {
		struct port_notify_node *next_n = p->p_notify_dead_name->nn_next;

		port_deref(p->p_notify_dead_name->nn_port,
		    MACH_PORT_RIGHT_SEND);
		kfree(p->p_notify_dead_name);
		p->p_notify_dead_name = next_n;
	}

	kfree(p);
}

void
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

/*
 * A ref that is no right: it keeps `p` in memory and is no sender, for a
 * send by the receiver to its own port (MAKE_SEND, MAKE_SEND_ONCE), which
 * must not look like the last sender leaving when it ends.
 */
void
port_hold(struct port *p)
{

	spin_lock(&p->p_lock);
	p->p_refs++;
	spin_unlock(&p->p_lock);
}

void
port_release(struct port *p)
{
	bool	last;

	spin_lock(&p->p_lock);
	KASSERT(p->p_refs > 0, "port_release: refs underflow");
	p->p_refs--;
	last = (p->p_refs == 0);
	spin_unlock(&p->p_lock);
	if (last)
		port_free(p);
}

/* Take `p` off the member list of `set`, which p_set no longer names. */
static void
port_leave_set(struct port *p, struct port_set *set)
{
	struct port	*cur, *prev;

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
	p->p_set_link = NULL;
}

/*
 * RECEIVE on `p` is being destroyed.  With PORT_DESTROYED armed the right
 * goes to the notify port instead, inside the notification (Mach's
 * ipc_port_destroy): the port lives on with its queue, its senders and
 * their names, and only its receiver changes.  As when it dies, it leaves
 * its set and its parked receivers are woken, to find the right gone.
 * One-shot.  False when nothing was armed, the port is already dead, or
 * the notification could not be queued: then it dies.
 *
 * A notify port that is itself in transit can close a cycle, each port's
 * right in the other's queue, as a MOVE_RECEIVE can; both then leak.
 */
static bool
port_destroyed_divert(struct port *p)
{
	struct port	*notify;
	struct port_set	*member_of;
	struct thread	*wake_head;
	struct thread	*hw;
	uint32_t	 tag;
	int		 rv;

	spin_lock(&p->p_lock);
	notify = p->p_notify_port_destroyed;
	tag    = p->p_notify_port_destroyed_id;
	p->p_notify_port_destroyed    = NULL;
	p->p_notify_port_destroyed_id = 0;
	if (notify == NULL || p->p_dead) {
		spin_unlock(&p->p_lock);
		if (notify != NULL)
			port_deref(notify, MACH_PORT_RIGHT_SEND);
		return (false);
	}
	member_of = p->p_set;
	p->p_set  = NULL;
	wake_head = p->p_waiters_head;
	p->p_waiters_head = p->p_waiters_tail = NULL;
	for (hw = wake_head; hw != NULL; hw = hw->th_wait_link)
		thread_hold(hw);
	spin_unlock(&p->p_lock);

	if (member_of != NULL)
		port_leave_set(p, member_of);
	while (wake_head != NULL) {
		hw = wake_head->th_wait_link;
		wake_head->th_wait_link = NULL;
		thread_wake(wake_head);
		thread_unhold(wake_head);
		wake_head = hw;
	}

	rv = port_destroyed_post(notify, p, tag);
	/* The registration's SEND ref, fired or not. */
	port_deref(notify, MACH_PORT_RIGHT_SEND);
	return (rv == MACH_MSG_OK);
}

void
port_deref(struct port *p, uint8_t rights)
{
	bool	last;
	struct port_msg *drain_head = NULL;
	struct thread	*wake_head = NULL;
	struct thread	*send_wake_head = NULL;
	struct thread	*hw;
	struct port	*notify_no_senders = NULL;
	struct port_notify_node *dead_name_list = NULL;
	uint32_t	 notify_no_senders_id = 0;
	bool		 fire_no_senders = false;

	/* A receive right with somewhere to go goes there, and lives. */
	if ((rights & MACH_PORT_RIGHT_RECEIVE) != 0 &&
	    port_destroyed_divert(p)) {
		rights &= (uint8_t)~MACH_PORT_RIGHT_RECEIVE;
		if (rights == 0)
			return;
	}

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

		/*
		 * Hold every thread on both chains while p_lock still proves
		 * them alive (a listed thread cannot finish exiting without
		 * p_lock to unlink itself).  The wakes run after the unlock,
		 * when a kill could retire and reap a chain member; the hold
		 * keeps the reaper off until our wake has landed
		 * (th_wake_hold, kern/thread.h).
		 */
		for (hw = wake_head; hw != NULL; hw = hw->th_wait_link)
			thread_hold(hw);
		for (hw = send_wake_head; hw != NULL; hw = hw->th_wait_link)
			thread_hold(hw);

		member_of = p->p_set;
		p->p_set = NULL;

		/*
		 * With the receiver gone NO_SENDERS can never fire: clear
		 * the slot, and release its SEND ref after the unlock
		 * without firing.
		 */
		if (p->p_notify_no_senders != NULL) {
			notify_no_senders = p->p_notify_no_senders;
			p->p_notify_no_senders    = NULL;
			p->p_notify_no_senders_id = 0;
		}
		/*
		 * The receiver going away is what DEAD_NAME waits for:
		 * detach the list and fire it after the unlock.
		 */
		dead_name_list = p->p_notify_dead_name;
		p->p_notify_dead_name = NULL;
	}

	/*
	 * No senders left while RECEIVE is still held:
	 *	a NO_SENDERS notification is armed -- fire it (one-shot);
	 *	  the port lives on, and the receiver may MAKE_SEND again;
	 *	none armed -- if the queue is empty and receivers are
	 *	  parked, mark the port dead and wake them with MACH_E_DEAD
	 *	  rather than strand them waiting for a message that cannot
	 *	  come.
	 *
	 * Set waiters are not woken: the set lives on while any other
	 * member has senders, which is not cheap to establish here.
	 */
	if ((rights & (MACH_PORT_RIGHT_SEND |
	    MACH_PORT_RIGHT_SEND_ONCE)) != 0 &&
	    p->p_send_count == 0 && p->p_send_once_count == 0 &&
	    p->p_has_receive && !p->p_dead) {
		if (p->p_notify_no_senders != NULL) {
			notify_no_senders    = p->p_notify_no_senders;
			notify_no_senders_id = p->p_notify_no_senders_id;
			p->p_notify_no_senders    = NULL;
			p->p_notify_no_senders_id = 0;
			fire_no_senders = true;
		} else if (p->p_qlen == 0 && p->p_waiters_head != NULL) {
			p->p_dead = true;
			wake_head = p->p_waiters_head;
			p->p_waiters_head = p->p_waiters_tail = NULL;
			/* Same hold as the RECEIVE-drop chains above. */
			for (hw = wake_head; hw != NULL;
			    hw = hw->th_wait_link)
				thread_hold(hw);
		}
	}

	last = (p->p_refs == 0);
	spin_unlock(&p->p_lock);

	if (fire_no_senders) {
		(void)port_notify_enqueue(notify_no_senders,
		    MACH_NOTIFY_NO_SENDERS, notify_no_senders_id);
	}
	if (notify_no_senders != NULL) {
		/*
		 * The registration's SEND ref, whether it fired or the
		 * source died with it armed.
		 */
		port_deref(notify_no_senders, MACH_PORT_RIGHT_SEND);
	}
	/*
	 * Fire each DEAD_NAME watch, then drop its SEND ref and free the
	 * node.
	 */
	while (dead_name_list != NULL) {
		struct port_notify_node *next = dead_name_list->nn_next;

		(void)port_notify_enqueue(dead_name_list->nn_port,
		    MACH_NOTIFY_DEAD_NAME, dead_name_list->nn_tag);
		port_deref(dead_name_list->nn_port, MACH_PORT_RIGHT_SEND);
		kfree(dead_name_list);
		dead_name_list = next;
	}

	while (drain_head != NULL) {
		struct port_msg *next = drain_head->m_next;
		free_pending_descs(drain_head->m_descs,
		    drain_head->m_ndescs);
		kfree(drain_head);
		drain_head = next;
	}

	while (wake_head != NULL) {
		struct thread *next = wake_head->th_wait_link;
		wake_head->th_wait_link = NULL;
		thread_wake(wake_head);
		thread_unhold(wake_head);
		wake_head = next;
	}

	while (send_wake_head != NULL) {
		struct thread *next = send_wake_head->th_wait_link;
		send_wake_head->th_wait_link = NULL;
		thread_wake(send_wake_head);
		thread_unhold(send_wake_head);
		send_wake_head = next;
	}

	/* Leave the set, so it holds no pointer to a freed port. */
	if (member_of != NULL)
		port_leave_set(p, member_of);

	if (last)
		port_free(p);
}

/* ---- port_set object ------------------------------------------------ */

struct port_set *
port_set_create(void)
{
	struct port_set	*ps;

	ps = kmalloc(sizeof(*ps));
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

void
port_set_free(struct port_set *ps)
{

	KASSERT(ps->ps_refs == 0, "port_set_free: refs != 0");
	KASSERT(ps->ps_member_count == 0,
	    "port_set_free: members still attached");
	kfree(ps);
}

void
port_set_ref(struct port_set *ps)
{

	spin_lock(&ps->ps_lock);
	ps->ps_refs++;
	spin_unlock(&ps->ps_lock);
}

void
port_set_deref(struct port_set *ps)
{
	bool		 last;
	struct port	*detach_head = NULL;
	struct thread	*wake_head = NULL;
	struct thread	*hw;

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
		/* Held under ps_lock for the reap window, as in port_deref. */
		for (hw = wake_head; hw != NULL; hw = hw->th_wait_link)
			thread_hold(hw);
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
		struct thread *next = wake_head->th_wait_link;
		wake_head->th_wait_link = NULL;
		thread_wake(wake_head);
		thread_unhold(wake_head);
		wake_head = next;
	}

	if (last)
		port_set_free(ps);
}

/* ---- special-port plumbing ------------------------------------------ */

int
port_install_task_self(struct task *t)
{
	struct port		*p;
	mach_port_name_t	 name;
	int			 rv;

	if (t == NULL || t->t_port_space == NULL)
		return (MACH_E_INVAL);
	if (t->t_self_port != NULL)
		return (MACH_MSG_OK);		/* already installed */

	p = port_create();
	if (p == NULL)
		return (MACH_E_NOMEM);

	/*
	 * Tag before the SEND is visible: mach_msg_send picks its path
	 * by p_special, so no send can ever queue on this port.
	 */
	p->p_special     = PORT_SPECIAL_TASK_SELF;
	/*
	 * The task's id, not a pointer.  Outside SEND holders (launchctl,
	 * the launchd keep_alive worker, the shell's child registry) keep
	 * the port alive past the task's kfree, and a pointer would dangle
	 * in the dispatch and SYS_TASK_KILL paths.  task_lookup_ref returns
	 * NULL once the task is reaped.  Ids start at 1 (next_task_id), so
	 * the value is never NULL.
	 */
	KASSERT(t->t_id != 0, "port_install_task_self: task id 0");
	p->p_special_arg = (void *)(uintptr_t)t->t_id;

	port_ref(p, MACH_PORT_RIGHT_RECEIVE);

	rv = space_install(t->t_port_space, p, MACH_PORT_RIGHT_SEND, &name);
	if (rv != MACH_MSG_OK) {
		port_deref(p, MACH_PORT_RIGHT_RECEIVE);
		return (rv);
	}

	/*
	 * The first slot of a fresh space (ps_hint starts at 1).  Any
	 * other name means the space was not empty, which would break the
	 * well-known-name ABI.
	 */
	if (name != MACH_PORT_TASK_SELF) {
		panic("port_install_task_self: name %u, expected %u "
		    "(was the port_space pre-populated?)",
		    (unsigned)name, (unsigned)MACH_PORT_TASK_SELF);
	}

	t->t_self_port = p;
	return (MACH_MSG_OK);
}

void
port_release_task_self(struct task *t)
{
	struct port	*p;

	if (t == NULL)
		return;
	p = t->t_self_port;
	if (p == NULL)
		return;
	t->t_self_port = NULL;
	port_deref(p, MACH_PORT_RIGHT_RECEIVE);
}

/*
 * Link a DEAD_NAME watch node onto `watched`, for both
 * port_arm_dead_name_object and port_request_notification; both
 * allocate the node and take the SEND ref beforehand, so nothing
 * allocates under p_lock.  Ownership contract in port_internal.h.  Only
 * watched->p_lock is taken.
 */
int
port_dead_name_link(struct port *watched, struct port *notify,
    struct port_notify_node *node, uint32_t tag, bool *was_dup)
{
	struct port_notify_node	*n;

	*was_dup = false;

	spin_lock(&watched->p_lock);
	if (watched->p_dead || !watched->p_has_receive) {
		spin_unlock(&watched->p_lock);
		return (MACH_E_DEAD);
	}
	for (n = watched->p_notify_dead_name; n != NULL; n = n->nn_next) {
		if (n->nn_port == notify) {
			n->nn_tag = tag;
			*was_dup = true;
			spin_unlock(&watched->p_lock);
			return (MACH_MSG_OK);
		}
	}
	node->nn_port = notify;
	node->nn_tag  = tag;
	node->nn_pad  = 0;
	node->nn_next = watched->p_notify_dead_name;
	watched->p_notify_dead_name = node;
	spin_unlock(&watched->p_lock);
	return (MACH_MSG_OK);
}

/*
 * DEAD_NAME arming on port objects rather than names, for an in-kernel
 * watcher that already holds the pointers (the launchd keep_alive
 * worker).  Appends a watch, one per `notify`, holding a SEND ref on it
 * that goes when the notification fires.  MACH_E_DEAD if `watched` is
 * already dead, MACH_E_NOMEM if no node.  The ref and the node are taken
 * before watched->p_lock.
 */
int
port_arm_dead_name_object(struct port *watched, struct port *notify,
    uint32_t tag)
{
	struct port_notify_node	*node;
	bool			 dup;
	int			 rv;

	if (watched == NULL || notify == NULL)
		return (MACH_E_INVAL);

	node = kmalloc(sizeof(*node));
	if (node == NULL)
		return (MACH_E_NOMEM);

	port_ref(notify, MACH_PORT_RIGHT_SEND);
	rv = port_dead_name_link(watched, notify, node, tag, &dup);
	if (rv != MACH_MSG_OK || dup) {
		port_deref(notify, MACH_PORT_RIGHT_SEND);
		kfree(node);
	}
	return (rv);
}

/*
 * Create a port the kernel itself owns (RECEIVE held, named in no
 * space) with its p_special tag set: the bootstrap port, the host port,
 * the service and driver control ports.
 */
struct port *
port_create_kernel_owned(uint8_t special_kind, void *special_arg)
{
	struct port	*p;

	p = port_create();
	if (p == NULL)
		return (NULL);

	p->p_special     = special_kind;
	p->p_special_arg = special_arg;
	port_ref(p, MACH_PORT_RIGHT_RECEIVE);
	return (p);
}

/*
 * Install a SEND right for `p` at a new name in kernel_space, returned
 * in *name_out, so the kernel can hand the port out by COPY_SEND.
 * Used for the bootstrap, host and device control ports.
 */
int
port_install_send_in_kernel(struct port *p, mach_port_name_t *name_out)
{

	if (p == NULL || name_out == NULL)
		return (MACH_E_INVAL);
	return (space_install(kernel_space, p, MACH_PORT_RIGHT_SEND,
	    name_out));
}

/*
 * Install a SEND right to the global bootstrap port at
 * MACH_PORT_BOOTSTRAP in t->t_port_space; like port_install_task_self,
 * but the port is shared by all tasks.
 */
int
port_install_bootstrap(struct task *t)
{
	struct port		*p;
	mach_port_name_t	 name;
	int			 rv;

	if (t == NULL || t->t_port_space == NULL)
		return (MACH_E_INVAL);

	p = bootstrap_get_port();
	if (p == NULL)
		return (MACH_E_DEAD);	/* bootstrap_init has not run */

	rv = space_install(t->t_port_space, p, MACH_PORT_RIGHT_SEND, &name);
	if (rv != MACH_MSG_OK)
		return (rv);

	if (name != MACH_PORT_BOOTSTRAP) {
		panic("port_install_bootstrap: name %u, expected %u "
		    "(was MACH_PORT_TASK_SELF installed first?)",
		    (unsigned)name, (unsigned)MACH_PORT_BOOTSTRAP);
	}
	return (MACH_MSG_OK);
}

/* ---- diagnostics ----------------------------------------------------- */

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
	case MACH_E_TIMEOUT:	return ("timed out");
	case MACH_E_INTR:	return ("interrupted by a kill");
	default:		return ("?");
	}
}
