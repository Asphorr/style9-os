/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * lsmp -- list mach ports.
 *
 * Counterpart to Darwin's lsmp(1): the calling task's port_space as a
 * table, one row per populated name.
 *
 * One syscall, SYS_TASK_GET_PORT_SNAPSHOT, returns
 * mach_port_snapshot_entry records: name, kind (port or port set),
 * rights, refs and queue counters per port, member count per set, and a
 * PORT_SPECIAL_* tag marking the kernel's well-known ports.  Only self
 * (task_id == 0) is supported.
 *
 * Before the snapshot the tool builds a varied port state -- a
 * receive+send port, a dead send-only name, a one-member port set and an
 * exception port -- so one run shows every row shape.
 */

#include "style9.h"

/* PORT_SPECIAL_* tag as a label for the description column. */
static const char *
special_name(uint8_t special)
{

	switch (special) {
	case PORT_SPECIAL_NONE:
		return ("");
	case PORT_SPECIAL_TASK_SELF:
		return ("task-self");
	case PORT_SPECIAL_BOOTSTRAP:
		return ("bootstrap");
	case PORT_SPECIAL_SERVICE:
		return ("service");
	default:
		return ("?");
	}
}

/* Role of a well-known slot, or NULL if the slot has no fixed meaning. */
static const char *
slot_name(mach_port_name_t name)
{

	switch (name) {
	case MACH_PORT_TASK_SELF:
		return ("task-self");
	case MACH_PORT_BOOTSTRAP:
		return ("bootstrap");
	case MACH_PORT_PARENT:
		return ("parent");
	default:
		return (NULL);
	}
}

/*
 * 6-char rights mnemonic, one letter per right or "-", in
 * port_space_print's style (R / S / O / P), padded to a fixed width.
 */
static void
fmt_rights(char out[7], uint8_t r)
{

	out[0] = (r & MACH_PORT_RIGHT_RECEIVE)   ? 'R' : '-';
	out[1] = (r & MACH_PORT_RIGHT_SEND)      ? 'S' : '-';
	out[2] = (r & MACH_PORT_RIGHT_SEND_ONCE) ? 'O' : '-';
	out[3] = (r & MACH_PORT_RIGHT_PORT_SET)  ? 'P' : '-';
	out[4] = '-';
	out[5] = '-';
	out[6] = '\0';
}

static void
print_header(void)
{

	printf("  %-10s %-10s %-6s %-7s %5s %5s %6s %6s %6s  %s\n",
	    "name", "obj_id", "kind", "rights",
	    "refs", "send", "sonce", "qlimit", "qlen", "description");
	printf("  ---------- ---------- ------ "
	    "------- ----- ----- ------ ------ ------  -----------\n");
}

static void
print_row(const struct mach_port_snapshot_entry *e)
{
	char		rights[7];
	const char	*desc;
	const char	*sn;

	fmt_rights(rights, e->mpse_rights);

	/*
	 * Description: the well-known slot label, else the port-special
	 * tag, else "port-set" for a set.  " DEAD" is appended apart.
	 */
	sn = slot_name(e->mpse_name);
	if (sn != NULL)
		desc = sn;
	else if (e->mpse_special != PORT_SPECIAL_NONE)
		desc = special_name(e->mpse_special);
	else if (e->mpse_kind == PORT_SNAPSHOT_KIND_SET)
		desc = "port-set";
	else
		desc = "";

	if (e->mpse_kind == PORT_SNAPSHOT_KIND_SET) {
		printf("  0x%08x 0x%08llx %-6s %-7s %5u %5s %6s %6s %6s  "
		    "%s (members=%u)%s\n",
		    e->mpse_name,
		    (unsigned long long)e->mpse_object_id,
		    "set", rights,
		    e->mpse_refs, "-", "-", "-", "-",
		    desc, e->mpse_member_count,
		    (e->mpse_flags & PORT_SNAPSHOT_FLAG_DEAD) ? " DEAD" : "");
		return;
	}

	printf("  0x%08x 0x%08llx %-6s %-7s %5u %5u %6u %6u %6u  %s%s\n",
	    e->mpse_name,
	    (unsigned long long)e->mpse_object_id,
	    "port", rights,
	    e->mpse_refs, e->mpse_send_count, e->mpse_send_once_count,
	    e->mpse_qmax, e->mpse_qlen,
	    desc,
	    (e->mpse_flags & PORT_SNAPSHOT_FLAG_DEAD) ? " DEAD" : "");
}

/*
 * Build a varied port_space.  A failed step is harmless: the snapshot
 * shows whatever state was reached.
 */
static void
seed_demo_state(void)
{
	mach_port_name_t	recv_port;
	mach_port_name_t	send_port;
	mach_port_name_t	set_name;
	mach_port_name_t	member;

	/* 1. Receive+send port. */
	recv_port = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (recv_port == MACH_PORT_NULL)
		return;

	/*
	 * 2. Send-only name on a dead port.  A port lives only while its
	 * receive right does, so dropping it kills the port; our name keeps
	 * the send right and the snapshot flags it PORT_SNAPSHOT_FLAG_DEAD,
	 * as a crashed service's clients would see.  The live send-only
	 * shape is already there in MACH_PORT_BOOTSTRAP and
	 * MACH_PORT_PARENT.
	 */
	send_port = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (send_port != MACH_PORT_NULL)
		(void)mach_port_mod_refs(send_port, MACH_PORT_RIGHT_RECEIVE);

	/* 3. Port set with one member (insert needs the receive right). */
	set_name = mach_port_set_allocate();
	member   = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE);
	if (set_name != MACH_PORT_NULL && member != MACH_PORT_NULL)
		(void)mach_port_set_insert(set_name, member);

	/*
	 * 4. Exception port slot on the port from step 1, which raises its
	 * send count.
	 */
	(void)task_set_exception_ports(EXC_MASK_BAD_INSTRUCTION, recv_port);
}

int
main(void)
{
	struct mach_port_snapshot_entry	entries[MACH_PORT_SNAPSHOT_MAX];
	long				n;
	long				i;

	seed_demo_state();

	n = task_get_port_snapshot(0, entries, MACH_PORT_SNAPSHOT_MAX);
	if (n < 0) {
		printf("lsmp: SYS_TASK_GET_PORT_SNAPSHOT failed (rv=%ld)\n", n);
		return (1);
	}

	printf("lsmp: %ld populated name%s in calling task's port_space\n",
	    n, n == 1 ? "" : "s");
	print_header();
	for (i = 0; i < n; i++)
		print_row(&entries[i]);
	return (0);
}
