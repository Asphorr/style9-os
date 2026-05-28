/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "port_internal.h"
#include "spinlock.h"

/*
 * Per-task name table operations.
 *
 * A port_space is the integer-name -> right mapping a task uses to
 * refer to ports.  Names are local to a space: task A's name 5 and
 * task B's name 5 are unrelated.  This file owns:
 *	- space lifecycle (new / destroy / grow),
 *	- name allocation / lookup / drop,
 *	- the public space-level operations the rest of the kernel calls
 *	  (port_allocate, port_set_*, port_space_inject_send, ...).
 *
 * Internal helpers used by mach_msg_send / recv live here as exported
 * symbols (via port_internal.h); see space_install, space_lookup, etc.
 */

/* ---- space lifecycle ------------------------------------------------ */

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

int
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

struct port *
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

struct port_set *
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
int
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
int
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
int
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

/* ---- public space-level operations ---------------------------------- */

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

mach_port_name_t
port_set_extract(struct port_space *ps, mach_port_name_t port_name)
{
	struct port_set		*set;
	struct port		*p;
	mach_port_name_t	 result;
	uint8_t			 dummy;
	size_t			 i;

	if (ps == NULL)
		return (MACH_PORT_NULL);

	/*
	 * Need RECEIVE to reach the port the same way port_set_insert /
	 * port_set_remove do; SEND-only holders never see set membership
	 * because the set bridges receive queues, not send rights.
	 */
	p = space_lookup(ps, port_name, MACH_PORT_RIGHT_RECEIVE, &dummy);
	if (p == NULL)
		return (MACH_PORT_NULL);

	spin_lock(&p->p_lock);
	set = p->p_set;
	spin_unlock(&p->p_lock);
	if (set == NULL)
		return (MACH_PORT_NULL);

	/*
	 * Map the port_set pointer back to its name in this space.  The
	 * set must have been installed somewhere in ps -- port_set_insert
	 * looks up both port and set in the same space -- so a linear
	 * scan finds it.  Stop at the first hit; sets occupy at most one
	 * name per space.
	 */
	result = MACH_PORT_NULL;
	spin_lock(&ps->ps_lock);
	for (i = 1; i < ps->ps_capacity; i++) {
		if (ps->ps_table[i].pe_set == set) {
			result = (mach_port_name_t)i;
			break;
		}
	}
	spin_unlock(&ps->ps_lock);
	return (result);
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

int
port_request_notification(struct port_space *space, mach_port_name_t name,
    uint32_t notify_type, mach_port_name_t notify_port_name,
    uint32_t notify_msgid, mach_port_name_t *prev_out)
{
	struct port	*source;
	struct port	*notify_port;
	struct port	*prev_target = NULL;
	uint8_t		 required;
	uint8_t		 dummy;

	if (space == NULL || prev_out == NULL)
		return (MACH_E_INVAL);

	/*
	 * Each notification type names a different precondition on the
	 * source:
	 *	NO_SENDERS -- caller is the RECEIVER, watching for "all my
	 *		     senders went away" while still holding RECV.
	 *	DEAD_NAME  -- caller is a SEND HOLDER, watching for "the
	 *		     port behind my SEND name died" so the name
	 *		     in their own space becomes a dead name.
	 * Other types (SEND_ONCE notification, port-deleted, ...)
	 * reserved for future revisions.
	 */
	switch (notify_type) {
	case MACH_NOTIFY_NO_SENDERS:
		required = MACH_PORT_RIGHT_RECEIVE;
		break;
	case MACH_NOTIFY_DEAD_NAME:
		required = MACH_PORT_RIGHT_SEND;
		break;
	default:
		return (MACH_E_INVAL);
	}

	/*
	 * Lookup itself does not take a ref; we rely on the caller's
	 * name table entry keeping the port alive for the duration of
	 * this call.
	 */
	source = space_lookup(space, name, required, &dummy);
	if (source == NULL)
		return (MACH_E_RIGHT);

	notify_port = space_lookup(space, notify_port_name,
	    MACH_PORT_RIGHT_SEND, &dummy);
	if (notify_port == NULL)
		return (MACH_E_RIGHT);

	/*
	 * Disallow source == notify target.  For NO_SENDERS the kernel
	 * SEND ref on the notify port would mask the user's own "all
	 * senders gone" event; for DEAD_NAME firing the notification
	 * onto the same dying port is a guaranteed dead letter.
	 */
	if (source == notify_port)
		return (MACH_E_INVAL);

	/*
	 * Take a SEND ref for the slot.  Released by port_deref's
	 * firing branch (one-shot) or the RECV-drop cleanup branch.
	 * Done before swapping the field so an interleaving fire reads
	 * a live ref.
	 */
	port_ref(notify_port, MACH_PORT_RIGHT_SEND);

	spin_lock(&source->p_lock);
	if (notify_type == MACH_NOTIFY_NO_SENDERS) {
		prev_target                    = source->p_notify_no_senders;
		source->p_notify_no_senders    = notify_port;
		source->p_notify_no_senders_id = notify_msgid;
	} else {
		prev_target                    = source->p_notify_dead_name;
		source->p_notify_dead_name     = notify_port;
		source->p_notify_dead_name_id  = notify_msgid;
	}
	spin_unlock(&source->p_lock);

	/*
	 * The kernel held a SEND ref for the previous registration; drop
	 * it now that the slot points elsewhere.
	 */
	if (prev_target != NULL)
		port_deref(prev_target, MACH_PORT_RIGHT_SEND);

	/*
	 * v1 makes no attempt to resolve prev_target back to a name in
	 * the caller's space (would require walking ps_table or
	 * stashing the name alongside the pointer).  Tracking the
	 * previous registration is the application's job.
	 */
	*prev_out = MACH_PORT_NULL;
	return (MACH_MSG_OK);
}

/* ---- introspection -------------------------------------------------- */

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

size_t
port_space_snapshot(struct port_space *ps,
    struct mach_port_snapshot_entry *out, size_t max_entries)
{
	struct port			*p;
	struct port_set			*set;
	struct mach_port_snapshot_entry	*e;
	size_t				 i, n;
	uint8_t				 r;

	if (ps == NULL || out == NULL || max_entries == 0)
		return (0);

	n = 0;
	spin_lock(&ps->ps_lock);
	for (i = 1; i < ps->ps_capacity && n < max_entries; i++) {
		p   = ps->ps_table[i].pe_port;
		set = ps->ps_table[i].pe_set;
		r   = ps->ps_table[i].pe_rights;

		if (p == NULL && set == NULL)
			continue;

		e = &out[n];
		e->mpse_name             = (mach_port_name_t)i;
		e->mpse_rights           = r;
		e->mpse_special          = PORT_SPECIAL_NONE;
		e->mpse_flags            = 0;
		e->mpse_object_id        = 0;
		e->mpse_qlen             = 0;
		e->mpse_qmax             = 0;
		e->mpse_refs             = 0;
		e->mpse_send_count       = 0;
		e->mpse_send_once_count  = 0;
		e->mpse_member_count     = 0;

		if (set != NULL) {
			e->mpse_kind = (uint8_t)PORT_SNAPSHOT_KIND_SET;
			spin_lock(&set->ps_lock);
			e->mpse_object_id    = set->ps_id;
			e->mpse_refs         = set->ps_refs;
			e->mpse_member_count = (uint32_t)set->ps_member_count;
			if (set->ps_dead)
				e->mpse_flags |= PORT_SNAPSHOT_FLAG_DEAD;
			spin_unlock(&set->ps_lock);
		} else {
			e->mpse_kind = (uint8_t)PORT_SNAPSHOT_KIND_PORT;
			spin_lock(&p->p_lock);
			e->mpse_object_id       = p->p_id;
			e->mpse_special         = p->p_special;
			e->mpse_qlen            = (uint32_t)p->p_qlen;
			e->mpse_qmax            = (uint32_t)p->p_qmax;
			e->mpse_refs            = p->p_refs;
			e->mpse_send_count      = p->p_send_count;
			e->mpse_send_once_count = p->p_send_once_count;
			if (p->p_dead)
				e->mpse_flags |= PORT_SNAPSHOT_FLAG_DEAD;
			spin_unlock(&p->p_lock);
		}

		n++;
	}
	spin_unlock(&ps->ps_lock);
	return (n);
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
