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
 * Object-lifecycle layer for the Mach IPC subsystem.
 *
 * Responsibilities:
 *	- port + port_set construction, destruction, and ref counting,
 *	- bootstrap of the subsystem (kernel_space + ID counters),
 *	- "special-port" plumbing for kernel-implemented Mach objects
 *	  (task_self, bootstrap, kernel-owned service ports).
 *
 * Message queueing, send/recv, and name-table operations live in
 * sibling files (port_msg.c, port_space.c).  Cross-file helpers are
 * declared in port_internal.h; everything in this file that doesn't
 * appear there stays static.
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

/*
 * Forward decl: free_pending_descs lives in port_msg.c because it's
 * primarily a deliver/recv helper; port_free invokes it on teardown
 * to release refs left in unconsumed queued messages.
 */
extern void	free_pending_descs(struct port_pending_desc *, size_t n);

struct port *
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
	p->p_special      = PORT_SPECIAL_NONE;
	p->p_special_arg  = NULL;
	p->p_stash_buf    = NULL;
	p->p_stash_size   = 0;
	p->p_stash_rv     = MACH_E_NOMSG;
	p->p_notify_no_senders     = NULL;
	p->p_notify_dead_name      = NULL;
	p->p_notify_no_senders_id  = 0;
	p->p_notify_dead_name_id   = 0;
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

void
port_deref(struct port *p, uint8_t rights)
{
	bool	last;
	struct port_msg *drain_head = NULL;
	struct thread	*wake_head = NULL;
	struct thread	*send_wake_head = NULL;
	struct port	*notify_no_senders = NULL;
	struct port	*notify_dead_name = NULL;
	uint32_t	 notify_no_senders_id = 0;
	uint32_t	 notify_dead_name_id = 0;
	bool		 fire_no_senders = false;
	bool		 fire_dead_name = false;

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

		/*
		 * NO_SENDERS slot: receiver going away means the
		 * no-senders event can no longer fire on this port.
		 * Snapshot+clear so the post-unlock cleanup releases
		 * our SEND ref on the notify target without firing.
		 */
		if (p->p_notify_no_senders != NULL) {
			notify_no_senders = p->p_notify_no_senders;
			p->p_notify_no_senders    = NULL;
			p->p_notify_no_senders_id = 0;
		}
		/*
		 * DEAD_NAME slot: receiver going away IS the trigger.
		 * Snapshot + tag, mark for firing, release the SEND ref
		 * post-unlock (after the notification is delivered).
		 */
		if (p->p_notify_dead_name != NULL) {
			notify_dead_name    = p->p_notify_dead_name;
			notify_dead_name_id = p->p_notify_dead_name_id;
			p->p_notify_dead_name    = NULL;
			p->p_notify_dead_name_id = 0;
			fire_dead_name = true;
		}
	}

	/*
	 * No-senders detection.  If dropping a send-bearing right took
	 * the per-port sender count to zero while the receive end is
	 * still held, either:
	 *	a notification was armed -- fire it (one-shot); the port
	 *	  stays alive so the receiver may MAKE_SEND a fresh right
	 *	  inside the notify handler if it chooses;
	 *	no notification -- fall back to the v0 behaviour: mark
	 *	  the port dead and wake any recv-blocked threads with
	 *	  MACH_E_DEAD so they aren't stranded waiting for a
	 *	  message that can never arrive.
	 *
	 * Set-waiters are not woken in the fallback path: the set as a
	 * whole stays alive as long as any other member still has
	 * senders, and we have no cheap way to certify that condition
	 * here.
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
		 * Release the kernel-held SEND ref on the notify port.
		 * Covers both the firing path (one-shot consumes the
		 * registration) and the RECV-drop cleanup path (source
		 * died with a registration still armed).
		 */
		port_deref(notify_no_senders, MACH_PORT_RIGHT_SEND);
	}
	if (fire_dead_name) {
		(void)port_notify_enqueue(notify_dead_name,
		    MACH_NOTIFY_DEAD_NAME, notify_dead_name_id);
	}
	if (notify_dead_name != NULL)
		port_deref(notify_dead_name, MACH_PORT_RIGHT_SEND);

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

/* ---- port_set object ------------------------------------------------ */

struct port_set *
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
	 * Tag the port before any visibility: once SEND lands in the
	 * task's space a sender could resolve it through space_lookup
	 * and reach mach_msg_send, which gates its intercept on
	 * p_special.  Setting the tag first means there is no window in
	 * which a regular queue path could fire against a port that is
	 * really meant for synchronous dispatch.
	 */
	p->p_special     = PORT_SPECIAL_TASK_SELF;
	/*
	 * Store the task's immutable id, NOT a raw struct task *.  A
	 * task-self port can outlive its task: any external holder of a
	 * SEND right (launchctl, the launchd keep_alive worker, the
	 * shell's child registry) keeps the port object alive past the
	 * task's kfree in task_deref.  A stored pointer would dangle and
	 * the dispatch + SYS_TASK_KILL paths would dereference freed
	 * memory.  The id is stable for the life of the task and resolves
	 * via task_lookup_ref, which returns NULL once the task has been
	 * reaped -- so a stale port fails safe.  Task ids start at 1
	 * (next_task_id in task.c), so the value never collides with the
	 * NULL p_special_arg of a non-special port.
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
	 * The well-known name is by construction the first slot allocated
	 * in a fresh port_space (ps_hint starts at 1).  Anything else
	 * means a caller put something else into the space first, which
	 * would break the ABI everyone else relies on -- panic instead of
	 * letting a wrong name leak out.
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
 * port_arm_dead_name_object: kernel-internal DEAD_NAME arming on port
 * objects rather than names.  The userspace path
 * (port_request_notification) resolves names in a space + checks the
 * caller's rights; an in-kernel watcher (the launchd keep_alive worker)
 * already holds port pointers, so it arms directly.
 *
 * Takes one SEND ref on `notify` for the registration; that ref is
 * dropped automatically when the one-shot fires (port_deref's
 * fire_dead_name path) or when `watched`'s RECEIVE is finally released
 * without firing.  Replacing an existing registration drops the old
 * notify ref.  Returns MACH_E_DEAD if `watched` is already dead (the
 * event the caller wanted can no longer be observed).
 *
 * Lock-order: ref `notify` BEFORE taking watched->p_lock so the two
 * per-port locks never nest.  The old-notify deref happens after the
 * unlock for the same reason.
 */
int
port_arm_dead_name_object(struct port *watched, struct port *notify,
    uint32_t tag)
{
	struct port	*old;

	if (watched == NULL || notify == NULL)
		return (MACH_E_INVAL);

	port_ref(notify, MACH_PORT_RIGHT_SEND);

	spin_lock(&watched->p_lock);
	if (watched->p_dead || !watched->p_has_receive) {
		spin_unlock(&watched->p_lock);
		port_deref(notify, MACH_PORT_RIGHT_SEND);
		return (MACH_E_DEAD);
	}
	old = watched->p_notify_dead_name;
	watched->p_notify_dead_name    = notify;
	watched->p_notify_dead_name_id = tag;
	spin_unlock(&watched->p_lock);

	if (old != NULL)
		port_deref(old, MACH_PORT_RIGHT_SEND);
	return (MACH_MSG_OK);
}

/*
 * Mint a port object the kernel itself owns (RECEIVE held, no name in
 * any port_space) with a p_special tag pre-set.  Used to mint the
 * singleton bootstrap port; future kernel-implemented Mach objects
 * (host_self etc.) can use the same factory.
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
 * Install a SEND right for `p` in kernel_space at the next-free name
 * and return that name in *name_out.  Used by services_init to wire
 * each kernel-side service port into kernel_space so bootstrap can
 * hand it out via COPY_SEND descriptors at lookup time.
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
 * Install a SEND right to the global bootstrap port at name
 * MACH_PORT_BOOTSTRAP in t->t_port_space.  Mirrors
 * port_install_task_self in shape; differs in that the underlying
 * port object is singleton-shared rather than per-task.
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
	case MACH_E_TIMEOUT:	return ("recv timed out");
	default:		return ("?");
	}
}
