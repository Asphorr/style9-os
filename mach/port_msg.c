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
#include "clock.h"
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
 * Message-passing layer for the Mach IPC subsystem.
 *
 * Owns:
 *	- the FIFO and pending-descriptor housekeeping for in-flight
 *	  messages,
 *	- mach_msg_send (with the special-port and inline-reply fast
 *	  paths),
 *	- mach_msg_recv / recv_block / recv_timed,
 *	- mach_msg_rpc (which arms the stash on the reply port so the
 *	  fast path inside mach_msg_send can deliver the reply with
 *	  zero kmalloc).
 *
 * Object and name-table operations are in sibling files; their shared
 * helpers (port_create, space_lookup, ...) come in through
 * port_internal.h.
 *
 * Synchronous dispatcher hooks (task_self_dispatch in task.c,
 * bootstrap_dispatch in bootstrap.c) are picked up via the regular
 * public headers; mach_msg_send's special-port intercept calls them
 * in the sender's context.
 */

/* ---- message-queue helpers ----------------------------------------- */

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

/*
 * Release the per-port refs each pending descriptor in `descs` holds,
 * then free the array.  Used by the failure path of mach_msg_send (to
 * unwind partial descriptor work) and by port_free in port_object.c
 * to drain unconsumed queued messages.
 */
void
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
	 * Inline-reply fast path.  If the destination has an armed stash
	 * AND the message is bare (no descriptors, no implicit reply
	 * port), write the bytes straight into the stash buffer and set
	 * p_stash_rv = OK.  This is the synchronous-RPC rendezvous: the
	 * armed thread is mid-mach_msg_rpc inside the same call stack,
	 * not parked, so no wake is needed -- it reads p_stash_rv after
	 * its mach_msg_send returns.  Zero kmalloc, zero enqueue.
	 *
	 * MOVE_* dispositions still drop the sender's right so the
	 * post-send sender's space is identical to the slow-path case.
	 */
	if (!complex && !has_local) {
		size_t	want_size;

		want_size = msg->msgh_size;
		spin_lock(&dest->p_lock);
		if (dest->p_stash_buf != NULL &&
		    dest->p_stash_rv == MACH_E_NOMSG &&
		    !dest->p_dead &&
		    want_size <= dest->p_stash_size) {
			uint8_t			*dst;
			const uint8_t		*src2;
			struct mach_msg_header	*sh;

			dst  = (uint8_t *)dest->p_stash_buf;
			src2 = (const uint8_t *)msg;
			for (i = 0; i < want_size; i++)
				dst[i] = src2[i];
			/*
			 * Sender filled msgh_local with their own name; the
			 * stash receiver has no use for it (the reply port
			 * name belongs to the receiver's space, but the
			 * sender's view doesn't translate).  Zero it so the
			 * receiver doesn't accidentally space_drop a name it
			 * never owned.
			 */
			sh = (struct mach_msg_header *)dst;
			sh->msgh_local = MACH_PORT_NULL;

			dest->p_stash_rv = MACH_MSG_OK;
			spin_unlock(&dest->p_lock);

			if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
			    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
				(void)space_drop_one_right(from,
				    msg->msgh_remote, remote_right);
			}
			return (MACH_MSG_OK);
		}
		spin_unlock(&dest->p_lock);
	}

	/*
	 * Special-port intercept.  When the destination is a kernel
	 * object (task_self_port etc.) we never queue: the dispatcher
	 * synthesises any reply directly and returns.  Honour MOVE_*
	 * by dropping the sender's right first so the dispatcher
	 * cannot observe a stale name in the sender's space.
	 */
	if (dest->p_special != PORT_SPECIAL_NONE) {
		if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
		    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
			(void)space_drop_one_right(from,
			    msg->msgh_remote, remote_right);
		}
		switch (dest->p_special) {
		case PORT_SPECIAL_TASK_SELF:
			return (task_self_dispatch(
			    (struct task *)dest->p_special_arg, msg, from));
		case PORT_SPECIAL_BOOTSTRAP:
			return (bootstrap_dispatch(msg, from));
		case PORT_SPECIAL_SERVICE: {
			port_service_fn fn;
			fn = (port_service_fn)(uintptr_t)dest->p_special_arg;
			return (fn(msg, from));
		}
		default:
			return (MACH_E_INVAL);
		}
	}

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
				(void)space_drop_one_right(to, pds[i].name,
				    m->m_descs[i].pd_disposition);
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
					(void)space_drop_one_right(to,
					    pds[i].name,
					    m->m_descs[i].pd_disposition);
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
 *
 * Inline-reply optimisation:
 *
 *	Before issuing the send, we arm the reply port's "stash": pointer
 *	to the caller's reply_buf and its size, with rv=NOMSG.  If the
 *	send's downstream dispatcher (PORT_SPECIAL_*) runs synchronously
 *	in this thread and replies via mach_msg_send to the reply port,
 *	the send-side fast path writes the bytes straight into reply_buf
 *	and sets the stash rv to OK.  No port_msg ever allocated.  We
 *	then disarm and check the rv -- if filled, return without ever
 *	calling mach_msg_recv_timed.
 *
 *	Async senders (a parked server thread that will reply later) take
 *	the slow path: the send-side fast path test fails (stash is still
 *	armed when the dispatcher returns, but the dispatcher itself
 *	enqueues -- no, dispatcher only fast-paths the bare reply case;
 *	queued sends still queue).  So disarm before recv, then recv runs
 *	normally against the FIFO.
 */
int
mach_msg_rpc(struct port_space *space, struct mach_msg_header *req,
    struct mach_msg_header *reply_buf, size_t reply_buf_size,
    uint64_t timeout_ms)
{
	mach_port_name_t	reply_name;
	struct port		*reply_port;
	uint32_t		remote_disp;
	uint8_t			dummy_rights;
	int			stash_rv;
	int			rv;

	if (space == NULL || req == NULL || reply_buf == NULL)
		return (MACH_E_INVAL);
	if (reply_buf_size < sizeof(struct mach_msg_header))
		return (MACH_E_INVAL);

	reply_name = port_allocate(space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (reply_name == MACH_PORT_NULL)
		return (MACH_E_NOMEM);

	reply_port = space_lookup(space, reply_name,
	    MACH_PORT_RIGHT_RECEIVE, &dummy_rights);
	if (reply_port == NULL) {
		/* Should not happen -- we just allocated it. */
		(void)port_deallocate(space, reply_name);
		return (MACH_E_INVAL);
	}

	/*
	 * Arm the stash.  mach_msg_send's fast path will overwrite
	 * p_stash_rv to MACH_MSG_OK on synchronous bare-reply delivery;
	 * if the dispatcher chose to enqueue (or replied with a complex
	 * message), p_stash_rv stays NOMSG and we fall through to recv.
	 */
	spin_lock(&reply_port->p_lock);
	reply_port->p_stash_buf  = reply_buf;
	reply_port->p_stash_size = reply_buf_size;
	reply_port->p_stash_rv   = MACH_E_NOMSG;
	spin_unlock(&reply_port->p_lock);

	remote_disp = MACH_MSGH_BITS_REMOTE(req->msgh_bits);
	req->msgh_bits  = MACH_MSGH_BITS(remote_disp,
	    MACH_MSG_TYPE_MAKE_SEND);
	req->msgh_local = reply_name;

	rv = mach_msg_send(space, req);

	/*
	 * Disarm regardless of how the send went; if the fast path fired
	 * stash_rv is MACH_MSG_OK and the reply bytes are already in
	 * reply_buf.
	 */
	spin_lock(&reply_port->p_lock);
	stash_rv = reply_port->p_stash_rv;
	reply_port->p_stash_buf  = NULL;
	reply_port->p_stash_size = 0;
	reply_port->p_stash_rv   = MACH_E_NOMSG;
	spin_unlock(&reply_port->p_lock);

	if (rv != MACH_MSG_OK) {
		(void)port_deallocate(space, reply_name);
		return (rv);
	}

	if (stash_rv == MACH_MSG_OK) {
		/* Fast-path delivery already wrote into reply_buf. */
		(void)port_deallocate(space, reply_name);
		return (MACH_MSG_OK);
	}

	rv = mach_msg_recv_timed(space, reply_name, reply_buf,
	    reply_buf_size, timeout_ms);
	(void)port_deallocate(space, reply_name);
	return (rv);
}
