/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * lsmp -- list mach ports.
 *
 * Userspace tool that mirrors Darwin's `lsmp(1)` debugger surface:
 * dumps the calling task's port_space as a column-formatted table
 * with one row per populated name.  The concept is meaningless on
 * Linux (no Mach ports there); only an XNU-flavored kernel can
 * answer the question.
 *
 * The kernel surface is one syscall, SYS_TASK_GET_PORT_SNAPSHOT,
 * which copies wire-format mach_port_snapshot_entry records back to
 * the caller -- name, kind (port or port_set), rights mask, refs +
 * queue counters per port, member count per set, and a
 * PORT_SPECIAL_* tag that distinguishes the kernel's well-known
 * ports (task_self, bootstrap, ...) from ordinary user-allocated
 * ones.  v1 only supports introspecting self (task_id == 0).
 *
 * Before snapshotting, this tool stands up a deliberately varied
 * port state -- one RECV+SEND port, one SEND-only name (obtained by
 * dropping the RECV right via mod_refs), a port_set with one member,
 * and a per-type exception port slot -- so the rendered table
 * exercises every column.  In a real debugger workflow the inspected
 * task would arrive at its own port_space organically; here we
 * synthesize the variety so a single run of `lsmp` from the shell
 * paints all the row shapes.
 */

#include "style9.h"

/*
 * Decode a PORT_SPECIAL_* tag into a short human label.  Used for the
 * "description" column on the right side of every row.
 */
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

/*
 * Map well-known slot numbers to their conventional roles.  Returns
 * NULL when the slot has no fixed meaning so the description column
 * falls through to special_name().
 */
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
 * Build a 6-char mnemonic for a rights mask -- one letter per right
 * kind, "-" when absent.  Layout chosen to match port_space_print's
 * style (R / S / O for receive / send / send-once), padded out to a
 * fixed width so columns line up.
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
	 * Description: prefer the well-known slot label
	 * (task-self / bootstrap / parent), fall back to the
	 * port-special tag, then the dead marker, then nothing.
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
 * Stand up a varied port_space so the rendered table covers every
 * row shape.  Failures of the individual setup steps are non-fatal:
 * the snapshot just shows whatever state was reached.
 */
static void
seed_demo_state(void)
{
	mach_port_name_t	recv_port;
	mach_port_name_t	send_port;
	mach_port_name_t	set_name;
	mach_port_name_t	member;

	/*
	 * 1. RECV+SEND port -- the canonical "I own this endpoint" name.
	 */
	recv_port = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (recv_port == MACH_PORT_NULL)
		return;

	/*
	 * 2. SEND-only name on a port whose RECV we then drop.
	 *
	 * Mach semantics: a port lives only while its receiver is
	 * alive.  Dropping the RECV right here kills the underlying
	 * port -- the name in OUR space still carries SEND and is
	 * perfectly visible to the snapshot, but the port object
	 * itself transitions to DEAD.  port_space_snapshot reports
	 * this via PORT_SNAPSHOT_FLAG_DEAD, and lsmp's print_row
	 * decorates the description with " DEAD".  This is the
	 * principled way the introspection surface signals a real
	 * stale-handle situation (e.g. service crashed, holders of
	 * SEND rights still have names that no longer refer to
	 * anything reachable).  The live SEND-only shape -- where
	 * RECV is held by SOME OTHER task -- is already covered by
	 * the well-known MACH_PORT_BOOTSTRAP and MACH_PORT_PARENT
	 * entries the kernel + parent set up at task creation.
	 */
	send_port = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (send_port != MACH_PORT_NULL)
		(void)mach_port_mod_refs(send_port, MACH_PORT_RIGHT_RECEIVE);

	/*
	 * 3. Port set with one member -- exercises the SET kind row.
	 * Member port is RECV only since insert requires it.
	 */
	set_name = mach_port_set_allocate();
	member   = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE);
	if (set_name != MACH_PORT_NULL && member != MACH_PORT_NULL)
		(void)mach_port_set_insert(set_name, member);

	/*
	 * 4. Per-type exception port slot, using our RECV+SEND name
	 * from step 1 -- bumps that port's send_count so the column
	 * shows non-trivial bookkeeping.
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
