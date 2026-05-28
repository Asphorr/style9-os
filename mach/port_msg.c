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
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "port_internal.h"
#include "sched.h"
#include "smap.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"
#include "vm.h"

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
	/*
	 * Forward-compat strictness: reject senders that scribble on
	 * msgh_bits reserved bits or on msgh_voucher (reserved for the
	 * unimplemented voucher capability).  A future revision will
	 * extend MACH_MSGH_BITS_USED_MASK + define voucher semantics;
	 * callers compiled against an older spec will fail fast with
	 * MACH_E_INVAL the moment they cross the boundary, rather than
	 * having their stray bits silently reinterpreted.
	 */
	if ((h->msgh_bits & ~MACH_MSGH_BITS_USED_MASK) != 0)
		return (MACH_E_INVAL);
	if (h->msgh_voucher != 0)
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
		if (descs[i].pd_type == MACH_MSG_PORT_DESCRIPTOR &&
		    descs[i].pd_port != NULL) {
			port_deref(descs[i].pd_port,
			    descs[i].pd_disposition);
		} else if (descs[i].pd_type == MACH_MSG_OOL_DESCRIPTOR &&
		    descs[i].pd_ool_buf != NULL) {
			kfree(descs[i].pd_ool_buf);
		}
	}
	kfree(descs);
}

/* ---- notifications --------------------------------------------------- */

/*
 * Synthesise a notification message and enqueue it on `notify_port`.
 * Called from port_deref's no-senders detection (and, eventually, the
 * dead-name path) with a local SEND-ref already held on notify_port so
 * the receiver does not race the firing against the destination's
 * teardown.  Caller owns that ref -- this routine does not drop it.
 *
 * Best-effort: a dead notify port (receiver gone) or a full queue
 * silently drops the notification.  v1 has no return-to-source backstop;
 * a future revision can stash the notification on a kernel-side dead
 * letter queue if real Mach semantics become important.
 */
int
port_notify_enqueue(struct port *notify_port, uint32_t notify_id,
    uint32_t user_tag)
{
	struct port_msg			*m;
	struct mach_notify_header	*nh;
	struct thread			*waiter = NULL;
	int				 rv;

	if (notify_port == NULL)
		return (MACH_E_INVAL);

	m = (struct port_msg *)kmalloc(sizeof(*m) +
	    sizeof(struct mach_notify_header));
	if (m == NULL)
		return (MACH_E_NOMEM);

	m->m_next   = NULL;
	m->m_size   = sizeof(struct mach_notify_header);
	m->m_ndescs = 0;
	m->m_descs  = NULL;

	nh = (struct mach_notify_header *)m->m_buf;
	nh->hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	nh->hdr.msgh_size    = sizeof(struct mach_notify_header);
	nh->hdr.msgh_remote  = MACH_PORT_NULL;
	nh->hdr.msgh_local   = MACH_PORT_NULL;
	nh->hdr.msgh_voucher = 0;
	nh->hdr.msgh_id      = notify_id;
	nh->nh_msgid         = user_tag;
	nh->nh_pad           = 0;

	rv = msg_enqueue(notify_port, m, &waiter);
	if (rv != MACH_MSG_OK) {
		kfree(m);
		return (rv);
	}
	if (waiter != NULL)
		thread_wake(waiter);
	return (MACH_MSG_OK);
}

int
port_exception_post(struct port *port, uint32_t trapno, uint32_t err,
    uint64_t rip, uint64_t rsp, uint64_t rflags, uint64_t cr2,
    uint64_t task_id, struct port *reply_port)
{
	struct port_msg			*m;
	struct port_pending_desc	*descs = NULL;
	struct mach_exception_header	*eh;
	struct thread			*waiter = NULL;
	uint8_t				 local_disp = 0;
	int				 rv;

	if (port == NULL)
		return (MACH_E_INVAL);

	m = (struct port_msg *)kmalloc(sizeof(*m) +
	    sizeof(struct mach_exception_header));
	if (m == NULL)
		return (MACH_E_NOMEM);

	/*
	 * When the watcher opted into the reply protocol the caller
	 * hands us a kernel-owned reply port; attach it as an implicit
	 * msgh_local descriptor with MAKE_SEND disposition so the
	 * watcher's deliver_msg installs a SEND name in their space.
	 * The receiver replies through that name; the kernel-side
	 * recv on `reply_port` (from the caller) wakes on the verdict.
	 *
	 * Ref accounting: take one SEND ref here for the message
	 * in-flight; deliver_msg drops it after installing the
	 * receiver's name, so refs balance whether or not delivery
	 * succeeds (the free_pending_descs failure path also drops
	 * via port_deref).
	 */
	if (reply_port != NULL) {
		descs = (struct port_pending_desc *)kcalloc(1,
		    sizeof(struct port_pending_desc));
		if (descs == NULL) {
			kfree(m);
			return (MACH_E_NOMEM);
		}
		descs[0].pd_type        = MACH_MSG_PORT_DESCRIPTOR;
		descs[0].pd_disposition = MACH_PORT_RIGHT_SEND;
		descs[0].pd_port        = reply_port;
		descs[0].pd_ool_buf     = NULL;
		descs[0].pd_ool_size    = 0;
		port_ref(reply_port, MACH_PORT_RIGHT_SEND);
		local_disp = MACH_MSG_TYPE_MAKE_SEND;
	}

	m->m_next   = NULL;
	m->m_size   = sizeof(struct mach_exception_header);
	m->m_ndescs = (reply_port != NULL) ? 1 : 0;
	m->m_descs  = descs;

	eh = (struct mach_exception_header *)m->m_buf;
	eh->hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    local_disp);
	eh->hdr.msgh_size    = sizeof(struct mach_exception_header);
	eh->hdr.msgh_remote  = MACH_PORT_NULL;
	eh->hdr.msgh_local   = MACH_PORT_NULL;
	eh->hdr.msgh_voucher = 0;
	eh->hdr.msgh_id      = MACH_EXC_FAULT;
	eh->eh_trapno        = trapno;
	eh->eh_err           = err;
	eh->eh_rip           = rip;
	eh->eh_rsp           = rsp;
	eh->eh_rflags        = rflags;
	eh->eh_cr2           = cr2;
	eh->eh_task_id       = task_id;

	rv = msg_enqueue(port, m, &waiter);
	if (rv != MACH_MSG_OK) {
		free_pending_descs(descs, m->m_ndescs);
		kfree(m);
		return (rv);
	}
	if (waiter != NULL)
		thread_wake(waiter);
	return (MACH_MSG_OK);
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

	pd->pd_type     = MACH_MSG_PORT_DESCRIPTOR;
	pd->pd_ool_buf  = NULL;
	pd->pd_ool_size = 0;

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

/* ---- OOL helpers --------------------------------------------------- */

/*
 * desc_step: one iteration of the variable-stride descriptor walker.
 * Reads the type tag at buf[off] and returns the descriptor's stride
 * (8 for port, 16 for OOL); writes the tag to *type_out.  Returns 0
 * if the type is unknown or the full descriptor would run past `cap`,
 * signalling MACH_E_INVAL to the caller without touching the buffer.
 */
static size_t
desc_step(const uint8_t *buf, size_t off, size_t cap, uint8_t *type_out)
{
	uint8_t	t;
	size_t	stride;

	if (off >= cap)
		return (0);
	t = buf[off];
	if (t == MACH_MSG_PORT_DESCRIPTOR)
		stride = sizeof(struct mach_msg_port_descriptor);
	else if (t == MACH_MSG_OOL_DESCRIPTOR)
		stride = sizeof(struct mach_msg_ool_descriptor);
	else
		return (0);
	if (off + stride > cap)
		return (0);
	*type_out = t;
	return (stride);
}

/*
 * send_capture_ool: on send, validate the OOL descriptor's sender VA
 * range, kmalloc a staging buffer the size of the payload, and copy
 * the bytes in.  The sender's pmap is current (sender is the calling
 * task), so reading from the user VA is a direct dereference.
 *
 * v1 normalises everything to MACH_MSG_PHYSICAL_COPY -- virtual-copy
 * descriptors get upgraded to physical at capture time so the recv
 * path only sees one shape.
 *
 * Per-descriptor `deallocate` IS honoured for ring-3 senders:
 * post-copy, if the sender set deallocate=1 AND the source range
 * mirrors a vm_allocate'd anonymous entry exactly, the kernel calls
 * vm_map_release on the sender's range so the user does not have to
 * race the send + manual vm_deallocate.  Non-matching ranges (stack,
 * static data, partial slices of a larger allocation) leave the
 * sender's VM untouched -- the flag is best-effort, not mandatory.
 * Kernel and trusted-send senders skip the hook (their addresses
 * are kernel-VA; nothing to vm_map_release).
 *
 * On success the pd carries an owned kmalloc'd buffer that the
 * receive path (or any cleanup path) is responsible for freeing.
 */
static int
send_capture_ool(struct port_space *from,
    const struct mach_msg_ool_descriptor *od,
    struct port_pending_desc *pd)
{
	struct task	*sender;
	uint8_t		*staging;
	uint32_t	 size;
	uint64_t	 addr;
	uint32_t	 i;

	(void)from;	/* sender == current task; from isn't needed */

	pd->pd_type     = MACH_MSG_OOL_DESCRIPTOR;
	pd->pd_port     = NULL;
	pd->pd_ool_buf  = NULL;
	pd->pd_ool_size = 0;
	pd->pd_ool_copy = MACH_MSG_PHYSICAL_COPY;

	size = od->size;
	addr = od->address;

	if (size == 0) {
		/* Zero-byte OOL: legal, just an empty buffer.  Receiver
		   will see address=0, size=0; no VM install needed. */
		return (MACH_MSG_OK);
	}
	if (size > MACH_MSG_OOL_MAX_BYTES)
		return (MACH_E_INVAL);
	if (od->copy != MACH_MSG_PHYSICAL_COPY &&
	    od->copy != MACH_MSG_VIRTUAL_COPY)
		return (MACH_E_INVAL);

	/*
	 * Validate the sender's address range is plausibly inside the
	 * task's user VA window.  This is a coarse check -- a perfect
	 * one would walk t_map for full coverage, but the subsequent
	 * memcpy will already fault on any unmapped page within the
	 * window, so the obvious wraparound + out-of-window cases are
	 * all that need explicit guarding here.
	 *
	 * Two exemptions, both for kernel-internal senders that ship
	 * blobs out of kmalloc'd / .rodata kernel addresses:
	 *
	 *	- kernel_task itself (kernel threads sending OOL)
	 *	- a thread with th_trusted_send set, toggled by
	 *	  mach_msg_send_trusted around a single send call
	 *	  (used by special-service dispatchers replying out of
	 *	  kernel rodata; see kern/thread.h)
	 *
	 * The kernel is trusted not to point at unmapped memory; if it
	 * ever does the subsequent memcpy triple-faults, which is a
	 * louder bug than the silent E_INVAL appropriate for ring-3
	 * senders.
	 */
	sender = current_thread != NULL ? current_thread->th_task : NULL;
	if (sender != NULL && sender != kernel_task && sender->t_map != NULL &&
	    !(current_thread != NULL && current_thread->th_trusted_send)) {
		uint64_t end = addr + size;
		if (end < addr)
			return (MACH_E_INVAL);	/* wraparound */
		if (addr < sender->t_map->vm_lo ||
		    end > sender->t_map->vm_hi)
			return (MACH_E_INVAL);
	}

	staging = (uint8_t *)kmalloc((size_t)size);
	if (staging == NULL)
		return (MACH_E_NOMEM);

	/*
	 * Sender's pmap is current; direct byte-by-byte copy.  Bracketed
	 * for SMAP so the kernel can read the U=1 source page once
	 * CR4.SMAP is enabled.
	 */
	{
		const uint8_t *src = (const uint8_t *)(uintptr_t)addr;
		smap_user_access_begin();
		for (i = 0; i < size; i++)
			staging[i] = src[i];
		smap_user_access_end();
	}

	pd->pd_ool_buf  = staging;
	pd->pd_ool_size = size;

	/*
	 * Honour the deallocate-on-send flag for ring-3 senders.  Skip
	 * for kernel-internal senders (kernel_task / trusted-send) since
	 * their `addr` is a kernel VA, not a managed user range.  Page-
	 * round the size before the lookup so a sub-page OOL (e.g. 100
	 * bytes out of a 4 KiB allocation) still matches the vm_map_entry
	 * that vm_allocate created.  Best-effort: a non-matching range
	 * silently leaves the sender's VM alone.
	 */
	if (od->deallocate != 0 && sender != NULL && sender != kernel_task &&
	    !(current_thread != NULL && current_thread->th_trusted_send)) {
		uint64_t aligned = ((uint64_t)size + 0xFFFull) & ~0xFFFull;
		(void)vm_map_release(sender->t_map, sender->t_pmap,
		    addr, aligned);
	}
	return (MACH_MSG_OK);
}

/*
 * apply_ool_deallocate: post-dispatch dealloc hook for the special-
 * port short-circuit path.
 *
 * The normal queue path runs through send_capture_ool, which honours
 * deallocate-on-send inline because it has already copied the bytes
 * into a kernel-side staging buffer.  Special-port destinations bypass
 * send_capture_ool: the dispatcher reads OOL bytes straight out of
 * the sender's pmap.  Releasing the source range BEFORE the dispatch
 * would yank the bytes out from under the dispatcher; releasing it
 * AFTER lets the dispatcher consume the bytes first.
 *
 * Caller (mach_msg_send's special-port branch) passes a kernel-VA
 * `msg` -- the upfront copyin already mirrored the user message into
 * m->m_buf, so this walk is pure kernel-VA bookkeeping.  Only the
 * vm_map_release calls touch user state, and they take the user-VA
 * `od->address` as data.
 */
static void
apply_ool_deallocate(const struct mach_msg_header *msg,
    struct port_space *from)
{
	const struct mach_msg_body	*body;
	struct task			*sender;
	const uint8_t			*p;
	size_t				 off;
	uint32_t			 i;
	uint32_t			 ndescs;
	uint8_t				 t;

	(void)from;

	if (current_thread == NULL)
		return;
	if (current_thread->th_trusted_send)
		return;
	sender = current_thread->th_task;
	if (sender == NULL || sender == kernel_task)
		return;
	if (sender->t_map == NULL || sender->t_pmap == NULL)
		return;

	if ((msg->msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0)
		return;
	if (msg->msgh_size < sizeof(struct mach_msg_header) +
	    sizeof(struct mach_msg_body))
		return;

	body = (const struct mach_msg_body *)
	    ((const uint8_t *)msg + sizeof(struct mach_msg_header));
	ndescs = body->msgh_descriptor_count;
	off    = sizeof(struct mach_msg_header) + sizeof(struct mach_msg_body);

	for (i = 0; i < ndescs; i++) {
		if (off >= msg->msgh_size)
			break;
		p = (const uint8_t *)msg + off;
		t = *p;

		if (t == MACH_MSG_PORT_DESCRIPTOR) {
			off += sizeof(struct mach_msg_port_descriptor);
		} else if (t == MACH_MSG_OOL_DESCRIPTOR) {
			const struct mach_msg_ool_descriptor	*od;
			uint64_t	 aligned;

			if (off + sizeof(struct mach_msg_ool_descriptor) >
			    msg->msgh_size)
				break;
			od = (const struct mach_msg_ool_descriptor *)p;
			if (od->deallocate != 0 && od->size > 0) {
				aligned = ((uint64_t)od->size + 0xFFFull) &
				    ~0xFFFull;
				(void)vm_map_release(sender->t_map,
				    sender->t_pmap, od->address, aligned);
			}
			off += sizeof(struct mach_msg_ool_descriptor);
		} else {
			break;
		}
	}
}

/*
 * recv_rollback_ool: on recv-path failure, tear down a partially-
 * installed OOL range in the receiver: walk the per-page mappings,
 * remove from pmap, free the underlying frames, and drop the vm_map
 * entry that recorded the range.
 *
 * `installed_pages` is the page count actually installed before the
 * failure; the caller passes whatever it got through before erroring
 * so we don't unmap-then-free pages that were never inserted.
 */
static void
recv_rollback_ool(struct task *to, uint64_t landing_va,
    size_t installed_pages, size_t total_aligned)
{
	size_t	i;

	for (i = 0; i < installed_pages; i++) {
		uint64_t	va = landing_va + (uint64_t)i * PAGE_SIZE;
		uint64_t	pa;

		pa = pmap_extract(to->t_pmap, va);
		(void)pmap_remove(to->t_pmap, va);
		if (pa != PA_INVALID)
			pmm_free_page(pa);
	}
	if (total_aligned > 0)
		(void)vm_map_remove(to->t_map, landing_va, total_aligned);
}

/*
 * recv_install_ool: on recv, take the captured staging buffer for an
 * OOL descriptor and materialise it in the receiver's address space.
 *
 *	1. find_space picks a fresh VA in the receiver's vm_map
 *	2. for each page: pmm_alloc a frame, copy the matching slice
 *	   of staging into it, pmap_enter at the landing VA
 *	3. vm_map_enter records the new range so it survives across
 *	   subsequent vm_map walks (and so a future vm_deallocate can
 *	   find + tear it down)
 *	4. patch the receiver-visible OOL descriptor: address is now
 *	   the new VA, type/size remain as the sender sent them
 *	5. free the staging buffer -- the receiver owns the data now
 *
 * Rolls back via recv_rollback_ool on any per-step failure so a
 * partial install can never leak frames or a vm_map entry.
 */
static int
recv_install_ool(struct port_space *to_space,
    struct port_pending_desc *pd,
    struct mach_msg_ool_descriptor *od_in_buf)
{
	struct task	*to;
	uint64_t	 landing_va = 0;
	uint64_t	 aligned;
	size_t		 npages, i;
	uint32_t	 size;
	uint8_t		*staging;

	(void)to_space;
	to   = current_thread != NULL ? current_thread->th_task : NULL;
	size = pd->pd_ool_size;

	/* Always normalise these on the recv-side descriptor. */
	od_in_buf->type       = MACH_MSG_OOL_DESCRIPTOR;
	od_in_buf->copy       = MACH_MSG_PHYSICAL_COPY;
	od_in_buf->deallocate = 0;
	od_in_buf->pad        = 0;
	od_in_buf->size       = size;
	od_in_buf->address    = 0;

	if (size == 0)
		return (MACH_MSG_OK);

	if (to == NULL || to->t_map == NULL || to->t_pmap == NULL)
		return (MACH_E_INVAL);

	staging = (uint8_t *)pd->pd_ool_buf;
	if (staging == NULL)
		return (MACH_E_INVAL);

	aligned = ((uint64_t)size + PAGE_SIZE - 1u) & ~(uint64_t)PAGE_MASK;
	npages  = (size_t)(aligned >> PAGE_SHIFT);

	if (!vm_map_find_space(to->t_map, aligned, &landing_va))
		return (MACH_E_NOMEM);

	for (i = 0; i < npages; i++) {
		uint64_t	 va = landing_va + (uint64_t)i * PAGE_SIZE;
		uint64_t	 pa;
		uint8_t		*kva;
		size_t		 chunk, offset;
		size_t		 j;

		pa = pmm_alloc_page();
		if (pa == PA_INVALID) {
			recv_rollback_ool(to, landing_va, i, 0);
			return (MACH_E_NOMEM);
		}

		kva    = (uint8_t *)pmm_kva_from_pa(pa);
		offset = i * PAGE_SIZE;
		chunk  = PAGE_SIZE;
		if (offset + chunk > (size_t)size)
			chunk = (size_t)size - offset;
		for (j = 0; j < chunk; j++)
			kva[j] = staging[offset + j];
		for (j = chunk; j < PAGE_SIZE; j++)
			kva[j] = 0;

		if (!pmap_enter(to->t_pmap, va, pa,
		    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER)) {
			pmm_free_page(pa);
			recv_rollback_ool(to, landing_va, i, 0);
			return (MACH_E_NOMEM);
		}
	}

	if (!vm_map_enter(to->t_map, landing_va, aligned,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER, VME_F_ANON)) {
		recv_rollback_ool(to, landing_va, npages, 0);
		return (MACH_E_NOMEM);
	}

	od_in_buf->address = landing_va;

	/* Receiver owns the pages now; staging is no longer needed. */
	kfree(pd->pd_ool_buf);
	pd->pd_ool_buf  = NULL;
	pd->pd_ool_size = 0;
	return (MACH_MSG_OK);
}

int
mach_msg_send_trusted(struct port_space *from,
    const struct mach_msg_header *msg)
{
	int	rv;

	if (current_thread == NULL)
		return (mach_msg_send(from, msg));

	current_thread->th_trusted_send = true;
	rv = mach_msg_send(from, msg);
	current_thread->th_trusted_send = false;
	return (rv);
}

int
mach_msg_send(struct port_space *from, const struct mach_msg_header *umsg)
{
	struct mach_msg_header	 hdr_copy;
	const struct mach_msg_header *msg;
	int			 rv;
	struct port		*dest = NULL;
	struct port_msg		*m = NULL;
	struct port_pending_desc *descs = NULL;
	size_t			 ndescs = 0;
	size_t			 i, hdrs_off;
	uint8_t			 remote_disp, local_disp;
	uint8_t			 remote_right;
	bool			 has_local;
	bool			 complex;
	uint8_t			 dummy_rights;

	if (umsg == NULL)
		return (MACH_E_INVAL);

	/*
	 * Single-pass copyin pattern: read the user header under one
	 * SMAP bracket, validate the kernel-side copy, then allocate
	 * the queue buffer m sized to fit the whole message and
	 * copyin the entire body straight into m->m_buf.  After this
	 * block, `msg` aliases m->m_buf (kernel VA) and every
	 * subsequent header / body / descriptor read in this function
	 * (and in the dispatchers / OOL walker we call out to) is a
	 * plain kernel-VA dereference -- no further brackets needed.
	 *
	 * The destination buffer for the copyin is the queue buffer
	 * itself, so the message bytes are copied exactly once.  The
	 * special-port short-circuit pays for this kmalloc + kfree
	 * even though it does not enqueue m; that cost is one heap
	 * round-trip per dispatch, well below the wire-format walk
	 * we are already doing.
	 */
	smap_user_access_begin();
	hdr_copy = *umsg;
	smap_user_access_end();

	rv = msg_validate(&hdr_copy);
	if (rv != MACH_MSG_OK)
		return (rv);

	/*
	 * No body-bound check here: kernel-spawned worker tasks (stress
	 * harness, future kernel servers) send with kernel-VA `umsg`
	 * pointers even though sender != kernel_task, and a sender vs.
	 * t_map check would wrongly reject them.  Ring-3 entry points
	 * (sys_msg_send / sys_msg_rpc) bound umsg + msgh_size against
	 * the user-VA window before reaching here, so the body copyin
	 * below is safe for the syscall caller; kernel callers are
	 * trusted not to point at unmapped memory.
	 */

	remote_disp = MACH_MSGH_BITS_REMOTE(hdr_copy.msgh_bits);
	local_disp  = MACH_MSGH_BITS_LOCAL(hdr_copy.msgh_bits);
	complex     = (hdr_copy.msgh_bits & MACH_MSGH_BITS_COMPLEX) != 0;
	has_local   = (hdr_copy.msgh_local != MACH_PORT_NULL) &&
	    (local_disp != 0);

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

	m = (struct port_msg *)kmalloc(sizeof(*m) + hdr_copy.msgh_size);
	if (m == NULL)
		return (MACH_E_NOMEM);
	m->m_next   = NULL;
	m->m_size   = hdr_copy.msgh_size;
	m->m_ndescs = 0;
	m->m_descs  = NULL;

	smap_user_access_begin();
	{
		const uint8_t	*src;

		src = (const uint8_t *)umsg;
		for (i = 0; i < hdr_copy.msgh_size; i++)
			m->m_buf[i] = src[i];
	}
	smap_user_access_end();

	msg = (const struct mach_msg_header *)m->m_buf;

	dest = space_lookup(from, msg->msgh_remote, remote_right,
	    &dummy_rights);
	if (dest == NULL) {
		kfree(m);
		return (MACH_E_RIGHT);
	}

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

			dst  = (uint8_t *)dest->p_stash_buf;	/* user VA */
			src2 = (const uint8_t *)msg;		/* kernel VA */
			/*
			 * dest->p_stash_buf was armed by mach_msg_rpc and
			 * points into the receiver's caller-supplied user
			 * buffer.  Single SMAP bracket on the copy + the
			 * msgh_local zero so the inline-reply fast path
			 * touches user memory the same way deliver_msg
			 * does on the slow path.
			 */
			smap_user_access_begin();
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
			smap_user_access_end();

			dest->p_stash_rv = MACH_MSG_OK;
			spin_unlock(&dest->p_lock);

			if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
			    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
				(void)space_drop_one_right(from,
				    msg->msgh_remote, remote_right);
			}
			kfree(m);
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
		int	special_rv;

		if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
		    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
			(void)space_drop_one_right(from,
			    msg->msgh_remote, remote_right);
		}
		switch (dest->p_special) {
		case PORT_SPECIAL_TASK_SELF:
			special_rv = task_self_dispatch(
			    (struct task *)dest->p_special_arg, msg, from);
			break;
		case PORT_SPECIAL_BOOTSTRAP:
			special_rv = bootstrap_dispatch(msg, from);
			break;
		case PORT_SPECIAL_SERVICE: {
			port_service_fn fn;
			fn = (port_service_fn)(uintptr_t)dest->p_special_arg;
			special_rv = fn(msg, from);
			break;
		}
		default:
			special_rv = MACH_E_INVAL;
			break;
		}
		/*
		 * Dispatchers read msg (== m->m_buf) directly -- it is
		 * kernel VA, so any further OOL-source touches happen
		 * with the OOL descriptor's `address` already validated
		 * in the kernel copy.  apply_ool_deallocate honours
		 * deallocate-on-send for any OOL with the flag set; it
		 * walks m->m_buf, so no SMAP brackets are needed there
		 * either.
		 */
		apply_ool_deallocate(msg, from);
		kfree(m);
		return (special_rv);
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

	/*
	 * m is already allocated and m->m_buf already populated from
	 * the upfront copyin -- nothing to do here beyond setting the
	 * hdrs offset that the descriptor walker uses.
	 */
	hdrs_off = sizeof(struct mach_msg_header);

	if (complex) {
		const struct mach_msg_body *body;
		size_t			 walk_off;
		size_t			 walk_i;

		if (msg->msgh_size < hdrs_off +
		    sizeof(struct mach_msg_body)) {
			rv = MACH_E_INVAL;
			goto fail;
		}
		body = (const struct mach_msg_body *)
		    (m->m_buf + hdrs_off);
		ndescs = body->msgh_descriptor_count;
		if (ndescs > 0) {
			/*
			 * Variable-stride walk: confirm every descriptor's
			 * type tag is known and the full descriptor fits
			 * within msgh_size before we commit to processing.
			 */
			walk_off = hdrs_off + sizeof(struct mach_msg_body);
			for (walk_i = 0; walk_i < ndescs; walk_i++) {
				uint8_t	t;
				size_t	stride;
				stride = desc_step(m->m_buf, walk_off,
				    msg->msgh_size, &t);
				if (stride == 0) {
					rv = MACH_E_INVAL;
					goto fail;
				}
				walk_off += stride;
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
		size_t	off;
		size_t	explicit_descs;

		off = hdrs_off + sizeof(struct mach_msg_body);
		explicit_descs = has_local ? ndescs - 1 : ndescs;

		for (i = 0; i < explicit_descs; i++) {
			uint8_t	t = m->m_buf[off];

			if (t == MACH_MSG_PORT_DESCRIPTOR) {
				struct mach_msg_port_descriptor *pd =
				    (struct mach_msg_port_descriptor *)
				    (m->m_buf + off);
				rv = send_xlate_desc(from, pd->name,
				    pd->disposition, &descs[i]);
				if (rv != MACH_MSG_OK)
					goto fail;
				pd->name = MACH_PORT_NULL;
				off += sizeof(struct mach_msg_port_descriptor);
			} else if (t == MACH_MSG_OOL_DESCRIPTOR) {
				struct mach_msg_ool_descriptor *od =
				    (struct mach_msg_ool_descriptor *)
				    (m->m_buf + off);
				rv = send_capture_ool(from, od, &descs[i]);
				if (rv != MACH_MSG_OK)
					goto fail;
				od->address = 0;
				off += sizeof(struct mach_msg_ool_descriptor);
			} else {
				/* Should have been caught by the walk. */
				rv = MACH_E_INVAL;
				goto fail;
			}
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
	/* Releases port refs AND frees OOL staging buffers as appropriate. */
	free_pending_descs(descs, ndescs);
	if (m != NULL)
		kfree(m);
	if (dest != NULL)
		port_deref(dest, remote_right);
	return (rv);
}

/* ---- mach_msg_recv --------------------------------------------------- */

/*
 * recv_rollback_installed: undo the first `up_to` descriptors of an
 * in-flight delivery -- caller has already noticed an error on
 * descriptor `up_to` and needs to back out everything that succeeded
 * up to (but not including) it.
 *
 * For each previously-installed descriptor:
 *	PORT	  -- pds[k].name was just rewritten to a receiver-space
 *		     name; drop that right back out of the receiver.
 *	OOL	  -- od[k].address was just rewritten to a receiver VA;
 *		     unmap the range, free the frames, drop the vm_map
 *		     entry.
 *
 * After undoing the install side, walk the remaining descs[] to
 * release ports we never delivered + free OOL staging that's still
 * sitting in pd_ool_buf.
 */
static void
recv_rollback_installed(struct port_space *to, struct port_msg *m,
    size_t up_to)
{
	size_t	off;
	size_t	i;

	off = sizeof(struct mach_msg_header) + sizeof(struct mach_msg_body);
	for (i = 0; i < up_to; i++) {
		uint8_t	t = m->m_descs[i].pd_type;

		if (t == MACH_MSG_PORT_DESCRIPTOR) {
			struct mach_msg_port_descriptor *pd =
			    (struct mach_msg_port_descriptor *)
			    (m->m_buf + off);
			if (pd->name != MACH_PORT_NULL) {
				(void)space_drop_one_right(to, pd->name,
				    m->m_descs[i].pd_disposition);
				pd->name = MACH_PORT_NULL;
			}
			off += sizeof(struct mach_msg_port_descriptor);
		} else if (t == MACH_MSG_OOL_DESCRIPTOR) {
			struct mach_msg_ool_descriptor *od =
			    (struct mach_msg_ool_descriptor *)
			    (m->m_buf + off);
			if (od->address != 0 &&
			    current_thread != NULL &&
			    current_thread->th_task != NULL) {
				uint64_t aligned;
				size_t   npages;

				aligned = ((uint64_t)od->size + PAGE_SIZE - 1u)
				    & ~(uint64_t)PAGE_MASK;
				npages  = (size_t)(aligned >> PAGE_SHIFT);
				recv_rollback_ool(current_thread->th_task,
				    od->address, npages, aligned);
				od->address = 0;
			}
			off += sizeof(struct mach_msg_ool_descriptor);
		}
	}

	/* The descs[] entries past `up_to` never installed; free them. */
	for (i = up_to; i < m->m_ndescs; i++) {
		if (m->m_descs[i].pd_type == MACH_MSG_PORT_DESCRIPTOR &&
		    m->m_descs[i].pd_port != NULL) {
			port_deref(m->m_descs[i].pd_port,
			    m->m_descs[i].pd_disposition);
		} else if (m->m_descs[i].pd_type == MACH_MSG_OOL_DESCRIPTOR &&
		    m->m_descs[i].pd_ool_buf != NULL) {
			kfree(m->m_descs[i].pd_ool_buf);
		}
	}
}

static int
deliver_msg(struct port_space *to, struct port *p, struct port_msg *m,
    struct mach_msg_header *buf, size_t buf_size)
{
	struct mach_msg_header	*hdr;
	size_t			 hdrs_off, explicit_descs;
	size_t			 off;
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
	} else {
		explicit_descs = 0;
	}

	off = hdrs_off + sizeof(struct mach_msg_body);
	for (i = 0; i < explicit_descs; i++) {
		uint8_t	t = m->m_descs[i].pd_type;

		if (t == MACH_MSG_PORT_DESCRIPTOR) {
			struct mach_msg_port_descriptor *pd =
			    (struct mach_msg_port_descriptor *)
			    (m->m_buf + off);
			mach_port_name_t	n;
			uint8_t			kind;

			kind = m->m_descs[i].pd_disposition;
			/*
			 * RECEIVE descriptors carry the right itself rather
			 * than a pending ref -- install without a fresh
			 * port_ref, and skip the matching port_deref below.
			 * See send_xlate_desc MOVE_RECEIVE for the symmetry.
			 */
			if (kind == MACH_PORT_RIGHT_RECEIVE)
				rv = space_install_no_ref(to,
				    m->m_descs[i].pd_port, kind, &n);
			else
				rv = space_install(to,
				    m->m_descs[i].pd_port, kind, &n);
			if (rv != MACH_MSG_OK) {
				recv_rollback_installed(to, m, i);
				if (m->m_descs != NULL)
					kfree(m->m_descs);
				kfree(m);
				return (rv);
			}
			if (kind != MACH_PORT_RIGHT_RECEIVE)
				port_deref(m->m_descs[i].pd_port, kind);
			m->m_descs[i].pd_port = NULL;
			pd->name = n;
			off += sizeof(struct mach_msg_port_descriptor);
		} else if (t == MACH_MSG_OOL_DESCRIPTOR) {
			struct mach_msg_ool_descriptor *od =
			    (struct mach_msg_ool_descriptor *)
			    (m->m_buf + off);
			rv = recv_install_ool(to, &m->m_descs[i], od);
			if (rv != MACH_MSG_OK) {
				recv_rollback_installed(to, m, i);
				if (m->m_descs != NULL)
					kfree(m->m_descs);
				kfree(m);
				return (rv);
			}
			off += sizeof(struct mach_msg_ool_descriptor);
		} else {
			/* Should have been caught on send. */
			recv_rollback_installed(to, m, i);
			if (m->m_descs != NULL)
				kfree(m->m_descs);
			kfree(m);
			return (MACH_E_INVAL);
		}
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
			recv_rollback_installed(to, m, explicit_descs);
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

	/*
	 * Copyout the queued kernel-VA message into the caller's user
	 * buffer.  Single SMAP bracket on the write side -- m->m_buf
	 * has been built up entirely in the kernel (post-receive
	 * descriptor translation lives in there), so this is the only
	 * user touch on the deliver path.
	 */
	{
		uint8_t *dst = (uint8_t *)buf;
		smap_user_access_begin();
		for (i = 0; i < m->m_size; i++)
			dst[i] = m->m_buf[i];
		smap_user_access_end();
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

	/*
	 * Read the caller's bits, then patch msgh_bits / msgh_local in
	 * place to splice in MAKE_SEND and the freshly-allocated reply
	 * name.  Both touch the user's req struct, so they happen under
	 * a SMAP bracket.  The COMPLEX flag is preserved so an RPC that
	 * carries body descriptors does not lose its descriptor area to
	 * a MACH_MSGH_BITS() recompute that would only encode the
	 * disposition bytes.
	 */
	{
		uint32_t bits_local;

		smap_user_access_begin();
		bits_local = req->msgh_bits;
		smap_user_access_end();

		remote_disp = MACH_MSGH_BITS_REMOTE(bits_local);

		smap_user_access_begin();
		req->msgh_bits = MACH_MSGH_BITS(remote_disp,
		    MACH_MSG_TYPE_MAKE_SEND) |
		    (bits_local & MACH_MSGH_BITS_COMPLEX);
		req->msgh_local = reply_name;
		smap_user_access_end();
	}

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
