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
 * Mach IPC message passing: the port FIFOs and pending-descriptor
 * housekeeping, mach_msg_send (with the special-port and inline-reply
 * fast paths), mach_msg_recv / recv_block / recv_timed, and mach_msg_rpc,
 * which arms the reply port's stash so a synchronous reply needs no
 * kmalloc.
 *
 * Object and name-table operations live in port_object.c and
 * port_space.c, shared through port_internal.h.  The special-port
 * dispatchers (task_self_dispatch in task.c, bootstrap_dispatch in
 * bootstrap.c) run in the sender's context from mach_msg_send.
 */

/* ---- message-queue helpers ----------------------------------------- */

static void	pd_ool_drop(struct port_pending_desc *pd);

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
	 * Reserved msgh_bits and msgh_voucher (vouchers are unimplemented)
	 * must be zero, so a later meaning for them cannot silently
	 * reinterpret an old caller's stray bits.
	 */
	if ((h->msgh_bits & ~MACH_MSGH_BITS_USED_MASK) != 0)
		return (MACH_E_INVAL);
	if (h->msgh_voucher != 0)
		return (MACH_E_INVAL);
	return (MACH_MSG_OK);
}

/*
 * Append a message.  If a thread is parked on this port or on its port
 * set, hand one out through *waiter_out for the caller to wake after our
 * locks are dropped (thread_wake takes sched_lock; lock order is port ->
 * sched).  Set waiters go first: the usual shape is one server parked on
 * a set of many ports.
 *
 * The waiter comes out held (thread_hold, taken under the list's lock
 * while it is provably alive): before the caller's wake it can be killed,
 * retire and be reaped, and the hold makes the reaper wait.  The caller
 * owes a thread_unhold after its thread_wake.
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
			set->ps_waiters_head = w->th_wait_link;
			if (set->ps_waiters_head == NULL)
				set->ps_waiters_tail = NULL;
			w->th_wait_link = NULL;
			thread_hold(w);
		}
		spin_unlock(&set->ps_lock);
	}

	if (w == NULL) {
		spin_lock(&p->p_lock);
		w = p->p_waiters_head;
		if (w != NULL) {
			p->p_waiters_head = w->th_wait_link;
			if (p->p_waiters_head == NULL)
				p->p_waiters_tail = NULL;
			w->th_wait_link = NULL;
			thread_hold(w);
		}
		spin_unlock(&p->p_lock);
	}

	*waiter_out = w;
	return (MACH_MSG_OK);
}

/*
 * Pop one thread off the port's send-waiter FIFO, if any.  Caller holds
 * p_lock, wakes the returned thread after dropping it (port -> sched),
 * and then owes a thread_unhold: the thread comes out held, as in
 * msg_enqueue.
 */
static struct thread *
port_extract_send_waiter_locked(struct port *p)
{
	struct thread	*w;

	w = p->p_send_waiters_head;
	if (w != NULL) {
		p->p_send_waiters_head = w->th_wait_link;
		if (p->p_send_waiters_head == NULL)
			p->p_send_waiters_tail = NULL;
		w->th_wait_link = NULL;
		thread_hold(w);
	}
	return (w);
}

/*
 * Take a specific thread off the port's recv-waiter list if it is still
 * on it.  Every thread that links itself here calls this on the way out,
 * whatever woke it (see the recv loop).  Caller holds p_lock.  A no-op if
 * a sender already extracted the thread.
 *
 * Removing the tail makes its predecessor the new tail, not NULL: a NULL
 * tail over a non-empty list sends the next parker down the empty-list
 * path, overwriting the head and stranding its waiter.
 */
static void
port_unbind_waiter_locked(struct port *p, struct thread *th)
{
	struct thread	**pp;
	struct thread	 *prev;

	prev = NULL;
	pp   = &p->p_waiters_head;
	while (*pp != NULL) {
		if (*pp == th) {
			*pp = th->th_wait_link;
			if (p->p_waiters_tail == th)
				p->p_waiters_tail = prev;
			th->th_wait_link = NULL;
			return;
		}
		prev = *pp;
		pp   = &(*pp)->th_wait_link;
	}
}

static void
port_set_unbind_waiter_locked(struct port_set *set, struct thread *th)
{
	struct thread	**pp;
	struct thread	 *prev;

	prev = NULL;
	pp   = &set->ps_waiters_head;
	while (*pp != NULL) {
		if (*pp == th) {
			*pp = th->th_wait_link;
			if (set->ps_waiters_tail == th)
				set->ps_waiters_tail = prev;
			th->th_wait_link = NULL;
			return;
		}
		prev = *pp;
		pp   = &(*pp)->th_wait_link;
	}
}

/*
 * The same for the list a sender parks on when the destination queue is
 * full.  Caller holds p_lock.
 */
static void
port_unbind_send_waiter_locked(struct port *p, struct thread *th)
{
	struct thread	**pp;
	struct thread	 *prev;

	prev = NULL;
	pp   = &p->p_send_waiters_head;
	while (*pp != NULL) {
		if (*pp == th) {
			*pp = th->th_wait_link;
			if (p->p_send_waiters_tail == th)
				p->p_send_waiters_tail = prev;
			th->th_wait_link = NULL;
			return;
		}
		prev = *pp;
		pp   = &(*pp)->th_wait_link;
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
 * Release what each pending descriptor in `descs` holds (a port ref or
 * an OOL payload), then free the array.  Used by the send failure paths
 * and by port_object.c to drain messages that were never received.
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
		} else if (descs[i].pd_type == MACH_MSG_OOL_DESCRIPTOR) {
			pd_ool_drop(&descs[i]);
		}
	}
	kfree(descs);
}

/*
 * Give up an in-flight OOL payload: every exit but delivery ends here (a
 * send that fails after capturing, a port torn down with mail queued).
 * The message holds one reference per frame, so a page shared with the
 * sender survives and a page copied for the message is freed.  Delivery
 * must not come here: it hands the same frames to the receiver instead.
 */
static void
pd_ool_drop(struct port_pending_desc *pd)
{

	if (pd->pd_ool_pages == NULL)
		return;
	vm_pages_release(pd->pd_ool_pages, pd->pd_ool_npages);
	kfree(pd->pd_ool_pages);
	pd->pd_ool_pages  = NULL;
	pd->pd_ool_npages = 0;
	pd->pd_ool_size   = 0;
}

/* ---- notifications --------------------------------------------------- */

/*
 * Synthesise a notification message and enqueue it on `notify_port`.
 * Called from port_deref (no-senders, dead-name) and from port_space.c
 * (send-once destroyed unused), with a ref on notify_port held so the
 * port cannot be torn down under the send.  The caller owns that ref;
 * this does not drop it.
 *
 * Best-effort: a dead notify port or a full queue drops the notification.
 * There is no dead-letter backstop.
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

	m = kmalloc(sizeof(*m) +
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
	if (waiter != NULL) {
		thread_wake(waiter);
		thread_unhold(waiter);
	}
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

	m = kmalloc(sizeof(*m) +
	    sizeof(struct mach_exception_header));
	if (m == NULL)
		return (MACH_E_NOMEM);

	/*
	 * A watcher that opted into the reply protocol gets a kernel-owned
	 * reply port as an implicit msgh_local descriptor (MAKE_SEND), so
	 * delivery installs a SEND name in its space; its reply wakes the
	 * caller's kernel-side recv on `reply_port`.
	 *
	 * One SEND ref is taken for the message in flight.  deliver_msg
	 * drops it after installing the name, and free_pending_descs drops
	 * it if the message dies undelivered.
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
		descs[0].pd_ool_pages   = NULL;
		descs[0].pd_ool_npages  = 0;
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
	if (waiter != NULL) {
		thread_wake(waiter);
		thread_unhold(waiter);
	}
	return (MACH_MSG_OK);
}

/* ---- mach_msg_send --------------------------------------------------- */

/*
 * Resolve a port descriptor on send: translate the sender's name, apply
 * the move/copy/make disposition, and record {port, right} in `pd`.
 *
 * pd_disposition is the right the receiver will be granted -- RECEIVE,
 * SEND or SEND_ONCE.  Move/copy/make only matters to the sender; the
 * receiver always gets a fresh name carrying the right.
 */
static int
send_xlate_desc(struct port_space *from, mach_port_name_t name,
    uint8_t disposition, struct port_pending_desc *pd)
{
	struct port	*p;
	uint8_t		 rights;

	pd->pd_type       = MACH_MSG_PORT_DESCRIPTOR;
	pd->pd_ool_pages  = NULL;
	pd->pd_ool_npages = 0;
	pd->pd_ool_size   = 0;

	switch (disposition) {
	case MACH_MSG_TYPE_MOVE_RECEIVE:
		p = space_lookup(from, name, MACH_PORT_RIGHT_RECEIVE,
		    &rights);
		if (p == NULL)
			return (MACH_E_RIGHT);
		/*
		 * Refuse while the port is in a set or has receivers
		 * parked on it: moving the right would silently detach the
		 * member or strand a thread on a right it no longer owns.
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
 * One step of the variable-stride descriptor walk: return the stride of
 * the descriptor at buf[off] (8 for port, 16 for OOL) and its tag in
 * *type_out, or 0 -- MACH_E_INVAL to the caller -- if the tag is unknown
 * or the descriptor would run past `cap`.
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
 * On send, validate the OOL descriptor's sender range and capture the
 * payload's frames.  The sender is the calling task, so its page tables
 * are current and vm_pages_capture_user shares what it can instead of
 * copying; there is no staging buffer, and a page that must be copied is
 * copied once.  Which pages are shared is vm/vm.h's decision.  A shared
 * page is write-protected in the sender before this returns, so the
 * receiver gets the bytes as they were at the send.
 *
 * `deallocate` is honoured for ring-3 senders: the sender's page-rounded
 * range is released with vm_map_release after the capture.  Best-effort:
 * vm_map_release refuses a range with a hole or a non-anonymous entry in
 * it, and the sender's map is then left alone.  Kernel and trusted
 * senders skip it; their addresses are kernel VA.
 *
 * On success `pd` owns the frames: delivery takes them over, every other
 * exit gives them back through pd_ool_drop.
 */
static int
send_capture_ool(struct port_space *from,
    const struct mach_msg_ool_descriptor *od,
    struct port_pending_desc *pd)
{
	struct task	*sender;
	uint64_t	*pages;
	size_t		 npages;
	size_t		 got;
	uint32_t	 size;
	uint64_t	 addr;
	bool		 from_kernel;

	(void)from;	/* sender == current task; from isn't needed */

	pd->pd_type       = MACH_MSG_OOL_DESCRIPTOR;
	pd->pd_port       = NULL;
	pd->pd_ool_pages  = NULL;
	pd->pd_ool_npages = 0;
	pd->pd_ool_size   = 0;
	pd->pd_ool_copy   = MACH_MSG_PHYSICAL_COPY;

	size = od->size;
	addr = od->address;

	if (size == 0) {
		/* Legal and empty: the receiver sees address 0, size 0. */
		return (MACH_MSG_OK);
	}
	if (size > MACH_MSG_OOL_MAX_BYTES)
		return (MACH_E_INVAL);
	if (od->copy != MACH_MSG_PHYSICAL_COPY &&
	    od->copy != MACH_MSG_VIRTUAL_COPY)
		return (MACH_E_INVAL);

	/*
	 * Coarse check that the range lies in the task's user VA window:
	 * wraparound and out-of-window are refused here, and an unmapped
	 * page inside the window faults when it is copied.
	 *
	 * Kernel senders are exempt -- kernel_task, and a thread with
	 * th_trusted_send set by mach_msg_send_trusted around one send
	 * (service dispatchers replying out of kernel .rodata).  They are
	 * trusted not to name unmapped memory; if one does, the copy
	 * faults in the kernel, which is louder than MACH_E_INVAL.
	 */
	sender = current_thread != NULL ? current_thread->th_task : NULL;
	from_kernel = (sender == NULL || sender == kernel_task ||
	    sender->t_map == NULL ||
	    (current_thread != NULL && current_thread->th_trusted_send));
	if (!from_kernel) {
		uint64_t end = addr + size;
		if (end < addr)
			return (MACH_E_INVAL);	/* wraparound */
		if (addr < sender->t_map->vm_lo ||
		    end > sender->t_map->vm_hi)
			return (MACH_E_INVAL);
	}

	npages = (size_t)((((uint64_t)size + 0xFFFull) & ~0xFFFull) >> 12);
	pages  = kmalloc(npages * sizeof(uint64_t));
	if (pages == NULL)
		return (MACH_E_NOMEM);

	/*
	 * A kernel sender's `addr` is in no task's map, so its payload is
	 * copied -- which also keeps a service's .rodata from becoming a
	 * receiver's writable page.
	 */
	if (from_kernel)
		got = vm_pages_capture_kernel((const void *)(uintptr_t)addr,
		    size, pages, npages);
	else
		got = vm_pages_capture_user(sender->t_map, sender->t_pmap,
		    addr, size, pages, npages);
	if (got != npages) {
		kfree(pages);
		return (MACH_E_NOMEM);
	}

	pd->pd_ool_pages  = pages;
	pd->pd_ool_npages = npages;
	pd->pd_ool_size   = size;

	/*
	 * Deallocate-on-send, ring-3 senders only.  The sender gives up the
	 * whole pages its payload occupies, and only those: vm_map cuts a
	 * larger entry, so handing over one page of eight keeps seven.
	 */
	if (od->deallocate != 0 && !from_kernel) {
		uint64_t aligned = ((uint64_t)size + 0xFFFull) & ~0xFFFull;
		(void)vm_map_release(sender->t_map, sender->t_pmap,
		    addr, aligned);
	}
	return (MACH_MSG_OK);
}

/*
 * Deallocate-on-send for the special-port path.  The queue path honours
 * the flag in send_capture_ool, once the frames are captured.  A special
 * port's dispatcher instead reads OOL bytes straight out of the sender's
 * address space, so the source can only be released after the dispatch.
 *
 * `msg` is the kernel copy in m->m_buf; only the vm_map_release calls
 * touch user state, taking `od->address` as data.
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
 * Undo an OOL range installed in the receiver: unmap each of the first
 * `installed_pages` pages, release its frame, and drop the vm_map entry
 * covering `total_aligned` bytes.
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
 * On recv, map the payload's frames into the receiver and write the
 * landing address into the descriptor.  vm_pages_install maps them
 * read-only under a writable copy-on-write entry, so a receiver that only
 * reads goes on sharing and one that writes takes a fault; nothing is
 * copied here.
 *
 * On success the frames belong to the receiver's map and `pd` forgets
 * them without releasing them.  On failure nothing was installed, `pd`
 * still owns them, and the caller's cleanup releases them.
 */
static int
recv_install_ool(struct port_space *to_space,
    struct port_pending_desc *pd,
    struct mach_msg_ool_descriptor *od_in_buf)
{
	struct task	*to;
	uint64_t	 landing_va = 0;
	uint32_t	 size;

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
	if (pd->pd_ool_pages == NULL)
		return (MACH_E_INVAL);

	if (!vm_pages_install(to->t_map, to->t_pmap, pd->pd_ool_pages,
	    pd->pd_ool_npages, &landing_va))
		return (MACH_E_NOMEM);

	od_in_buf->address = landing_va;

	/* Handed over.  Forget them without releasing them. */
	kfree(pd->pd_ool_pages);
	pd->pd_ool_pages  = NULL;
	pd->pd_ool_npages = 0;
	pd->pd_ool_size   = 0;
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
	 * Copy in the header, validate the copy, then copy the whole
	 * message straight into the queue buffer m->m_buf.  From there on
	 * `msg` aliases m->m_buf, and every read here, in the dispatchers
	 * and in the OOL walker, is a plain kernel-VA access.  The bytes
	 * are copied once; the special-port path pays a kmalloc/kfree for
	 * a buffer it never queues.
	 */
	smap_user_access_begin();
	hdr_copy = *umsg;
	smap_user_access_end();

	rv = msg_validate(&hdr_copy);
	if (rv != MACH_MSG_OK)
		return (rv);

	/*
	 * No bounds check on the body: kernel-spawned worker tasks send
	 * with kernel-VA `umsg` although they are not kernel_task.  The
	 * syscalls (sys_msg_send / sys_msg_rpc) bound umsg + msgh_size
	 * against the user window before calling here; kernel callers are
	 * trusted.
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

	m = kmalloc(sizeof(*m) + hdr_copy.msgh_size);
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
		/*
		 * A name whose port died and was converted to a dead-name
		 * tombstone reads as absent here; report it as dead so the
		 * sender sees MACH_E_DEAD rather than MACH_E_RIGHT.
		 */
		if (space_name_is_dead(from, msg->msgh_remote))
			return (MACH_E_DEAD);
		return (MACH_E_RIGHT);
	}

	/*
	 * Inline-reply fast path.  A bare message (no descriptors, no
	 * reply port) to a port with an armed stash is written straight
	 * into the stash buffer and p_stash_rv set to OK: no enqueue, no
	 * wake.  This is the synchronous-RPC rendezvous -- the armer is
	 * inside mach_msg_rpc on this same call stack and reads
	 * p_stash_rv when its send returns.  MOVE_* still drops the
	 * sender's right, as on the queue path.
	 *
	 * Only the arming thread qualifies.  The stash stays armed for the
	 * whole of the RPC's send, so a server woken by it on another CPU
	 * can reply first; its CR3 is its own, and the stash VA there is
	 * whatever the server keeps at that address.  Such a reply queues.
	 */
	if (!complex && !has_local) {
		size_t	want_size;

		want_size = msg->msgh_size;
		spin_lock(&dest->p_lock);
		if (dest->p_stash_buf != NULL &&
		    dest->p_stash_thread == current_thread &&
		    dest->p_stash_rv == MACH_E_NOMSG &&
		    !dest->p_dead &&
		    want_size <= dest->p_stash_size) {
			uint8_t			*dst;
			const uint8_t		*src2;
			struct mach_msg_header	*sh;

			dst  = (uint8_t *)dest->p_stash_buf;	/* user VA */
			src2 = (const uint8_t *)msg;		/* kernel VA */
			/*
			 * p_stash_buf is the RPC caller's user buffer: one
			 * SMAP bracket over the copy and the msgh_local
			 * fixup, as in deliver_msg.
			 */
			smap_user_access_begin();
			for (i = 0; i < want_size; i++)
				dst[i] = src2[i];
			/*
			 * msgh_local is a name in the sender's space, not the
			 * receiver's; zero it so the receiver cannot drop a
			 * name it never owned.
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
	 * Special-port intercept: a kernel-object destination (task self,
	 * bootstrap, a service) is never queued; its dispatcher runs here
	 * and sends any reply itself.  MOVE_* drops the sender's right
	 * first, so the dispatcher never sees a stale name.
	 */
	if (dest->p_special != PORT_SPECIAL_NONE) {
		int	special_rv;

		if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
		    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
			(void)space_drop_one_right(from,
			    msg->msgh_remote, remote_right);
		}
		switch (dest->p_special) {
		case PORT_SPECIAL_TASK_SELF: {
			struct task	*t;
			uint64_t	 tid;

			/*
			 * p_special_arg is the target's task id, not a
			 * pointer: the port can outlive the task.  A reaped
			 * task resolves to NULL and gives MACH_E_DEAD.
			 */
			tid = (uint64_t)(uintptr_t)dest->p_special_arg;
			t = task_lookup_ref(tid);
			if (t == NULL) {
				special_rv = MACH_E_DEAD;
				break;
			}
			special_rv = task_self_dispatch(t, msg, from);
			task_deref(t);
			break;
		}
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
		/* Now that the dispatcher has read them, deallocate-on-send. */
		apply_ool_deallocate(msg, from);
		kfree(m);
		return (special_rv);
	}

	/*
	 * A ref on dest of the sender's right's kind, held across the
	 * enqueue whether the right is moved or copied.
	 */
	port_ref(dest, remote_right);

	/*
	 * MOVE_* gives up the sender's right; other rights under the same
	 * name (RECEIVE alongside SEND) survive.
	 */
	if (remote_disp == MACH_MSG_TYPE_MOVE_SEND ||
	    remote_disp == MACH_MSG_TYPE_MOVE_SEND_ONCE) {
		(void)space_drop_one_right(from, msg->msgh_remote,
		    remote_right);
	}

	hdrs_off = sizeof(struct mach_msg_header);

	/*
	 * A reply right travels only with both a name and a disposition;
	 * short of either, neither reaches the receiver.  deliver_msg reads
	 * local bits as a right waiting in m_descs, and a queued message
	 * with the bits but no name had it index past the array.
	 */
	if (!has_local) {
		struct mach_msg_header	*qh;

		qh = (struct mach_msg_header *)m->m_buf;
		qh->msgh_bits  &= ~MACH_MSGH_BITS(0, 0xFFu);
		qh->msgh_local  = MACH_PORT_NULL;
	}

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
			 * Check every tag and bound before processing any
			 * descriptor.
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
	 * A reply port in msgh_local travels as an implicit port
	 * descriptor with the local disposition, in the last slot of
	 * m_descs.
	 */
	if (has_local) {
		struct port_pending_desc local_pd;
		rv = send_xlate_desc(from, msg->msgh_local, local_disp,
		    &local_pd);
		if (rv != MACH_MSG_OK)
			goto fail;
		/* Grow the array by one for it. */
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
		/* Delivery writes the receiver's name here. */
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
		 * Enqueue with backpressure: on a full queue, park on
		 * p_send_waiters until a receive frees a slot
		 * (port_extract_send_waiter_locked), then retry.  If the
		 * port dies meanwhile, port_deref's RECEIVE-drop path wakes
		 * the list and the retry sees p_dead: MACH_E_DEAD.
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
			KASSERT(self->th_wait_link == NULL &&
			    dest->p_send_waiters_tail != self,
			    "send: already on this port's send-waiter list");
			self->th_wait_link = NULL;
			if (dest->p_send_waiters_tail == NULL) {
				dest->p_send_waiters_head = self;
				dest->p_send_waiters_tail = self;
			} else {
				dest->p_send_waiters_tail->th_wait_link =
				    self;
				dest->p_send_waiters_tail = self;
			}
			thread_wait_note(self, &dest->p_send_waiters_head,
			    &dest->p_send_waiters_tail, &dest->p_lock);
			thread_block_release(THREAD_BLOCK_PORT, dest,
			    &dest->p_lock);
			/*
			 * Off the list before going round again,
			 * unconditionally, as in the recv loop below.
			 */
			spin_lock(&dest->p_lock);
			port_unbind_send_waiter_locked(dest, self);
			thread_wait_forget(self);
			spin_unlock(&dest->p_lock);
			/* Loop and retry. */
		}
		if (waiter != NULL) {
			thread_wake(waiter);
			thread_unhold(waiter);
		}
	}

	/* The ref taken after lookup, of the sender's right's kind. */
	port_deref(dest, remote_right);
	return (MACH_MSG_OK);

fail:
	/* Releases the descriptors' port refs and OOL frames. */
	free_pending_descs(descs, ndescs);
	if (m != NULL)
		kfree(m);
	if (dest != NULL)
		port_deref(dest, remote_right);
	return (rv);
}

/* ---- mach_msg_recv --------------------------------------------------- */

/*
 * Delivery failed at descriptor `up_to`: undo the ones before it and
 * release the rest.
 *
 *	PORT	-- the name now in the descriptor is the receiver's; drop
 *		   that right from the receiver.
 *	OOL	-- the address is a receiver VA; unmap the range, release
 *		   the frames, drop the vm_map entry.
 *
 * Descriptors from `up_to` on were never installed: their port refs are
 * dropped and their OOL payloads given back.
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

	/* From `up_to` on, nothing was installed. */
	for (i = up_to; i < m->m_ndescs; i++) {
		if (m->m_descs[i].pd_type == MACH_MSG_PORT_DESCRIPTOR &&
		    m->m_descs[i].pd_port != NULL) {
			port_deref(m->m_descs[i].pd_port,
			    m->m_descs[i].pd_disposition);
		} else if (m->m_descs[i].pd_type == MACH_MSG_OOL_DESCRIPTOR) {
			pd_ool_drop(&m->m_descs[i]);
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
			 * A RECEIVE descriptor carries the right itself, not
			 * a ref (see MOVE_RECEIVE in send_xlate_desc): install
			 * it without a port_ref and skip the port_deref.
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
	 * Copy out the translated message: the only user access on the
	 * delivery path.
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
	if (send_waiter != NULL) {
		thread_wake(send_waiter);
		thread_unhold(send_waiter);
	}
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
	 * The deadline, once.  Unused for FOREVER (never timed) and NONE
	 * (MACH_E_NOMSG on empty, never blocks).
	 */
	deadline = 0;
	if (timeout_ms != MACH_TIMEOUT_FOREVER &&
	    timeout_ms != MACH_TIMEOUT_NONE)
		deadline = clock_uptime_ms() + timeout_ms;

	/*
	 * The name is a port (RECEIVE right) or a port set; a set serves
	 * whichever member has a message first.
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
				if (send_waiter != NULL) {
					thread_wake(send_waiter);
					thread_unhold(send_waiter);
				}
				return (rv);
			}

			if (timeout_ms == MACH_TIMEOUT_NONE) {
				spin_unlock(&p->p_lock);
				return (MACH_E_NOMSG);
			}

			self = current_thread;
			KASSERT(self->th_wait_link == NULL &&
			    p->p_waiters_tail != self,
			    "recv: already on this port's waiter list");
			self->th_wait_link = NULL;
			self->th_timed_out = 0;
			if (p->p_waiters_tail == NULL) {
				p->p_waiters_head = self;
				p->p_waiters_tail = self;
			} else {
				p->p_waiters_tail->th_wait_link = self;
				p->p_waiters_tail = self;
			}
			thread_wait_note(self, &p->p_waiters_head,
			    &p->p_waiters_tail, &p->p_lock);

			if (timeout_ms != MACH_TIMEOUT_FOREVER) {
				self->th_wake_deadline_ms = deadline;
				sched_add_timed_waiter(self);
			}

			thread_block_release(THREAD_BLOCK_PORT, p,
			    &p->p_lock);

			/*
			 * Off both lists, whatever ended the park.  A sender
			 * that woke us has already unlinked us; a deadline has
			 * not, nor has a park that declined to park because
			 * th_wake_pending was set.  Left linked, the next trip
			 * round links this thread again through the tail,
			 * making a one-element cycle that swallows every
			 * later wake on the port.  Detaching unconditionally
			 * covers every way out of a park, for a walk of a list
			 * that is nearly always empty.
			 */
			sched_remove_timed_waiter(self);

			spin_lock(&p->p_lock);
			port_unbind_waiter_locked(p, self);
			thread_wait_forget(self);

			if (self->th_timed_out) {
				self->th_timed_out = 0;
				/*
				 * The deadline woke us; look at the queue once
				 * more in case a message landed meanwhile.
				 */
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
					if (send_waiter != NULL) {
						thread_wake(send_waiter);
						thread_unhold(send_waiter);
					}
					return (rv);
				}
				spin_unlock(&p->p_lock);
				return (MACH_E_TIMEOUT);
			}
			spin_unlock(&p->p_lock);
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
				if (send_waiter != NULL) {
					thread_wake(send_waiter);
					thread_unhold(send_waiter);
				}
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
		KASSERT(self->th_wait_link == NULL &&
		    set->ps_waiters_tail != self,
		    "recv: already on this port set's waiter list");
		self->th_wait_link = NULL;
		self->th_timed_out = 0;
		if (set->ps_waiters_tail == NULL) {
			set->ps_waiters_head = self;
			set->ps_waiters_tail = self;
		} else {
			set->ps_waiters_tail->th_wait_link = self;
			set->ps_waiters_tail = self;
		}
		thread_wait_note(self, &set->ps_waiters_head,
		    &set->ps_waiters_tail, &set->ps_lock);

		if (timeout_ms != MACH_TIMEOUT_FOREVER) {
			self->th_wake_deadline_ms = deadline;
			sched_add_timed_waiter(self);
		}

		thread_block_release(THREAD_BLOCK_PORT, set,
		    &set->ps_lock);

		/* Off both lists unconditionally -- see the port case above. */
		sched_remove_timed_waiter(self);

		spin_lock(&set->ps_lock);
		port_set_unbind_waiter_locked(set, self);
		thread_wait_forget(self);
		spin_unlock(&set->ps_lock);

		if (self->th_timed_out) {
			self->th_timed_out = 0;
			/*
			 * Unlike the port case there is no last look: a
			 * message that landed meanwhile stays queued for the
			 * next receive.
			 */
			return (MACH_E_TIMEOUT);
		}
	}
}

/*
 * mach_msg_rpc: send, then receive on a fresh reply port.
 *
 * The reply port is allocated in `space` with RECEIVE + SEND and spliced
 * into req->msgh_local as MAKE_SEND, so the server gets a SEND right
 * under its own name and replies to it.  req->msgh_bits gets the local
 * disposition; the caller's remote disposition and COMPLEX bit are kept.
 * The reply port is deallocated on every return.
 *
 * Inline reply: before the send the reply port's stash is armed with
 * reply_buf, its size and rv = MACH_E_NOMSG.  If a special-port
 * dispatcher replies synchronously, in this thread, with a bare message,
 * mach_msg_send's fast path writes it straight into reply_buf and sets
 * the rv to OK -- no port_msg at all -- and the recv is skipped.  Any
 * other reply is queued as usual: the stash is disarmed when the send
 * returns and the recv reads the FIFO.
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
	 * Arm the stash.  A complex or queued reply leaves p_stash_rv at
	 * MACH_E_NOMSG, and we fall through to the recv.
	 */
	spin_lock(&reply_port->p_lock);
	reply_port->p_stash_buf    = reply_buf;
	reply_port->p_stash_thread = current_thread;
	reply_port->p_stash_size   = reply_buf_size;
	reply_port->p_stash_rv     = MACH_E_NOMSG;
	spin_unlock(&reply_port->p_lock);

	/*
	 * Splice MAKE_SEND and the reply name into the caller's req (user
	 * memory, hence the SMAP brackets).  COMPLEX is carried over by
	 * hand: MACH_MSGH_BITS() encodes only the dispositions.
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
	 * Disarm whatever the send did; stash_rv == MACH_MSG_OK means the
	 * reply is already in reply_buf.
	 */
	spin_lock(&reply_port->p_lock);
	stash_rv = reply_port->p_stash_rv;
	reply_port->p_stash_buf    = NULL;
	reply_port->p_stash_thread = NULL;
	reply_port->p_stash_size   = 0;
	reply_port->p_stash_rv     = MACH_E_NOMSG;
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

/* ---- selftest -------------------------------------------------------- */

/*
 * Selftest: a port's waiter list must be empty once every waiter has left
 * it, however each one left.  A thread that leaves the CPU but stays on
 * the list hangs the port silently, so the test looks at the list itself.
 *
 * Every scene is arranged, not raced for: the wake-before-sleep window is
 * reproduced by setting th_wake_pending directly, so the test is
 * deterministic on one CPU or many.
 */
#define	PW_TIMEOUT_MS	20u	/* long enough to be a real park */
#define	PW_HELPER_MS	3000u	/* the helper always leaves, pass or fail */
#define	PW_WAIT_MS	4000u	/* ...and this outlasts it, so it is gone */

struct pw_helper {
	struct port		*ph_port;
	mach_port_name_t	 ph_name;
	volatile int		 ph_got;	/* the message arrived  */
	volatile int		 ph_done;	/* and the thread is out */
	struct thread		*ph_thread;
};

static struct pw_helper	pw;	/* static: the helper outlives the frame */
static struct pw_helper	pw_corpse;	/* scene 3: dies on the list    */
static struct pw_helper	pw_victim;	/* scene 4: killed mid-park     */

static void
pw_helper_entry(void *arg)
{
	struct mach_msg_header	 buf;
	struct pw_helper	*h;
	int			 rv;

	h = arg;
	rv = mach_msg_recv_timed(kernel_space, h->ph_name, &buf, sizeof(buf),
	    PW_HELPER_MS);
	if (rv == MACH_MSG_OK)
		h->ph_got = 1;
	h->ph_done = 1;
	thread_exit();
}

/*
 * Scene 3's helper: link onto the port's waiter list exactly as the recv
 * loop does, thread_wait_note included, then die still linked.  The scene
 * checks that thread_exit unlinked it.
 */
static void
pw_corpse_entry(void *arg)
{
	struct pw_helper	*h;
	struct port		*p;

	h = arg;
	p = h->ph_port;
	spin_lock(&p->p_lock);
	current_thread->th_wait_link = NULL;
	if (p->p_waiters_tail == NULL) {
		p->p_waiters_head = current_thread;
		p->p_waiters_tail = current_thread;
	} else {
		p->p_waiters_tail->th_wait_link = current_thread;
		p->p_waiters_tail = current_thread;
	}
	thread_wait_note(current_thread, &p->p_waiters_head,
	    &p->p_waiters_tail, &p->p_lock);
	spin_unlock(&p->p_lock);
	h->ph_done = 1;
	thread_exit();
}

/*
 * Scene 4's victim: park in the real recv path with no timeout.  Only a
 * kill ends it, retiring the thread inside thread_block_release, so
 * returning from the recv at all is a failure (ph_got).
 */
static void
pw_victim_entry(void *arg)
{
	struct mach_msg_header	 buf;
	struct pw_helper	*h;

	h = arg;
	(void)mach_msg_recv_block(kernel_space, h->ph_name, &buf,
	    sizeof(buf));
	h->ph_got  = 1;
	h->ph_done = 1;
	thread_exit();
}

void
port_wait_selftest(void)
{
	struct mach_msg_header	 hdr;
	struct mach_msg_header	 buf;
	struct port		*p;
	struct task		*victim_task;
	mach_port_name_t	 name;
	unsigned int		 i;
	uint8_t			 rights;
	int			 rv;

	name = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (name == MACH_PORT_NULL) {
		kprintf("port-wait: FAIL no port to test with\n");
		return;
	}
	p = space_lookup(kernel_space, name, MACH_PORT_RIGHT_RECEIVE, &rights);
	if (p == NULL) {
		kprintf("port-wait: FAIL the port just made cannot be found\n");
		return;
	}

	/*
	 * 1. A park that declines to park: with th_wake_pending set,
	 *    thread_block_release returns without blocking, and the recv
	 *    loop must still unlink before it links itself again.
	 */
	current_thread->th_wake_pending = 1;
	rv = mach_msg_recv_timed(kernel_space, name, &buf, sizeof(buf),
	    PW_TIMEOUT_MS);
	if (rv != MACH_E_TIMEOUT) {
		kprintf("port-wait: FAIL a recv on an empty port answered %s, "
		    "not a timeout\n", mach_msg_strerror(rv));
		goto out;
	}
	if (p->p_waiters_head != NULL || p->p_waiters_tail != NULL) {
		kprintf("port-wait: FAIL the port still names a waiter after "
		    "its only waiter gave up\n");
		goto out;
	}

	/*
	 * 2. A second waiter, which makes the tail matter.  The helper parks
	 *    first and stays; this thread parks behind it and times out, and
	 *    removing it must leave the tail naming the helper.
	 */
	pw.ph_name   = name;
	pw.ph_got    = 0;
	pw.ph_done   = 0;
	pw.ph_thread = thread_create(kernel_task, pw_helper_entry, &pw,
	    "port-wait");
	if (pw.ph_thread == NULL) {
		kprintf("port-wait: FAIL no thread to wait with\n");
		goto out;
	}
	thread_start(pw.ph_thread);

	for (i = 0; i < PW_TIMEOUT_MS * 10; i++) {
		if (p->p_waiters_head == pw.ph_thread)
			break;
		sched_nap_ms(1);
	}
	if (p->p_waiters_head != pw.ph_thread) {
		kprintf("port-wait: FAIL the helper never reached the port's "
		    "waiter list\n");
		goto drain;
	}

	rv = mach_msg_recv_timed(kernel_space, name, &buf, sizeof(buf),
	    PW_TIMEOUT_MS);
	if (rv != MACH_E_TIMEOUT) {
		kprintf("port-wait: FAIL the second waiter answered %s, not a "
		    "timeout\n", mach_msg_strerror(rv));
		goto drain;
	}
	if (p->p_waiters_tail != pw.ph_thread) {
		kprintf("port-wait: FAIL leaving the list from behind the "
		    "helper left the tail naming somebody else\n");
		goto drain;
	}

	/*
	 *    Park and leave once more.  With a wrong tail this trip would
	 *    overwrite the head, which shows at the final send.
	 */
	rv = mach_msg_recv_timed(kernel_space, name, &buf, sizeof(buf),
	    PW_TIMEOUT_MS);
	if (rv != MACH_E_TIMEOUT) {
		kprintf("port-wait: FAIL the third park answered %s, not a "
		    "timeout\n", mach_msg_strerror(rv));
		goto drain;
	}

	/*
	 * 3. A thread that dies on the list.  A kill retires its target
	 *    inside thread_block_release, so the recv loop's detach never
	 *    runs; thread_exit must unlink it through its thread_wait_note,
	 *    or the port names freed memory.  The corpse is linked behind
	 *    the scene-2 helper, so the unlink must also fix the tail.
	 */
	pw_corpse.ph_port   = p;
	pw_corpse.ph_done   = 0;
	pw_corpse.ph_thread = thread_create(kernel_task, pw_corpse_entry,
	    &pw_corpse, "port-corpse");
	if (pw_corpse.ph_thread == NULL) {
		kprintf("port-wait: FAIL no thread to die with\n");
		goto drain;
	}
	thread_start(pw_corpse.ph_thread);
	for (i = 0; i < PW_WAIT_MS && pw_corpse.ph_done == 0; i++)
		sched_nap_ms(1);
	for (i = 0; i < PW_WAIT_MS &&
	    p->p_waiters_tail == pw_corpse.ph_thread; i++)
		sched_nap_ms(1);
	if (p->p_waiters_head != pw.ph_thread ||
	    p->p_waiters_tail != pw.ph_thread) {
		kprintf("port-wait: FAIL a thread that died on the list is "
		    "still on it\n");
		goto drain;
	}

	/*
	 * 4. The real thing: a task killed while its thread is parked in the
	 *    recv path.  The kill's wake breaks the park, the post-wake check
	 *    retires the thread, and the recv loop's thread_wait_note lets
	 *    thread_exit unlink it.
	 */
	victim_task = task_create("port-victim");
	if (victim_task == NULL) {
		kprintf("port-wait: FAIL no task to kill\n");
		goto drain;
	}
	pw_victim.ph_name   = name;
	pw_victim.ph_got    = 0;
	pw_victim.ph_thread = thread_create(victim_task, pw_victim_entry,
	    &pw_victim, "port-victim");
	if (pw_victim.ph_thread == NULL) {
		kprintf("port-wait: FAIL no thread to kill\n");
		task_deref(victim_task);
		goto drain;
	}
	thread_start(pw_victim.ph_thread);
	for (i = 0; i < PW_WAIT_MS &&
	    p->p_waiters_tail != pw_victim.ph_thread; i++)
		sched_nap_ms(1);
	if (p->p_waiters_tail != pw_victim.ph_thread) {
		kprintf("port-wait: FAIL the victim never reached the "
		    "port's waiter list\n");
		task_deref(victim_task);
		goto drain;
	}
	task_request_terminate(victim_task->t_id);
	for (i = 0; i < PW_WAIT_MS &&
	    p->p_waiters_tail == pw_victim.ph_thread; i++)
		sched_nap_ms(1);
	if (p->p_waiters_head != pw.ph_thread ||
	    p->p_waiters_tail != pw.ph_thread || pw_victim.ph_got != 0) {
		kprintf("port-wait: FAIL a thread killed mid-park is still "
		    "on the port's waiter list\n");
		task_deref(victim_task);
		goto drain;
	}
	task_deref(victim_task);

	hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	hdr.msgh_size    = sizeof(hdr);
	hdr.msgh_remote  = name;
	hdr.msgh_local   = MACH_PORT_NULL;
	hdr.msgh_voucher = 0;
	hdr.msgh_id      = 0x9317;
	rv = mach_msg_send(kernel_space, &hdr);
	if (rv != MACH_MSG_OK) {
		kprintf("port-wait: FAIL the send answered %s\n",
		    mach_msg_strerror(rv));
		goto drain;
	}

drain:
	for (i = 0; i < PW_WAIT_MS && pw.ph_done == 0; i++)
		sched_nap_ms(1);
	if (pw.ph_done == 0) {
		kprintf("port-wait: FAIL the helper is still parked\n");
		goto out;
	}
	if (pw.ph_got == 0) {
		kprintf("port-wait: FAIL the message never reached the thread "
		    "that was waiting for it\n");
		goto out;
	}
	if (p->p_waiters_head != NULL || p->p_waiters_tail != NULL) {
		kprintf("port-wait: FAIL the port still names a waiter with "
		    "nobody left waiting\n");
		goto out;
	}
	kprintf("port-wait: PASS -- a park that never slept left the list "
	    "empty, a waiter was still reachable behind two timeouts, and "
	    "two threads that died on the list took themselves off\n");
out:
	(void)port_deallocate(kernel_space, name);
}
