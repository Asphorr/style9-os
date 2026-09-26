/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Ring-3 demo for style9-os, linked against libstyle9.  main() runs the
 * steps in order and stops at the first failure, returning its code.
 *
 *	1. printf banner via SYS_PRINT
 *	2. mach_port_allocate + self-send + recv round-trip
 *	3. mach_msg_recv_timed on an empty port (expect E_TIMEOUT)
 *	4. task_self GET_INFO RPC (sync dispatcher in the kernel)
 *	5. bootstrap_lookup("kernel_task") + GET_INFO chained call
 *	6. OOL round-trip via svc/echool (FNV-1a checksum)
 *	7. vm_allocate + write + read back + vm_deallocate
 *	8. OOL round-trip with deallocate=1 (kernel releases sender range)
 *	9. NO_SENDERS notification fires when last SEND drops
 *     10. port set: insert + extract introspection + recv across two members
 *     11. DEAD_NAME notification fires when source RECV drops
 *     12. spawn_with_port: child detects MACH_PORT_PARENT and pings back
 *     12b. OOL payload from a ring-3 child (`oolchild'), intact as of the
 *         send and copy-on-write after it
 *     13. exception port: spawned `excchild' faults, kernel posts
 *         MACH_EXC_FAULT to the parent's port carrying trapframe state
 *     13b. per-type ports: `excchild_ud' installs only BAD_INSTRUCTION,
 *         UD2's; parent verifies trapno=6 routed through that slot
 *     13c. thread-level ports: `excchild_thr' installs both task- and
 *         thread-level slots for BAD_INSTRUCTION; UD2 fires through
 *         the thread-level slot exactly once (task-level short-circuited)
 *     13d. reply protocol: `excchild_resume' opts into EXC_FLAG_RESUMABLE,
 *         UD2's; watcher sends EXC_VERDICT_RESUME with rip_advance=2;
 *         child resumes past UD2 and sends a survive tag back
 *     14. bootstrap publish: register a port under a name, look it up,
 *         send-recv round-trip via the registered name, deregister
 *     15. msg strictness: nonzero voucher and reserved msgh_bits are
 *         rejected with MACH_E_INVAL; clean header still round-trips
 *     16. spawned tools and kill paths: lsmp, vmmap, launchctl, selfkill,
 *         compute-loop kill, parent-managed kill, top, stale taskport
 *     17. argv across spawn (argecho), a Mach-O container (machotest),
 *         and Darwin-ABI programs
 *
 * A hello.elf spawned via SYS_SPAWN_WITH_PORT (a send right at
 * MACH_PORT_PARENT) only pings its parent and exits.
 */

#include "style9.h"

#define	DEMO_TAG	0xCAFEBABEu

static int
demo_round_trip(void)
{
	struct mach_msg_header	tx;
	struct mach_msg_header	rx;
	mach_port_name_t	name;
	int			rv;

	name = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (name == MACH_PORT_NULL) {
		printf("  port_allocate failed\n");
		return (1);
	}
	printf("  allocated port = 0x%x\n", (uint32_t)name);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = name;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = DEMO_TAG;

	rv = mach_msg_send(&tx);
	if (rv != MACH_MSG_OK) {
		printf("  msg_send failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (2);
	}
	printf("  self-send queued\n");

	rv = mach_msg_recv(name, &rx, sizeof(rx));
	if (rv != MACH_MSG_OK) {
		printf("  msg_recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (3);
	}
	if (rx.msgh_id != DEMO_TAG) {
		printf("  TAG MISMATCH: got 0x%x expected 0x%x\n",
		    rx.msgh_id, DEMO_TAG);
		(void)mach_port_deallocate(name);
		return (4);
	}
	printf("  mach_msg round-trip via SYSCALL: OK\n");

	/* Empty-port timeout probe. */
	rv = mach_msg_recv_timed(name, &rx, sizeof(rx), 50);
	if (rv == MACH_MSG_OK) {
		printf("  recv_timed unexpectedly returned a message\n");
		(void)mach_port_deallocate(name);
		return (5);
	}
	if (rv != MACH_E_TIMEOUT)
		printf("  recv_timed odd rv = %d\n", rv);
	else
		printf("  recv_timed returned E_TIMEOUT after 50 ms: OK\n");

	(void)mach_port_deallocate(name);
	return (0);
}

static int
demo_task_self(void)
{
	struct mach_msg_header	tx;
	struct {
		struct mach_msg_header	hdr;
		struct task_info_reply	body;
	} reply;
	int	rv;

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = MACH_PORT_TASK_SELF;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = TASK_OP_GET_INFO;

	rv = mach_msg_rpc(&tx, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  task_self GET_INFO rpc failed (rv=%d)\n", rv);
		return (6);
	}
	printf("  task_self GET_INFO ok: name='%s' tir_task_id=%llu\n",
	    reply.body.tir_name,
	    (unsigned long long)reply.body.tir_task_id);
	return (0);
}

/*
 * OOL round-trip: send a patterned buffer as the one OOL descriptor of a
 * complex message to svc/echool and expect our FNV-1a back in msgh_id.
 * Descriptor count, type tag, size and address, and the kernel's
 * special-port intercept must all be right.
 */
static uint32_t
demo_ool_fnv1a(const uint8_t *buf, uint32_t size)
{
	uint32_t	h, i;

	h = 0x811C9DC5u;
	for (i = 0; i < size; i++) {
		h ^= (uint32_t)buf[i];
		h *= 0x01000193u;
	}
	return (h);
}

static int
demo_ool_roundtrip(void)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	} req;
	struct mach_msg_header	reply;
	uint8_t			buf[512];
	mach_port_name_t	svc;
	uint32_t		i, expected;
	int			rv;

	for (i = 0; i < sizeof(buf); i++)
		buf[i] = (uint8_t)((i * 37u + 11u) & 0xFFu);
	expected = demo_ool_fnv1a(buf, sizeof(buf));

	svc = bootstrap_lookup(SVC_ECHOOL_NAME);
	if (svc == MACH_PORT_NULL) {
		printf("  bootstrap_lookup('echool') failed\n");
		return (9);
	}

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0)
	    | MACH_MSGH_BITS_COMPLEX;
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = ECHOOL_OP_CHECKSUM;

	req.body.msgh_descriptor_count = 1;

	req.ool.type       = MACH_MSG_OOL_DESCRIPTOR;
	req.ool.copy       = MACH_MSG_PHYSICAL_COPY;
	req.ool.deallocate = 0;
	req.ool.pad        = 0;
	req.ool.size       = (uint32_t)sizeof(buf);
	req.ool.address    = (uint64_t)(uintptr_t)buf;

	rv = mach_msg_rpc(&req.hdr, &reply, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("  echool rpc failed (rv=%d)\n", rv);
		return (10);
	}
	if (reply.msgh_id != expected) {
		printf("  OOL checksum MISMATCH: kernel=0x%x expected=0x%x\n",
		    (unsigned)reply.msgh_id, (unsigned)expected);
		return (11);
	}
	printf("  OOL round-trip %u bytes via echool: fnv1a=0x%x OK\n",
	    (unsigned)sizeof(buf), (unsigned)expected);
	return (0);
}

static int
demo_bootstrap_chain(void)
{
	struct mach_msg_header	tx;
	struct {
		struct mach_msg_header	hdr;
		struct task_info_reply	body;
	} info_reply;
	mach_port_name_t	svc;
	int			rv;

	svc = bootstrap_lookup("kernel_task");
	if (svc == MACH_PORT_NULL) {
		printf("  bootstrap_lookup('kernel_task') failed\n");
		return (7);
	}
	printf("  bootstrap_lookup('kernel_task') -> name=0x%x\n",
	    (uint32_t)svc);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = svc;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = TASK_OP_GET_INFO;

	rv = mach_msg_rpc(&tx, &info_reply.hdr, sizeof(info_reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("  GET_INFO via bootstrap name failed (rv=%d)\n", rv);
		return (8);
	}
	printf("  GET_INFO via bootstrap name: ok, tir_task_id=%llu\n",
	    (unsigned long long)info_reply.body.tir_task_id);
	return (0);
}

/*
 * vm_allocate: a fresh range reads zero, holds a written pattern, and
 * is released by vm_deallocate.
 */
static int
demo_vm_allocate(void)
{
	uint8_t		*buf;
	uint32_t	 i;
	int		 rv;

	buf = (uint8_t *)vm_allocate(8192, VM_PROT_READ | VM_PROT_WRITE);
	if (buf == NULL) {
		printf("  vm_allocate failed\n");
		return (12);
	}
	printf("  vm_allocate(8192) -> %p\n", buf);

	for (i = 0; i < 8192; i++) {
		if (buf[i] != 0) {
			printf("  vm_allocate: page not zero at i=%u (0x%x)\n",
			    (unsigned)i, (unsigned)buf[i]);
			(void)vm_deallocate(buf, 8192);
			return (13);
		}
	}

	for (i = 0; i < 8192; i++)
		buf[i] = (uint8_t)(i & 0xFFu);
	for (i = 0; i < 8192; i++) {
		if (buf[i] != (uint8_t)(i & 0xFFu)) {
			printf("  vm_allocate: read-back mismatch at i=%u\n",
			    (unsigned)i);
			(void)vm_deallocate(buf, 8192);
			return (14);
		}
	}

	rv = vm_deallocate(buf, 8192);
	if (rv != 0) {
		printf("  vm_deallocate failed (rv=%d)\n", rv);
		return (15);
	}
	printf("  vm_deallocate: OK (zero-filled, writable, released)\n");
	return (0);
}

/*
 * OOL with deallocate=1: send a vm_allocate'd buffer to svc/echool and
 * check the checksum.  The send released the range, so a vm_deallocate
 * of it afterwards must fail.
 */
static int
demo_ool_deallocate(void)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	}			req;
	struct mach_msg_header	reply;
	uint8_t			*buf;
	mach_port_name_t	svc;
	uint32_t		i, expected;
	int			rv;

	buf = (uint8_t *)vm_allocate(4096, VM_PROT_READ | VM_PROT_WRITE);
	if (buf == NULL) {
		printf("  vm_allocate(4096) failed\n");
		return (16);
	}
	for (i = 0; i < 4096; i++)
		buf[i] = (uint8_t)((i * 31u + 7u) & 0xFFu);
	expected = demo_ool_fnv1a(buf, 4096);

	svc = bootstrap_lookup(SVC_ECHOOL_NAME);
	if (svc == MACH_PORT_NULL) {
		printf("  bootstrap_lookup('echool') failed\n");
		(void)vm_deallocate(buf, 4096);
		return (17);
	}

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0)
	    | MACH_MSGH_BITS_COMPLEX;
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = ECHOOL_OP_CHECKSUM;
	req.body.msgh_descriptor_count = 1;
	req.ool.type       = MACH_MSG_OOL_DESCRIPTOR;
	req.ool.copy       = MACH_MSG_PHYSICAL_COPY;
	req.ool.deallocate = 1;
	req.ool.pad        = 0;
	req.ool.size       = 4096;
	req.ool.address    = (uint64_t)(uintptr_t)buf;

	rv = mach_msg_rpc(&req.hdr, &reply, sizeof(reply), 1000);
	(void)mach_port_deallocate(svc);
	if (rv != MACH_MSG_OK) {
		printf("  echool rpc (deallocate=1) failed (rv=%d)\n", rv);
		return (18);
	}
	if (reply.msgh_id != expected) {
		printf("  OOL+dealloc checksum MISMATCH: 0x%x vs 0x%x\n",
		    (unsigned)reply.msgh_id, (unsigned)expected);
		return (19);
	}

	/* Gone: vm_deallocate of an unknown range returns SYS_E_INVAL. */
	rv = vm_deallocate(buf, 4096);
	if (rv == 0) {
		printf("  vm_deallocate succeeded post-send (sent buffer "
		    "still mapped!)\n");
		return (20);
	}
	printf("  OOL+deallocate=1 4 KiB: checksum OK, post-send "
	    "vm_deallocate refused (rv=%d): OK\n", rv);
	return (0);
}

/*
 * NO_SENDERS: request the notification on a source port, then drop its
 * only send right with mach_port_mod_refs.  The kernel posts
 * MACH_NOTIFY_NO_SENDERS to the notify port, carrying our nh_msgid back
 * verbatim so one notify port can watch many sources.
 */
#define	NO_SENDERS_TAG	0xDEADBEEFu

static int
demo_no_senders(void)
{
	struct mach_notify_header	nh;
	mach_port_name_t		source, notify;
	int				rv;

	source = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (source == MACH_PORT_NULL) {
		printf("  port_allocate(source) failed\n");
		return (21);
	}
	notify = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (notify == MACH_PORT_NULL) {
		printf("  port_allocate(notify) failed\n");
		(void)mach_port_deallocate(source);
		return (22);
	}

	rv = mach_port_request_notification(source, MACH_NOTIFY_NO_SENDERS,
	    notify, NO_SENDERS_TAG);
	if (rv != MACH_MSG_OK) {
		printf("  request_notification failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (23);
	}

	/*
	 * Drop only the send right: the receive right keeps the port alive
	 * with p_send_count at 0.
	 */
	rv = mach_port_mod_refs(source, MACH_PORT_RIGHT_SEND);
	if (rv != MACH_MSG_OK) {
		printf("  mod_refs(SEND) failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (24);
	}

	rv = mach_msg_recv_timed(notify, &nh.hdr, sizeof(nh), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  notify recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (25);
	}
	if (nh.hdr.msgh_id != (uint32_t)MACH_NOTIFY_NO_SENDERS) {
		printf("  notify msgh_id=%u expected %u\n",
		    (unsigned)nh.hdr.msgh_id,
		    (unsigned)MACH_NOTIFY_NO_SENDERS);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (26);
	}
	if (nh.nh_msgid != NO_SENDERS_TAG) {
		printf("  notify nh_msgid=0x%x expected 0x%x\n",
		    (unsigned)nh.nh_msgid, (unsigned)NO_SENDERS_TAG);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (27);
	}
	printf("  NO_SENDERS notification: id=%u tag=0x%x OK\n",
	    (unsigned)nh.hdr.msgh_id, (unsigned)nh.nh_msgid);

	(void)mach_port_deallocate(source);
	(void)mach_port_deallocate(notify);
	return (0);
}

/*
 * Port set: two members inserted, one tagged message sent to each, two
 * receives on the set name.  Either order is accepted: FIFO is per
 * member, so this tests the wiring, not ordering.
 */
#define	PSET_TAG_A	0xA1A1A1A1u
#define	PSET_TAG_B	0xB2B2B2B2u

static int
demo_port_set(void)
{
	struct mach_msg_header	tx;
	struct mach_msg_header	rx;
	mach_port_name_t	set_name, port_a, port_b;
	int			rv;
	uint32_t		got_a, got_b;

	set_name = mach_port_set_allocate();
	if (set_name == MACH_PORT_NULL) {
		printf("  port_set_allocate failed\n");
		return (28);
	}

	port_a = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (port_a == MACH_PORT_NULL) {
		printf("  port_allocate(A) failed\n");
		(void)mach_port_deallocate(set_name);
		return (29);
	}
	port_b = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (port_b == MACH_PORT_NULL) {
		printf("  port_allocate(B) failed\n");
		(void)mach_port_deallocate(set_name);
		(void)mach_port_deallocate(port_a);
		return (30);
	}

	rv = mach_port_set_insert(set_name, port_a);
	if (rv != MACH_MSG_OK) {
		printf("  set_insert(A) failed (rv=%d)\n", rv);
		return (31);
	}
	rv = mach_port_set_insert(set_name, port_b);
	if (rv != MACH_MSG_OK) {
		printf("  set_insert(B) failed (rv=%d)\n", rv);
		return (32);
	}

	/* Both members must report the set's name. */
	if (mach_port_set_extract(port_a) != set_name) {
		printf("  set_extract(A) returned wrong set name\n");
		return (65);
	}
	if (mach_port_set_extract(port_b) != set_name) {
		printf("  set_extract(B) returned wrong set name\n");
		return (66);
	}
	printf("  port_set_extract: both members report set=0x%x OK\n",
	    (unsigned)set_name);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;

	tx.msgh_remote = port_a;
	tx.msgh_id     = PSET_TAG_A;
	rv = mach_msg_send(&tx);
	if (rv != MACH_MSG_OK) {
		printf("  send to A failed (rv=%d)\n", rv);
		return (33);
	}
	tx.msgh_remote = port_b;
	tx.msgh_id     = PSET_TAG_B;
	rv = mach_msg_send(&tx);
	if (rv != MACH_MSG_OK) {
		printf("  send to B failed (rv=%d)\n", rv);
		return (34);
	}

	rv = mach_msg_recv_timed(set_name, &rx, sizeof(rx), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  recv-on-set #1 failed (rv=%d)\n", rv);
		return (35);
	}
	got_a = rx.msgh_id;
	rv = mach_msg_recv_timed(set_name, &rx, sizeof(rx), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  recv-on-set #2 failed (rv=%d)\n", rv);
		return (36);
	}
	got_b = rx.msgh_id;

	if (!((got_a == PSET_TAG_A && got_b == PSET_TAG_B) ||
	      (got_a == PSET_TAG_B && got_b == PSET_TAG_A))) {
		printf("  set recv tags wrong: got 0x%x 0x%x expected "
		    "{0x%x, 0x%x}\n",
		    (unsigned)got_a, (unsigned)got_b,
		    (unsigned)PSET_TAG_A, (unsigned)PSET_TAG_B);
		return (37);
	}
	printf("  port_set recv: tags 0x%x 0x%x via set name 0x%x OK\n",
	    (unsigned)got_a, (unsigned)got_b, (unsigned)set_name);

	(void)mach_port_set_remove(set_name, port_a);
	(void)mach_port_set_remove(set_name, port_b);

	/* Post-remove: both members are standalone again. */
	if (mach_port_set_extract(port_a) != MACH_PORT_NULL) {
		printf("  set_extract(A) still reports set after remove\n");
		return (67);
	}
	if (mach_port_set_extract(port_b) != MACH_PORT_NULL) {
		printf("  set_extract(B) still reports set after remove\n");
		return (68);
	}

	(void)mach_port_deallocate(port_a);
	(void)mach_port_deallocate(port_b);
	(void)mach_port_deallocate(set_name);
	return (0);
}

/*
 * DEAD_NAME, the mirror of demo_no_senders: holding a send right
 * (required to register), learn when the receive right goes.  Drop the
 * source's receive right with mod_refs; the port dies and
 * MACH_NOTIFY_DEAD_NAME arrives on the notify port.
 */
#define	DEAD_NAME_TAG	0xCAFE0DEAu

static int
demo_dead_name(void)
{
	struct mach_notify_header	nh;
	mach_port_name_t		source, notify;
	int				rv;

	source = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (source == MACH_PORT_NULL) {
		printf("  port_allocate(source) failed\n");
		return (38);
	}
	notify = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (notify == MACH_PORT_NULL) {
		printf("  port_allocate(notify) failed\n");
		(void)mach_port_deallocate(source);
		return (39);
	}

	rv = mach_port_request_notification(source, MACH_NOTIFY_DEAD_NAME,
	    notify, DEAD_NAME_TAG);
	if (rv != MACH_MSG_OK) {
		printf("  request_notification(DEAD_NAME) failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (40);
	}

	/* Drop only the receive right; the send right stays as the name. */
	rv = mach_port_mod_refs(source, MACH_PORT_RIGHT_RECEIVE);
	if (rv != MACH_MSG_OK) {
		printf("  mod_refs(RECV) failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(source);
		(void)mach_port_deallocate(notify);
		return (41);
	}

	rv = mach_msg_recv_timed(notify, &nh.hdr, sizeof(nh), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  dead-name recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(notify);
		return (42);
	}
	if (nh.hdr.msgh_id != (uint32_t)MACH_NOTIFY_DEAD_NAME) {
		printf("  dead-name msgh_id=%u expected %u\n",
		    (unsigned)nh.hdr.msgh_id,
		    (unsigned)MACH_NOTIFY_DEAD_NAME);
		(void)mach_port_deallocate(notify);
		return (43);
	}
	if (nh.nh_msgid != DEAD_NAME_TAG) {
		printf("  dead-name nh_msgid=0x%x expected 0x%x\n",
		    (unsigned)nh.nh_msgid, (unsigned)DEAD_NAME_TAG);
		(void)mach_port_deallocate(notify);
		return (44);
	}
	printf("  DEAD_NAME notification: id=%u tag=0x%x OK\n",
	    (unsigned)nh.hdr.msgh_id, (unsigned)nh.nh_msgid);

	(void)mach_port_deallocate(notify);
	return (0);
}

/*
 * spawn_with_port: spawn hello.elf with a work port at the child's
 * MACH_PORT_PARENT and receive the tagged ping it sends from main()
 * before exiting.
 */
#define	HELLO_CHILD_PING_TAG	0xCAFEC417u

static int
demo_spawn_inject(void)
{
	struct mach_msg_header	rx;
	mach_port_name_t	work;
	long			child_id;
	int			rv;

	work = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (work == MACH_PORT_NULL) {
		printf("  port_allocate(work) failed\n");
		return (45);
	}

	child_id = spawn_with_port("hello", work);
	if (child_id < 0) {
		printf("  spawn_with_port failed (rv=%ld)\n", child_id);
		(void)mach_port_deallocate(work);
		return (46);
	}
	printf("  spawned child hello.elf, task_id=%lu, work_port=0x%x\n",
	    (unsigned long)child_id, (unsigned)work);

	rv = mach_msg_recv_timed(work, &rx, sizeof(rx), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  child-ping recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(work);
		return (47);
	}
	if (rx.msgh_id != HELLO_CHILD_PING_TAG) {
		printf("  child-ping tag mismatch: got 0x%x expected 0x%x\n",
		    (unsigned)rx.msgh_id, (unsigned)HELLO_CHILD_PING_TAG);
		(void)mach_port_deallocate(work);
		return (48);
	}
	printf("  spawn_with_port + child ping: tag=0x%x OK\n",
	    (unsigned)rx.msgh_id);

	(void)mach_port_deallocate(work);
	return (0);
}

/*
 * demo_ool_from_child: receive a bulk payload from another ring-3 task.
 * The other OOL demos talk to kernel services, which are dispatched in the
 * sender's context and never move the bytes; only a message queued
 * between user tasks hands the receiver the sender's frames.
 *
 *	The payload must match the pattern computed here, not a checksum
 *	  from the child.
 *	It must be what the child had at send time: oolchild overwrites its
 *	  buffer as soon as the send returns, so frames shared without
 *	  write-protecting the child's copy read 0xA5 here.
 *
 * Then we write the received range, the receiving half of copy-on-write.
 */
#define	OOLCHILD_PAGES	3
#define	OOLCHILD_BYTES	(OOLCHILD_PAGES * 4096u)
#define	OOLCHILD_TAG	0x00010CA1u

static int
demo_ool_from_child(void)
{
	struct {
		struct mach_msg_header		hdr;
		struct mach_msg_body		body;
		struct mach_msg_ool_descriptor	ool;
	}			rx;
	const uint8_t		*got;
	mach_port_name_t	 work;
	long			 child_id;
	unsigned		 i;
	int			 rv;

	work = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (work == MACH_PORT_NULL) {
		printf("  port_allocate(ool work) failed\n");
		return (80);
	}

	child_id = spawn_with_port("oolchild", work);
	if (child_id < 0) {
		printf("  spawn_with_port('oolchild') failed (rv=%ld)\n",
		    child_id);
		(void)mach_port_deallocate(work);
		return (81);
	}

	rv = mach_msg_recv_timed(work, &rx.hdr, sizeof(rx), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  oolchild payload recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(work);
		return (82);
	}
	if (rx.hdr.msgh_id != OOLCHILD_TAG) {
		printf("  oolchild tag mismatch: 0x%x\n",
		    (unsigned)rx.hdr.msgh_id);
		(void)mach_port_deallocate(work);
		return (83);
	}
	if ((rx.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX) == 0 ||
	    rx.body.msgh_descriptor_count != 1 ||
	    rx.ool.type != MACH_MSG_OOL_DESCRIPTOR ||
	    rx.ool.size != OOLCHILD_BYTES || rx.ool.address == 0) {
		printf("  oolchild descriptor malformed\n");
		(void)mach_port_deallocate(work);
		return (84);
	}

	got = (const uint8_t *)(uintptr_t)rx.ool.address;
	for (i = 0; i < OOLCHILD_BYTES; i++) {
		if (got[i] == (uint8_t)((i * 31u + 7u) & 0xFFu))
			continue;
		printf("  oolchild payload WRONG at %u: 0x%x (0xa5 means the "
		    "child's post-send writes reached us)\n",
		    i, (unsigned)got[i]);
		(void)mach_port_deallocate(work);
		return (85);
	}

	/*
	 * The range is mapped writable but its PTEs are not, so each page
	 * takes a copy-on-write fault and becomes ours.
	 */
	{
		uint8_t	*mine = (uint8_t *)(uintptr_t)rx.ool.address;

		for (i = 0; i < OOLCHILD_BYTES; i++)
			mine[i] = (uint8_t)(i & 0xFFu);
		for (i = 0; i < OOLCHILD_BYTES; i++) {
			if (mine[i] == (uint8_t)(i & 0xFFu))
				continue;
			printf("  received range did not keep our write at %u\n",
			    i);
			(void)mach_port_deallocate(work);
			return (86);
		}
	}

	printf("  OOL %u bytes from a ring-3 child: intact at send time, "
	    "writable after: OK\n", OOLCHILD_BYTES);
	(void)mach_port_deallocate(work);
	return (0);
}

/*
 * Exception port: `excchild' installs our watcher port (its
 * MACH_PORT_PARENT) as its exception port and NULL-derefs; the kernel
 * posts MACH_EXC_FAULT to it before retiring the thread.  Check trapno
 * 14 (#PF), cr2 0 and the task id.
 */
static int
demo_exception(void)
{
	struct mach_exception_header	eh;
	mach_port_name_t		watch;
	long				child_id;
	int				rv;

	watch = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (watch == MACH_PORT_NULL) {
		printf("  port_allocate(watch) failed\n");
		return (49);
	}

	child_id = spawn_with_port("excchild", watch);
	if (child_id < 0) {
		printf("  spawn_with_port(excchild) failed (rv=%ld)\n",
		    child_id);
		(void)mach_port_deallocate(watch);
		return (50);
	}
	printf("  spawned excchild, task_id=%lu, watch=0x%x\n",
	    (unsigned long)child_id, (unsigned)watch);

	rv = mach_msg_recv_timed(watch, &eh.hdr, sizeof(eh), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  exception recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(watch);
		return (51);
	}
	if (eh.hdr.msgh_id != (uint32_t)MACH_EXC_FAULT) {
		printf("  exc msgh_id=%u expected %u\n",
		    (unsigned)eh.hdr.msgh_id, (unsigned)MACH_EXC_FAULT);
		(void)mach_port_deallocate(watch);
		return (52);
	}
	if (eh.eh_trapno != 14) {
		printf("  exc trapno=%u expected 14 (#PF)\n",
		    (unsigned)eh.eh_trapno);
		(void)mach_port_deallocate(watch);
		return (53);
	}
	if (eh.eh_cr2 != 0) {
		printf("  exc cr2=0x%llx expected 0 (NULL deref)\n",
		    (unsigned long long)eh.eh_cr2);
		(void)mach_port_deallocate(watch);
		return (54);
	}
	if (eh.eh_task_id != (uint64_t)child_id) {
		printf("  exc task_id=%llu expected %lu\n",
		    (unsigned long long)eh.eh_task_id,
		    (unsigned long)child_id);
		(void)mach_port_deallocate(watch);
		return (55);
	}
	printf("  exception: vec=%u rip=0x%llx cr2=0x%llx task_id=%llu OK\n",
	    (unsigned)eh.eh_trapno,
	    (unsigned long long)eh.eh_rip,
	    (unsigned long long)eh.eh_cr2,
	    (unsigned long long)eh.eh_task_id);

	(void)mach_port_deallocate(watch);
	return (0);
}

/*
 * Reply protocol.  `excchild_resume' sets EXC_FLAG_RESUMABLE on its
 * BAD_INSTRUCTION slot and executes UD2.  We reply on the fault's
 * msgh_local with EXC_VERDICT_RESUME and rip_advance=2 (the length of
 * UD2); the kernel resumes the child past it, and the child sends
 * RESUME_SURVIVE_TAG to prove it ran on.
 */
#define	RESUME_SURVIVE_TAG	0xC0DEBA5Eu

static int
demo_exception_resume(void)
{
	struct mach_exception_header	eh;
	struct mach_msg_header		survive;
	struct mach_exception_reply	verdict;
	mach_port_name_t		watch;
	long				child_id;
	int				rv;

	watch = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (watch == MACH_PORT_NULL) {
		printf("  port_allocate(resume) failed\n");
		return (84);
	}

	child_id = spawn_with_port("excchild_resume", watch);
	if (child_id < 0) {
		printf("  spawn_with_port(excchild_resume) failed (rv=%ld)\n",
		    child_id);
		(void)mach_port_deallocate(watch);
		return (85);
	}
	printf("  spawned excchild_resume, task_id=%lu, watch=0x%x\n",
	    (unsigned long)child_id, (unsigned)watch);

	rv = mach_msg_recv_timed(watch, &eh.hdr, sizeof(eh), 2000);
	if (rv != MACH_MSG_OK || eh.hdr.msgh_id != (uint32_t)MACH_EXC_FAULT) {
		printf("  resume recv-exception failed "
		    "(rv=%d id=%u)\n", rv, (unsigned)eh.hdr.msgh_id);
		(void)mach_port_deallocate(watch);
		return (86);
	}
	if (eh.hdr.msgh_local == MACH_PORT_NULL) {
		printf("  resume: exception lacks reply port "
		    "(EXC_FLAG_RESUMABLE not honored?)\n");
		(void)mach_port_deallocate(watch);
		return (87);
	}

	/*
	 * msgh_remote is the send name the kernel minted in our space for
	 * the reply port; MOVE_SEND consumes it (one-shot reply).
	 */
	verdict.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0);
	verdict.hdr.msgh_size    = sizeof(verdict);
	verdict.hdr.msgh_remote  = eh.hdr.msgh_local;
	verdict.hdr.msgh_local   = MACH_PORT_NULL;
	verdict.hdr.msgh_voucher = 0;
	verdict.hdr.msgh_id      = MACH_EXC_REPLY;
	verdict.er_verdict       = EXC_VERDICT_RESUME;
	verdict.er_rip_advance   = 2;	/* sizeof(ud2)               */
	rv = mach_msg_send(&verdict.hdr);
	if (rv != MACH_MSG_OK) {
		printf("  resume verdict send failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(watch);
		return (88);
	}

	/*
	 * The child sends the survival tag after UD2.  Without RESUME it
	 * would be killed and this receive would time out.
	 */
	rv = mach_msg_recv_timed(watch, &survive, sizeof(survive), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  resume survive-tag recv failed (rv=%d) -- "
		    "child probably did not resume\n", rv);
		(void)mach_port_deallocate(watch);
		return (89);
	}
	if (survive.msgh_id != RESUME_SURVIVE_TAG) {
		printf("  resume survive-tag mismatch: got 0x%x expected 0x%x\n",
		    (unsigned)survive.msgh_id, (unsigned)RESUME_SURVIVE_TAG);
		(void)mach_port_deallocate(watch);
		return (90);
	}
	printf("  resume: RESUME verdict applied, child ran past ud2 "
	    "and posted survive tag 0x%x OK\n",
	    (unsigned)survive.msgh_id);

	(void)mach_port_deallocate(watch);
	return (0);
}

/*
 * Thread-level exception ports take precedence.  `excchild_thr' installs
 * our port at both the task- and thread-level BAD_INSTRUCTION slots and
 * executes UD2.  The thread-level hit short-circuits the task level, so
 * after the first MACH_EXC_FAULT a 200 ms receive must time out.
 */
static int
demo_exception_thread_level(void)
{
	struct mach_exception_header	eh;
	struct mach_msg_header		extra;
	mach_port_name_t		watch;
	long				child_id;
	int				rv;

	watch = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (watch == MACH_PORT_NULL) {
		printf("  port_allocate(thr) failed\n");
		return (80);
	}

	child_id = spawn_with_port("excchild_thr", watch);
	if (child_id < 0) {
		printf("  spawn_with_port(excchild_thr) failed (rv=%ld)\n",
		    child_id);
		(void)mach_port_deallocate(watch);
		return (81);
	}
	printf("  spawned excchild_thr, task_id=%lu, watch=0x%x\n",
	    (unsigned long)child_id, (unsigned)watch);

	rv = mach_msg_recv_timed(watch, &eh.hdr, sizeof(eh), 2000);
	if (rv != MACH_MSG_OK || eh.hdr.msgh_id != (uint32_t)MACH_EXC_FAULT ||
	    eh.eh_trapno != 6) {
		printf("  thr first recv unexpected (rv=%d id=%u trap=%u)\n",
		    rv, (unsigned)eh.hdr.msgh_id, (unsigned)eh.eh_trapno);
		(void)mach_port_deallocate(watch);
		return (82);
	}

	/* A second copy would mean the task-level slot fired too. */
	rv = mach_msg_recv_timed(watch, &extra, sizeof(extra), 200);
	if (rv != MACH_E_TIMEOUT) {
		printf("  precedence violated: second recv rv=%d "
		    "(expected E_TIMEOUT, task-level should have been "
		    "bypassed)\n", rv);
		(void)mach_port_deallocate(watch);
		return (83);
	}
	printf("  thread-level precedence: one MACH_EXC_FAULT, "
	    "task-level bypassed: OK\n");

	(void)mach_port_deallocate(watch);
	return (0);
}

/*
 * Per-type exception ports.  `excchild_ud' installs our port for
 * EXC_MASK_BAD_INSTRUCTION only and executes UD2; exc_type_from_trapno
 * routes trapno 6 there.  Check trapno 6 and the child's task id.
 */
static int
demo_exception_per_type(void)
{
	struct mach_exception_header	eh;
	mach_port_name_t		watch;
	long				child_id;
	int				rv;

	watch = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (watch == MACH_PORT_NULL) {
		printf("  port_allocate(per-type) failed\n");
		return (74);
	}

	child_id = spawn_with_port("excchild_ud", watch);
	if (child_id < 0) {
		printf("  spawn_with_port(excchild_ud) failed (rv=%ld)\n",
		    child_id);
		(void)mach_port_deallocate(watch);
		return (75);
	}
	printf("  spawned excchild_ud, task_id=%lu, watch=0x%x\n",
	    (unsigned long)child_id, (unsigned)watch);

	rv = mach_msg_recv_timed(watch, &eh.hdr, sizeof(eh), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  per-type recv failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(watch);
		return (76);
	}
	if (eh.hdr.msgh_id != (uint32_t)MACH_EXC_FAULT) {
		printf("  per-type msgh_id=%u expected %u\n",
		    (unsigned)eh.hdr.msgh_id, (unsigned)MACH_EXC_FAULT);
		(void)mach_port_deallocate(watch);
		return (77);
	}
	if (eh.eh_trapno != 6) {
		printf("  per-type trapno=%u expected 6 (#UD)\n",
		    (unsigned)eh.eh_trapno);
		(void)mach_port_deallocate(watch);
		return (78);
	}
	if (eh.eh_task_id != (uint64_t)child_id) {
		printf("  per-type task_id=%llu expected %lu\n",
		    (unsigned long long)eh.eh_task_id,
		    (unsigned long)child_id);
		(void)mach_port_deallocate(watch);
		return (79);
	}
	printf("  per-type exception: vec=%u rip=0x%llx task_id=%llu OK\n",
	    (unsigned)eh.eh_trapno,
	    (unsigned long long)eh.eh_rip,
	    (unsigned long long)eh.eh_task_id);

	(void)mach_port_deallocate(watch);
	return (0);
}

/*
 * Header strictness: a nonzero msgh_voucher and a reserved msgh_bits bit
 * (the 15 bits between the disposition lanes and COMPLEX at bit 31) are
 * each refused with MACH_E_INVAL, and a clean header still goes through.
 */
static int
demo_msg_strictness(void)
{
	struct mach_msg_header	tx;
	struct mach_msg_header	rx;
	mach_port_name_t	name;
	int			rv;

	name = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (name == MACH_PORT_NULL) {
		printf("  port_allocate(strict) failed\n");
		return (69);
	}

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = name;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0xDEADu;
	tx.msgh_id      = 0;

	rv = mach_msg_send(&tx);
	if (rv != MACH_E_INVAL) {
		printf("  nonzero voucher accepted (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (70);
	}

	/* Reserved bit 16 inside msgh_bits; must reject. */
	tx.msgh_voucher = 0;
	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0) |
	    0x00010000u;
	rv = mach_msg_send(&tx);
	if (rv != MACH_E_INVAL) {
		printf("  reserved msgh_bits accepted (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (71);
	}

	/* Clean header round-trips. */
	tx.msgh_bits = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_id   = 0x57727AFFu;
	rv = mach_msg_send(&tx);
	if (rv != MACH_MSG_OK) {
		printf("  clean send unexpectedly rejected (rv=%d)\n", rv);
		(void)mach_port_deallocate(name);
		return (72);
	}
	rv = mach_msg_recv_timed(name, &rx, sizeof(rx), 1000);
	if (rv != MACH_MSG_OK || rx.msgh_id != 0x57727AFFu) {
		printf("  clean recv failed (rv=%d id=0x%x)\n",
		    rv, (unsigned)rx.msgh_id);
		(void)mach_port_deallocate(name);
		return (73);
	}
	printf("  msg strictness: voucher + reserved bits rejected, clean OK\n");

	(void)mach_port_deallocate(name);
	return (0);
}

/* Name and tag for demo_bootstrap_publish. */
#define	PUB_DEMO_NAME	"hello.demo"
#define	PUB_DEMO_TAG	0xFEEDFACEu

/*
 * demo_launchctl_spawn: spawn the scripted `launchctl' demo against the
 * in-kernel launchd.  Many turns: it does several RPCs and spawns
 * grandchildren (echod and others).
 */
static int
demo_launchctl_spawn(void)
{
	long	child_id;
	int	i;

	printf("\nlaunchctl demo:\n");
	child_id = spawn("launchctl");
	if (child_id < 0) {
		printf("  spawn('launchctl') failed (rv=%ld)\n", child_id);
		return (74);
	}
	(void)child_id;
	for (i = 0; i < 256; i++)
		(void)poll_turn();
	return (0);
}

/*
 * demo_selfkill_spawn: `selfkill' kills itself through
 * MACH_PORT_TASK_SELF.  Detection point #5 (syscall exit) retires it
 * before sysretq, so its BUG line never appears.
 */
static int
demo_selfkill_spawn(void)
{
	long	child_id;
	int	i;
	int	alive;

	printf("\nselfkill demo:\n");
	child_id = spawn("selfkill");
	if (child_id < 0) {
		printf("  spawn('selfkill') failed (rv=%ld)\n", child_id);
		return (76);
	}
	alive = 1;
	for (i = 0; i < 128 && alive; i++) {
		(void)poll_turn();
		alive = task_alive((uint64_t)child_id);
	}
	printf("  task_alive(%ld) after self-kill: %s (waited %d turns)\n",
	    child_id, alive ? "yes (kill failed)" : "no (kill landed)", i);
	return (0);
}

/*
 * demo_parent_managed_kill: spawn_returns_taskport gives the parent the
 * task id and a send right on the child's task port in one syscall, so
 * killing needs no bootstrap lookup (sh.c's child table works this way).
 */
static int
demo_parent_managed_kill(void)
{
	mach_port_name_t	taskport;
	long			child_id;
	int			i;
	int			alive;
	int			rv;

	printf("\nparent-managed kill demo (spawn_returns_taskport):\n");
	taskport = MACH_PORT_NULL;
	child_id = spawn_returns_taskport("loopchild", &taskport);
	if (child_id < 0) {
		printf("  spawn_returns_taskport('loopchild') failed (rv=%ld)\n",
		    child_id);
		return (80);
	}
	if (taskport == MACH_PORT_NULL) {
		printf("  spawn returned task_id=%ld but no taskport\n",
		    child_id);
		return (81);
	}
	printf("  spawned task_id=%ld with taskport=0x%x in our space\n",
	    child_id, (unsigned)taskport);

	/*
	 * Let loopchild reach its loop first; killing it earlier is
	 * correct but makes a muddled boot log.
	 */
	for (i = 0; i < 16; i++)
		(void)poll_turn();

	rv = task_kill(taskport);
	printf("  task_kill(taskport) -> %d\n", rv);

	alive = 1;
	for (i = 0; i < 64 && alive; i++) {
		(void)poll_turn();
		alive = task_alive((uint64_t)child_id);
	}
	printf("  task_alive(%ld) after parent-managed kill: %s "
	    "(waited %d turns)\n",
	    child_id,
	    alive ? "yes (capability path BROKEN)" : "no (capability path OK)",
	    i);

	(void)mach_port_deallocate(taskport);
	return (0);
}

/*
 * demo_compute_kill_spawn: detection point #4 (interrupt return to ring
 * 3).  loopchild publishes its task port under bootstrap and spins with
 * no syscalls; we look it up and task_kill it.  Only the IRQ-return
 * check can catch it, at the next timer tick.
 */
static int
demo_compute_kill_spawn(void)
{
	mach_port_name_t	tport;
	long			child_id;
	int			i;
	int			alive;
	int			rv;

	printf("\ncompute-loop kill demo:\n");
	child_id = spawn("loopchild");
	if (child_id < 0) {
		printf("  spawn('loopchild') failed (rv=%ld)\n", child_id);
		return (78);
	}

	/*
	 * Wait for loopchild to register.  The budget is in turns, not
	 * yields: with several CPUs a yield may find nothing to run and
	 * return at once, while poll_turn always waits about a tick (see
	 * lib/style9_sys.c).
	 */
	tport = MACH_PORT_NULL;
	for (i = 0; i < 64 && tport == MACH_PORT_NULL; i++) {
		(void)poll_turn();
		tport = bootstrap_lookup("loopchild.tport");
	}
	if (tport == MACH_PORT_NULL) {
		printf("  loopchild.tport lookup failed after %d turns\n", i);
		return (79);
	}
	printf("  loopchild.tport = 0x%x (after %d lookup turns)\n",
	    (unsigned)tport, i);

	rv = task_kill(tport);
	printf("  task_kill(loopchild) -> %d\n", rv);

	/* Only the IRQ-return check can retire it; bounded wait. */
	alive = 1;
	for (i = 0; i < 128 && alive; i++) {
		(void)poll_turn();
		alive = task_alive((uint64_t)child_id);
	}
	printf("  task_alive(%ld) after task_kill: %s (waited %d turns)\n",
	    child_id, alive ? "yes (IRQ-return MISSED)" : "no (IRQ-return OK)",
	    i);

	(void)mach_port_deallocate(tport);
	return (0);
}

/* demo_vmmap_spawn: spawn `vmmap' and give it time to print. */
static int
demo_vmmap_spawn(void)
{
	long	child_id;
	int	i;

	printf("\nvmmap demo:\n");
	child_id = spawn("vmmap");
	if (child_id < 0) {
		printf("  spawn('vmmap') failed (rv=%ld)\n", child_id);
		return (72);
	}
	(void)child_id;
	for (i = 0; i < 64; i++)
		(void)poll_turn();
	return (0);
}

/*
 * demo_lsmp_spawn: spawn `lsmp', which builds a varied port space and
 * prints its own snapshot into the boot log.
 */
static int
demo_lsmp_spawn(void)
{
	long	child_id;
	int	i;

	printf("\nlsmp demo:\n");
	child_id = spawn("lsmp");
	if (child_id < 0) {
		printf("  spawn('lsmp') failed (rv=%ld)\n", child_id);
		return (70);
	}
	(void)child_id;

	/*
	 * lsmp sends us nothing to block on, so wait a fixed number of
	 * turns, far more than it needs.  Output still in flight after
	 * that just interleaves with the boot log.
	 */
	for (i = 0; i < 64; i++)
		(void)poll_turn();
	return (0);
}

/*
 * demo_top_spawn: spawn `top' and wait enough turns for all its samples
 * to reach the boot log; as with lsmp there is nothing to block on.
 */
static int
demo_top_spawn(void)
{
	long	child_id;
	int	i;

	printf("\ntop demo:\n");
	child_id = spawn("top");
	if (child_id < 0) {
		printf("  spawn('top') failed (rv=%ld)\n", child_id);
		return (73);
	}
	(void)child_id;
	for (i = 0; i < 160; i++)
		(void)poll_turn();
	return (0);
}

/*
 * demo_stale_taskport: a task port can outlive its task, since any
 * outside send right (here the one spawn_returns_taskport gives us)
 * keeps the port past task_deref's free.  Neither space_lookup nor the
 * special-port intercept checks p_dead, so the port stores the task's
 * id, not a pointer, and the kernel resolves it with task_lookup_ref.
 *
 * Spawn loopchild, check a GET_INFO RPC to its task port works, kill it
 * through that port, wait until it leaves the live list, then use the
 * stale port: GET_INFO must return MACH_E_DEAD and task_kill must do
 * nothing, neither touching freed memory.
 */
static int
demo_stale_taskport(void)
{
	struct mach_msg_header	tx;
	struct {
		struct mach_msg_header	hdr;
		struct task_info_reply	body;
	} reply;
	mach_port_name_t	tport;
	long			child_id;
	int			i;
	int			rv;

	printf("\nstale taskport demo (UAF guard):\n");

	tport = MACH_PORT_NULL;
	child_id = spawn_returns_taskport("loopchild", &tport);
	if (child_id < 0 || tport == MACH_PORT_NULL) {
		printf("  spawn_returns_taskport('loopchild') failed (rv=%ld)\n",
		    child_id);
		return (80);
	}
	printf("  spawned loopchild id=%ld taskport=0x%x\n",
	    child_id, (unsigned)tport);

	/* Positive control: cross-task GET_INFO on a live task port. */
	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = tport;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = TASK_OP_GET_INFO;
	rv = mach_msg_rpc(&tx, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  live taskport GET_INFO failed (rv=%d)\n", rv);
		return (81);
	}
	if (reply.body.tir_task_id != (uint64_t)child_id) {
		printf("  live taskport GET_INFO id mismatch: got %llu want %ld\n",
		    (unsigned long long)reply.body.tir_task_id, child_id);
		return (82);
	}
	printf("  live taskport GET_INFO ok: name='%s' id=%llu\n",
	    reply.body.tir_name,
	    (unsigned long long)reply.body.tir_task_id);

	/* Kill the child through the very capability we hold. */
	rv = task_kill(tport);
	if (rv != MACH_MSG_OK) {
		printf("  task_kill(taskport) failed (rv=%d)\n", rv);
		return (83);
	}

	/* Wait for the task to leave the live list (struct task freed). */
	for (i = 0; i < 256 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	if (task_alive((uint64_t)child_id)) {
		printf("  child still alive after 256 turns; "
		    "skipping stale assertion\n");
		(void)mach_port_deallocate(tport);
		return (0);
	}
	printf("  child reaped; taskport 0x%x is now stale\n",
	    (unsigned)tport);

	/*
	 * GET_INFO on the stale port: p_special_arg holds the dead child's
	 * id, task_lookup_ref returns NULL, the intercept answers
	 * MACH_E_DEAD.
	 */
	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = tport;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = TASK_OP_GET_INFO;
	rv = mach_msg_rpc(&tx, &reply.hdr, sizeof(reply), 1000);
	if (rv != MACH_E_DEAD) {
		printf("  FAIL: stale GET_INFO rv=%d (want MACH_E_DEAD=%d)\n",
		    rv, MACH_E_DEAD);
		(void)mach_port_deallocate(tport);
		return (84);
	}
	printf("  stale taskport GET_INFO correctly refused: MACH_E_DEAD\n");

	/*
	 * task_kill on the stale name: sys_task_kill reads the dead id and
	 * task_request_terminate finds no task.
	 */
	rv = task_kill(tport);
	if (rv == MACH_MSG_OK)
		printf("  task_kill(stale taskport) safely no-op'd\n");
	else
		printf("  task_kill(stale) rv=%d (no crash either way)\n", rv);

	(void)mach_port_deallocate(tport);
	return (0);
}

/*
 * demo_spawn_argv: SYS_SPAWN_ARGS.  argecho prints the vector it got,
 * so the boot transcript shows the kernel's stack builder and crt0
 * delivering it to main(argc, argv).
 */
static int
demo_spawn_argv(void)
{
	char			*child_argv[4];
	mach_port_name_t	 taskport;
	long			 child_id;
	int			 i;

	printf("\nspawn argv demo (SYS_SPAWN_ARGS):\n");

	child_argv[0] = "argecho";
	child_argv[1] = "alpha";
	child_argv[2] = "bravo";
	child_argv[3] = "charlie";

	taskport = MACH_PORT_NULL;
	child_id = spawn_args("argecho", 4, child_argv, &taskport);
	if (child_id < 0 || taskport == MACH_PORT_NULL) {
		printf("  spawn_args('argecho') failed (rv=%ld)\n", child_id);
		return (85);
	}
	printf("  spawned argecho id=%ld taskport=0x%x with 4 args\n",
	    child_id, (unsigned)taskport);

	/* Give argecho the CPU to run + print, then confirm it retired. */
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();

	(void)mach_port_deallocate(taskport);
	printf("  argecho retired after %d turns\n", i);
	return (0);
}

/*
 * demo_macho_spawn: the Mach-O container loader (kern/macho.c).
 * machotest is a style9 program wrapped as a Mach-O.  The thin image is
 * spawned with arguments, showing main(argc, argv) works whatever the
 * container; then the one-slice fat archive drives macho_load's slice
 * picker.  Each is waited out so the outputs do not interleave.
 */
static int
demo_macho_spawn(void)
{
	char			*child_argv[3];
	mach_port_name_t	 taskport;
	long			 child_id;
	int			 i;

	printf("\nMach-O loader demo (kern/macho.c):\n");

	child_argv[0] = "machotest";
	child_argv[1] = "thin";
	child_argv[2] = "slice";
	taskport = MACH_PORT_NULL;
	child_id = spawn_args("machotest", 3, child_argv, &taskport);
	if (child_id < 0) {
		printf("  spawn_args('machotest') failed (rv=%ld)\n", child_id);
		return (86);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	if (taskport != MACH_PORT_NULL)
		(void)mach_port_deallocate(taskport);
	printf("  thin Mach-O retired after %d turns\n", i);

	child_id = spawn("machotest_fat");
	if (child_id < 0) {
		printf("  spawn('machotest_fat') failed (rv=%ld)\n", child_id);
		return (87);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	printf("  fat Mach-O (x86_64 slice) retired after %d turns\n", i);
	return (0);
}

/*
 * demo_darwin_spawn: the Darwin personality (kern/darwin.c), in order:
 * darwinhello (raw class-encoded syscalls and the carry/errno
 * convention), darwinmsg (a mach_msg round trip), dyldhello and dyldbig
 * (dynamic Mach-Os through our dyld and libSystem), our own ABI probes,
 * the host and task ports, then real Apple binaries: figlet, tree, the
 * coreutils, gmake and dash.  Each child is waited on with a bounded
 * number of turns; output goes to the boot transcript.
 */
static int
demo_darwin_spawn(void)
{
	long	child_id;
	int	i;

	printf("\nDarwin syscall personality demo (kern/darwin.c):\n");

	child_id = spawn("darwinhello");
	if (child_id < 0) {
		printf("  spawn('darwinhello') failed (rv=%ld)\n", child_id);
		return (88);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	printf("  darwinhello (Darwin ABI) retired after %d turns\n", i);

	child_id = spawn("darwinmsg");
	if (child_id < 0) {
		printf("  spawn('darwinmsg') failed (rv=%ld)\n", child_id);
		return (89);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	printf("  darwinmsg (Darwin mach_msg) retired after %d turns\n", i);

	child_id = spawn("dyldhello");
	if (child_id < 0) {
		printf("  spawn('dyldhello') failed (rv=%ld)\n", child_id);
		return (90);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	printf("  dyldhello (dyld + libSystem) retired after %d turns\n", i);

	child_id = spawn("dyldbig");
	if (child_id < 0) {
		printf("  spawn('dyldbig') failed (rv=%ld)\n", child_id);
		return (91);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	printf("  dyldbig (real-Apple base 0x100000000, relocated low) retired "
	    "after %d turns\n", i);

	/*
	 * dirlist: our probe walking the volume through libSystem's
	 * $INODE64 opendir/readdir/stat, ahead of tree(1).
	 */
	child_id = spawn("dirlist");
	if (child_id < 0) {
		printf("  spawn('dirlist') failed (rv=%ld)\n", child_id);
		return (93);
	}
	for (i = 0; i < 256 && task_alive((uint64_t)child_id); i++)
		(void)poll_turn();
	printf("  dirlist (opendir/readdir probe) retired after %d "
	    "turns\n", i);

	/*
	 * The host port behind mach_host_self(): RPC its page-size and
	 * machine-info opcodes and print the answers.
	 */
	{
		struct svc_host_info_reply	info;
		mach_port_name_t		host;
		uint32_t			ps;
		int				rv;

		printf("  >>> host port: mach_host_self + host_page_size + "
		    "host_info <<<\n");
		host = mach_host_self();
		if (host == MACH_PORT_NULL) {
			printf("  mach_host_self() failed\n");
			return (96);
		}
		rv = host_page_size(host, &ps);
		if (rv != MACH_MSG_OK) {
			printf("  host_page_size rv=%d\n", rv);
			(void)mach_port_deallocate(host);
			return (96);
		}
		rv = host_info(host, &info);
		if (rv != MACH_MSG_OK) {
			printf("  host_info rv=%d\n", rv);
			(void)mach_port_deallocate(host);
			return (96);
		}
		printf("  host: page_size=%u cpus=%u/%u mem=%lluMiB "
		    "free=%lluMiB cpu_type=0x%x sub=%u\n",
		    ps, info.hi_avail_cpus, info.hi_max_cpus,
		    (unsigned long long)(info.hi_memory_size >> 20),
		    (unsigned long long)(info.hi_memory_free >> 20),
		    info.hi_cpu_type, info.hi_cpu_subtype);
		(void)mach_port_deallocate(host);
	}

	/*
	 * VM through our own task port: allocate as a Mach RPC (as a Darwin
	 * binary's mach_vm_allocate does), check it as demo_vm_allocate
	 * does, and deallocate through the port.
	 */
	{
		uint64_t	addr;
		uint8_t		*buf;
		uint32_t	i;
		int		rv;

		printf("  >>> task port: task_vm_allocate + write + "
		    "task_vm_deallocate <<<\n");
		addr = 0;
		rv = task_vm_allocate(MACH_PORT_TASK_SELF, 8192,
		    VM_PROT_READ | VM_PROT_WRITE, &addr);
		if (rv != MACH_MSG_OK || addr == 0) {
			printf("  task_vm_allocate rv=%d addr=%p\n", rv,
			    (void *)(uintptr_t)addr);
			return (95);
		}
		buf = (uint8_t *)(uintptr_t)addr;

		for (i = 0; i < 8192; i++) {
			if (buf[i] != 0) {
				printf("  task vm: not zero at i=%u\n",
				    (unsigned)i);
				(void)task_vm_deallocate(MACH_PORT_TASK_SELF,
				    addr, 8192);
				return (95);
			}
		}
		for (i = 0; i < 8192; i++)
			buf[i] = (uint8_t)(i & 0xFFu);
		for (i = 0; i < 8192; i++) {
			if (buf[i] != (uint8_t)(i & 0xFFu)) {
				printf("  task vm: mismatch at i=%u\n",
				    (unsigned)i);
				(void)task_vm_deallocate(MACH_PORT_TASK_SELF,
				    addr, 8192);
				return (95);
			}
		}

		rv = task_vm_deallocate(MACH_PORT_TASK_SELF, addr, 8192);
		if (rv != MACH_MSG_OK) {
			printf("  task_vm_deallocate rv=%d\n", rv);
			return (95);
		}
		printf("  task vm: VA=%p (zero-filled, writable, freed "
		    "via task port)\n", buf);
	}

	/*
	 * task_get_special_port: the host port comes back in a port
	 * descriptor of a complex reply, as on Mach, and must answer
	 * host_page_size; the bootstrap index must resolve too.
	 */
	{
		mach_port_name_t	bs;
		mach_port_name_t	h2;
		uint32_t		ps;
		int			rv;

		printf("  >>> task port: task_get_special_port(HOST, "
		    "BOOTSTRAP) <<<\n");
		rv = task_get_special_port(MACH_PORT_TASK_SELF,
		    TASK_SPECIAL_HOST, &h2);
		if (rv != MACH_MSG_OK || h2 == MACH_PORT_NULL) {
			printf("  get_special_port(HOST) rv=%d\n", rv);
			return (94);
		}
		ps = 0;
		rv = host_page_size(h2, &ps);
		if (rv != MACH_MSG_OK || ps != 4096) {
			printf("  host_page_size via task port rv=%d ps=%u\n",
			    rv, (unsigned)ps);
			(void)mach_port_deallocate(h2);
			return (94);
		}
		(void)mach_port_deallocate(h2);

		rv = task_get_special_port(MACH_PORT_TASK_SELF,
		    TASK_SPECIAL_BOOTSTRAP, &bs);
		if (rv != MACH_MSG_OK || bs == MACH_PORT_NULL) {
			printf("  get_special_port(BOOTSTRAP) rv=%d\n", rv);
			return (94);
		}
		(void)mach_port_deallocate(bs);
		printf("  task get_special_port: HOST live (page_size=%u) "
		    "+ BOOTSTRAP resolved\n", (unsigned)ps);
	}

	/*
	 * figlet, a Homebrew bottle: a genuine Apple-toolchain dynamic
	 * Mach-O, relocated low by macho_load and bound by our dyld against
	 * our libSystem.  Its argument goes through our getopt and stdio.
	 */
	{
		mach_port_name_t	figlet_tp;
		char			*figlet_argv[2];

		figlet_tp = MACH_PORT_NULL;
		figlet_argv[0] = "figlet";
		figlet_argv[1] = "hi";
		printf("  >>> spawning figlet -- a REAL Apple x86-64 macOS "
		    "binary <<<\n");
		child_id = spawn_args("figlet", 2, figlet_argv, &figlet_tp);
		if (child_id < 0) {
			printf("  spawn_args('figlet') failed (rv=%ld)\n",
			    child_id);
			return (92);
		}
		for (i = 0; i < 256 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  figlet retired after %d turns\n", i);
	}

	/*
	 * tree(1) (Homebrew): walks the whole volume from "/" through
	 * libSystem's opendir/readdir/lstat/stat.
	 */
	{
		mach_port_name_t	tree_tp;
		char			*tree_argv[2];

		tree_tp = MACH_PORT_NULL;
		tree_argv[0] = "tree";
		tree_argv[1] = "/";
		printf("  >>> spawning tree -- a REAL Apple x86-64 macOS "
		    "binary <<<\n");
		child_id = spawn_args("tree", 2, tree_argv, &tree_tp);
		if (child_id < 0) {
			printf("  spawn_args('tree') failed (rv=%ld)\n",
			    child_id);
			return (94);
		}
		for (i = 0; i < 512 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  tree retired after %d turns\n", i);
	}

	/*
	 * gcat (coreutils cat): a file's bytes from the disk, out of an
	 * unmodified Apple binary.  /etc/hello.txt is on the APFS image and
	 * /docs/readme.txt on the FAT one, so whichever is attached, cat
	 * prints one and reports the other missing -- its own message,
	 * chosen from our errno.
	 */
	{
		mach_port_name_t	gcat_tp;
		char			*gcat_argv[3];

		gcat_tp = MACH_PORT_NULL;
		gcat_argv[0] = "gcat";
		gcat_argv[1] = "/etc/hello.txt";
		gcat_argv[2] = "/docs/readme.txt";
		printf("  >>> spawning gcat -- a REAL Apple x86-64 macOS "
		    "binary reading a file off the volume <<<\n");
		child_id = spawn_args("gcat", 3, gcat_argv, &gcat_tp);
		if (child_id < 0) {
			printf("  spawn_args('gcat') failed (rv=%ld)\n",
			    child_id);
			return (95);
		}
		for (i = 0; i < 512 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gcat retired after %d turns\n", i);
	}

	/*
	 * gls -l (coreutils ls): every column but the name comes from the
	 * inode, so a plausible line means mode, link count, owner, size
	 * and date survived from the on-disk record.  /bin is synthetic
	 * (files in the kernel image); / is the attached disk.
	 */
	{
		mach_port_name_t	gls_tp;
		char			*gls_argv[4];

		gls_tp = MACH_PORT_NULL;
		gls_argv[0] = "gls";
		gls_argv[1] = "-l";
		gls_argv[2] = "/";
		gls_argv[3] = "/bin";
		printf("  >>> spawning gls -l -- a REAL Apple x86-64 macOS "
		    "binary printing what the inodes say <<<\n");
		child_id = spawn_args("gls", 4, gls_argv, &gls_tp);
		if (child_id < 0) {
			printf("  spawn_args('gls') failed (rv=%ld)\n",
			    child_id);
			return (96);
		}
		for (i = 0; i < 4096 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gls retired after %d turns\n", i);
	}

	/*
	 * The wall clock from ring 3: plausible date, advances, never runs
	 * backwards.
	 */
	{
		mach_port_name_t	tprobe_tp;

		tprobe_tp = MACH_PORT_NULL;
		printf("  >>> spawning timeprobe -- wall-clock probe "
		    "(gettimeofday / clock_gettime / time) <<<\n");
		child_id = spawn_args("timeprobe", 0, NULL, &tprobe_tp);
		if (child_id < 0) {
			printf("  spawn_args('timeprobe') failed (rv=%ld)\n",
			    child_id);
			return (95);
		}
		for (i = 0; i < 512 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  timeprobe retired after %d turns\n", i);
	}

	/*
	 * Demand paging from ring 3: map more than the machine has, let the
	 * kernel be first to write a page, compare a file mapping with
	 * read(2).
	 */
	{
		mach_port_name_t	mprobe_tp;

		mprobe_tp = MACH_PORT_NULL;
		printf("  >>> spawning mmaptest -- demand-paging probe "
		    "(mmap / munmap / the pager) <<<\n");
		child_id = spawn_args("mmaptest", 0, NULL, &mprobe_tp);
		if (child_id < 0) {
			printf("  spawn_args('mmaptest') failed (rv=%ld)\n",
			    child_id);
			return (95);
		}
		for (i = 0; i < 4096 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  mmaptest retired after %d turns\n", i);
	}

	/* filewrite: ring 3 creating, changing and removing files. */
	{
		mach_port_name_t	wprobe_tp;

		wprobe_tp = MACH_PORT_NULL;
		printf("  >>> spawning filewrite -- the volume, from ring 3 "
		    "(open O_CREAT / write / append / truncate / unlink) <<<\n");
		child_id = spawn_args("filewrite", 0, NULL, &wprobe_tp);
		if (child_id < 0) {
			printf("  spawn_args('filewrite') failed (rv=%ld)\n",
			    child_id);
			return (96);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  filewrite retired after %d turns\n", i);
	}

	/*
	 * ttyprobe: terminal control.  We feed it one byte with no newline,
	 * which a canonical terminal would hold forever; reading it proves
	 * raw mode.  The feed goes in before the spawn: a session's feed
	 * ends with the session, and the probe's own session must find it.
	 */
	{
		mach_port_name_t	tprobe_tp;

		tprobe_tp = MACH_PORT_NULL;
		printf("  >>> spawning ttyprobe -- the terminal, from ring 3 "
		    "(tcgetattr / tcsetattr / TIOCGWINSZ / raw) <<<\n");
		(void)cons_feed("q", 1);
		child_id = spawn_args("ttyprobe", 0, NULL, &tprobe_tp);
		if (child_id < 0) {
			printf("  spawn_args('ttyprobe') failed (rv=%ld)\n",
			    child_id);
			return (97);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  ttyprobe retired after %d turns\n", i);
	}

	/*
	 * gstty, the oracle for ttyprobe: `stty -a' prints every termios
	 * flag as a Mac would, `stty -echo' changes our terminal and `stty
	 * sane' restores it, so Apple code drives tcsetattr both ways.
	 */
	{
		mach_port_name_t	 stty_tp;
		char			*stty_argv[3];

		stty_argv[0] = "gstty";
		stty_argv[1] = "-a";
		stty_argv[2] = NULL;
		stty_tp = MACH_PORT_NULL;
		printf("  >>> gstty -a -- a REAL Apple binary READING OUR "
		    "TERMINAL <<<\n");
		child_id = spawn_args("gstty", 2, stty_argv, &stty_tp);
		if (child_id < 0) {
			printf("  spawn_args('gstty -a') failed (rv=%ld)\n",
			    child_id);
			return (98);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gstty[-a] retired after %d turns\n", i);

		stty_argv[1] = "-echo";
		stty_tp = MACH_PORT_NULL;
		printf("  >>> gstty -echo -- and now CHANGING it <<<\n");
		child_id = spawn_args("gstty", 2, stty_argv, &stty_tp);
		if (child_id < 0) {
			printf("  spawn_args('gstty -echo') failed (rv=%ld)\n",
			    child_id);
			return (98);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gstty[-echo] retired after %d turns\n", i);

		stty_argv[1] = "sane";
		stty_tp = MACH_PORT_NULL;
		child_id = spawn_args("gstty", 2, stty_argv, &stty_tp);
		if (child_id < 0) {
			printf("  spawn_args('gstty sane') failed (rv=%ld)\n",
			    child_id);
			return (98);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gstty[sane] retired after %d turns\n", i);
	}

	/*
	 * gmkdir and grmdir: `mkdir -m 700', then gls -ld shows the mode
	 * that arrived (umask applied, bits written), then rmdir.
	 */
	{
		mach_port_name_t	 dir_tp;
		char			*dir_argv[4];

		printf("  >>> gmkdir / grmdir -- REAL Apple binaries making "
		    "and removing a directory <<<\n");
		dir_argv[0] = "gmkdir";
		dir_argv[1] = "-m";
		dir_argv[2] = "700";
		dir_argv[3] = "/etc/appledir";
		dir_tp = MACH_PORT_NULL;
		child_id = spawn_args("gmkdir", 4, dir_argv, &dir_tp);
		if (child_id < 0) {
			printf("  spawn_args('gmkdir') failed (rv=%ld)\n",
			    child_id);
			return (99);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gmkdir retired after %d turns\n", i);

		dir_argv[0] = "gls";
		dir_argv[1] = "-ld";
		dir_argv[2] = "/etc/appledir";
		dir_argv[3] = NULL;
		dir_tp = MACH_PORT_NULL;
		child_id = spawn_args("gls", 3, dir_argv, &dir_tp);
		if (child_id >= 0)
			for (i = 0; i < 8192 &&
			    task_alive((uint64_t)child_id); i++)
				(void)poll_turn();

		dir_argv[0] = "grmdir";
		dir_argv[1] = "/etc/appledir";
		dir_argv[2] = NULL;
		dir_tp = MACH_PORT_NULL;
		child_id = spawn_args("grmdir", 2, dir_argv, &dir_tp);
		if (child_id < 0) {
			printf("  spawn_args('grmdir') failed (rv=%ld)\n",
			    child_id);
			return (99);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  grmdir retired after %d turns\n", i);
	}

	/*
	 * gmake, via user/makedemo.sh: writes a small project and drives
	 * make through a build, a rebuild that finds nothing to do, a
	 * parallel build over a jobserver pipe (select/pselect) and a
	 * failing recipe.  The script prints its own PASS/FAIL lines.
	 */
	{
		mach_port_name_t	 mk_tp;
		char			*mk_argv[3];

		printf("  >>> gmake -- a REAL Apple build tool, running a "
		    "REAL build <<<\n");
		mk_argv[0] = "dash";
		mk_argv[1] = "/bin/makedemo.sh";
		mk_argv[2] = NULL;
		mk_tp = MACH_PORT_NULL;
		child_id = spawn_args("dash", 2, mk_argv, &mk_tp);
		if (child_id < 0) {
			printf("  spawn_args('dash makedemo.sh') failed "
			    "(rv=%ld)\n", child_id);
			return (99);
		}
		for (i = 0; i < 65536 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  dash[makedemo] retired after %d turns\n", i);
	}

	/*
	 * guname -a (coreutils uname): prints the Darwin identity uname(2)
	 * returns, "Darwin style9 23.6.0 ... x86_64".
	 */
	{
		mach_port_name_t	guname_tp;
		char			*guname_argv[2];

		guname_tp = MACH_PORT_NULL;
		guname_argv[0] = "guname";
		guname_argv[1] = "-a";
		printf("  >>> spawning guname -- a REAL Apple x86-64 macOS "
		    "binary (machine-identity trick) <<<\n");
		child_id = spawn_args("guname", 2, guname_argv, &guname_tp);
		if (child_id < 0) {
			printf("  spawn_args('guname') failed (rv=%ld)\n",
			    child_id);
			return (95);
		}
		for (i = 0; i < 512 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  guname retired after %d turns\n", i);
	}

	/*
	 * gfactor (coreutils factor) binds libgmp's __gmpz_/__gmpn_ symbols
	 * and libSystem's libc, so dyld maps gfactor -> libgmp -> libSystem
	 * and resolves each import by its lib_ordinal.  Two values use
	 * factor's own word arithmetic; 2^128 is past its width and runs
	 * inside libgmp.
	 */
	{
		const char		*gtests[3];
		mach_port_name_t	 gfactor_tp;
		char			*gfactor_argv[2];
		int			 t;

		gtests[0] = "42";          /* native path: 42: 2 3 7 */
		gtests[1] = "600851475143"; /* native: 71 839 1471 6857 (Euler P3) */
		gtests[2] = "340282366920938463463374607431768211456"; /* 2^128: gmp */
		gfactor_argv[0] = "gfactor";
		printf("  >>> spawning gfactor -- a REAL Apple binary linking a "
		    "SECOND dylib (libgmp) <<<\n");
		for (t = 0; t < 3; t++) {
			gfactor_tp = MACH_PORT_NULL;
			gfactor_argv[1] = (char *)gtests[t];
			printf("  >>> gfactor %s <<<\n", gfactor_argv[1]);
			child_id = spawn_args("gfactor", 2, gfactor_argv,
			    &gfactor_tp);
			if (child_id < 0) {
				printf("  spawn_args('gfactor') failed "
				    "(rv=%ld)\n", child_id);
				return (96);
			}
			for (i = 0; i < 4096 &&
			    task_alive((uint64_t)child_id); i++)
				(void)poll_turn();
			printf("  gfactor[%d] retired after %d turns\n", t, i);
		}
	}

	/*
	 * Darwin tasks creating, replacing and reaping others: pipefork (our
	 * probe), then genv, which exec(2)s gfactor in place, and gtimeout,
	 * which fork(2)s and wait4(2)s it and would kill(2) it on expiry.
	 */
	{
		mach_port_name_t	 proc_tp;
		char			*proc_argv[5];

		printf("  >>> pipefork: fork/execve/wait4/pipe/dup2 probe "
		    "<<<\n");
		proc_tp = MACH_PORT_NULL;
		child_id = spawn_args("pipefork", 0, NULL, &proc_tp);
		if (child_id < 0) {
			printf("  spawn_args('pipefork') failed (rv=%ld)\n",
			    child_id);
			return (97);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  pipefork retired after %d turns\n", i);

		printf("  >>> genv /bin/gfactor 42 -- a REAL Apple binary "
		    "exec(2)ing another <<<\n");
		proc_tp = MACH_PORT_NULL;
		proc_argv[0] = "genv";
		proc_argv[1] = "/bin/gfactor";
		proc_argv[2] = "42";
		proc_argv[3] = NULL;
		child_id = spawn_args("genv", 3, proc_argv, &proc_tp);
		if (child_id < 0) {
			printf("  spawn_args('genv') failed (rv=%ld)\n",
			    child_id);
			return (98);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  genv retired after %d turns\n", i);

		printf("  >>> gtimeout 10 /bin/gfactor 42 -- a REAL Apple "
		    "binary fork+exec+waiting another <<<\n");
		proc_tp = MACH_PORT_NULL;
		proc_argv[0] = "gtimeout";
		proc_argv[1] = "10";
		proc_argv[2] = "/bin/gfactor";
		proc_argv[3] = "42";
		proc_argv[4] = NULL;
		child_id = spawn_args("gtimeout", 4, proc_argv, &proc_tp);
		if (child_id < 0) {
			printf("  spawn_args('gtimeout') failed (rv=%ld)\n",
			    child_id);
			return (99);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  gtimeout retired after %d turns\n", i);
	}

	/*
	 * dash, a real Apple POSIX shell: a builtin; a pipeline (dash forks
	 * both sides, a kernel pipe feeds gfactor); a redirect to disk; a
	 * script file (PATH lookups against the synthetic /bin, open(2),
	 * fcntl(F_DUPFD), a command substitution); and an interactive
	 * session over a console feed.
	 */
	{
		mach_port_name_t	 sh_tp;
		char			*sh_argv[4];

		printf("  >>> dash -c 'echo ...' -- a REAL Apple shell, "
		    "builtin only <<<\n");
		sh_tp = MACH_PORT_NULL;
		sh_argv[0] = "dash";
		sh_argv[1] = "-c";
		sh_argv[2] = "echo hello from a real shell on style9";
		sh_argv[3] = NULL;
		child_id = spawn_args("dash", 3, sh_argv, &sh_tp);
		if (child_id < 0) {
			printf("  spawn_args('dash') failed (rv=%ld)\n",
			    child_id);
			return (100);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  dash[builtin] retired after %d turns\n", i);

		printf("  >>> dash -c 'echo 600851475143 | gfactor' -- a "
		    "shell pipeline <<<\n");
		sh_tp = MACH_PORT_NULL;
		sh_argv[2] = "echo 600851475143 | gfactor";
		child_id = spawn_args("dash", 3, sh_argv, &sh_tp);
		if (child_id < 0) {
			printf("  spawn_args('dash|') failed (rv=%ld)\n",
			    child_id);
			return (101);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  dash[pipeline] retired after %d turns\n", i);

		/*
		 * Redirection: dash opens the file O_WRONLY|O_CREAT|O_TRUNC
		 * and dup2s it onto fd 1, so its builtins write to the volume;
		 * gcat reads it back.  The file is left behind on purpose: the
		 * next boot's apfs-shell check (fs/fs.c) finds it, proving the
		 * bytes reached the disk and not a cache.
		 */
		printf("  >>> dash -c 'echo ... > /etc/notes.txt' -- a REAL "
		    "Apple shell WRITING TO THE VOLUME <<<\n");
		sh_tp = MACH_PORT_NULL;
		sh_argv[2] =
		    "echo a line from a real Apple shell > /etc/notes.txt; "
		    "echo appended by the same shell >> /etc/notes.txt; "
		    "gcat /etc/notes.txt";
		child_id = spawn_args("dash", 3, sh_argv, &sh_tp);
		if (child_id < 0) {
			printf("  spawn_args('dash>') failed (rv=%ld)\n",
			    child_id);
			return (103);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  dash[redirect] retired after %d turns\n", i);

		printf("  >>> dash /bin/demo.sh -- a shell SCRIPT from the "
		    "synthetic /bin <<<\n");
		sh_tp = MACH_PORT_NULL;
		sh_argv[1] = "/bin/demo.sh";
		sh_argv[2] = NULL;
		child_id = spawn_args("dash", 2, sh_argv, &sh_tp);
		if (child_id < 0) {
			printf("  spawn_args('dash script') failed "
			    "(rv=%ld)\n", child_id);
			return (102);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  dash[script] retired after %d turns\n", i);

		printf("  >>> dash -i  -- a REAL Apple shell, INTERACTIVE "
		    "over a scripted console feed <<<\n");
		{
			static const char	feed[] =
			    "echo interactive dash is alive on style9\n"
			    "gfactor 42\n"
			    "echo \"captured: $(echo 600851475143 | gfactor)\"\n"
			    "exit\n";

			(void)cons_feed(feed, sizeof(feed) - 1);
		}
		sh_tp = MACH_PORT_NULL;
		sh_argv[0] = "dash";
		sh_argv[1] = "-i";
		sh_argv[2] = NULL;
		child_id = spawn_args("dash", 2, sh_argv, &sh_tp);
		if (child_id < 0) {
			printf("  spawn_args('dash -i') failed (rv=%ld)\n",
			    child_id);
			return (103);
		}
		for (i = 0; i < 8192 && task_alive((uint64_t)child_id); i++)
			(void)poll_turn();
		printf("  dash[interactive] retired after %d turns\n", i);
	}
	return (0);
}

/*
 * Bootstrap publish: register a port under a name, look the name up (a
 * fresh send name for the same port), send a tag through it and receive
 * it on the original name; then deregister and check the lookup fails.
 * Ring-3 publish is what lets a user task serve peers the way the
 * kernel services in mach/services.c do.
 */
static int
demo_bootstrap_publish(void)
{
	struct mach_msg_header	tx;
	struct mach_msg_header	rx;
	mach_port_name_t	svc;
	mach_port_name_t	cli;
	int			rv;

	svc = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (svc == MACH_PORT_NULL) {
		printf("  port_allocate(svc) failed\n");
		return (56);
	}

	rv = bootstrap_register_service(PUB_DEMO_NAME, svc);
	if (rv != MACH_MSG_OK) {
		printf("  bootstrap_register_service failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(svc);
		return (57);
	}
	printf("  registered '%s' -> svc=0x%x\n",
	    PUB_DEMO_NAME, (unsigned)svc);

	cli = bootstrap_lookup(PUB_DEMO_NAME);
	if (cli == MACH_PORT_NULL) {
		printf("  lookup of just-registered '%s' failed\n",
		    PUB_DEMO_NAME);
		(void)bootstrap_deregister_service(PUB_DEMO_NAME);
		(void)mach_port_deallocate(svc);
		return (58);
	}
	if (cli == svc) {
		printf("  lookup returned same name as register (got 0x%x)\n",
		    (unsigned)cli);
		(void)mach_port_deallocate(cli);
		(void)bootstrap_deregister_service(PUB_DEMO_NAME);
		(void)mach_port_deallocate(svc);
		return (59);
	}
	printf("  lookup '%s' -> cli=0x%x (distinct from svc)\n",
	    PUB_DEMO_NAME, (unsigned)cli);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = cli;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = PUB_DEMO_TAG;
	rv = mach_msg_send(&tx);
	if (rv != MACH_MSG_OK) {
		printf("  send via cli failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(cli);
		(void)bootstrap_deregister_service(PUB_DEMO_NAME);
		(void)mach_port_deallocate(svc);
		return (60);
	}

	rv = mach_msg_recv_timed(svc, &rx, sizeof(rx), 1000);
	if (rv != MACH_MSG_OK) {
		printf("  recv on svc failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(cli);
		(void)bootstrap_deregister_service(PUB_DEMO_NAME);
		(void)mach_port_deallocate(svc);
		return (61);
	}
	if (rx.msgh_id != PUB_DEMO_TAG) {
		printf("  publish tag mismatch: got 0x%x expected 0x%x\n",
		    (unsigned)rx.msgh_id, (unsigned)PUB_DEMO_TAG);
		(void)mach_port_deallocate(cli);
		(void)bootstrap_deregister_service(PUB_DEMO_NAME);
		(void)mach_port_deallocate(svc);
		return (62);
	}
	printf("  publish round-trip: tag=0x%x via registered name OK\n",
	    (unsigned)rx.msgh_id);

	rv = bootstrap_deregister_service(PUB_DEMO_NAME);
	if (rv != MACH_MSG_OK) {
		printf("  bootstrap_deregister_service failed (rv=%d)\n", rv);
		(void)mach_port_deallocate(cli);
		(void)mach_port_deallocate(svc);
		return (63);
	}

	/*
	 * A fresh lookup must miss now, though our `cli' name still holds
	 * its send right.
	 */
	if (bootstrap_lookup(PUB_DEMO_NAME) != MACH_PORT_NULL) {
		printf("  '%s' still resolves after deregister\n",
		    PUB_DEMO_NAME);
		(void)mach_port_deallocate(cli);
		(void)mach_port_deallocate(svc);
		return (64);
	}
	printf("  deregister + post-lookup miss: OK\n");

	(void)mach_port_deallocate(cli);
	(void)mach_port_deallocate(svc);
	return (0);
}

/*
 * Are we a spawn_with_port child?  A tagged send to MACH_PORT_PARENT
 * (slot 3) succeeds only if the parent put a send right there;
 * MACH_E_RIGHT means we are the boot-time hello.elf.
 */
static int
hello_try_parent_ping(void)
{
	struct mach_msg_header	ping;
	int			rv;

	ping.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	ping.msgh_size    = sizeof(ping);
	ping.msgh_remote  = MACH_PORT_PARENT;
	ping.msgh_local   = MACH_PORT_NULL;
	ping.msgh_voucher = 0;
	ping.msgh_id      = HELLO_CHILD_PING_TAG;
	rv = mach_msg_send(&ping);
	return (rv == MACH_MSG_OK);
}

int
main(void)
{
	int	rv;

	/* A child has sent its ping and is done. */
	if (hello_try_parent_ping())
		return (0);

	printf("hello from hello.elf (libstyle9, ring 3)\n");

	rv = demo_round_trip();
	if (rv != 0)
		return (rv);

	rv = demo_task_self();
	if (rv != 0)
		return (rv);

	rv = demo_bootstrap_chain();
	if (rv != 0)
		return (rv);

	rv = demo_ool_roundtrip();
	if (rv != 0)
		return (rv);

	rv = demo_vm_allocate();
	if (rv != 0)
		return (rv);

	rv = demo_ool_deallocate();
	if (rv != 0)
		return (rv);

	rv = demo_no_senders();
	if (rv != 0)
		return (rv);

	rv = demo_port_set();
	if (rv != 0)
		return (rv);

	rv = demo_dead_name();
	if (rv != 0)
		return (rv);

	rv = demo_spawn_inject();
	if (rv != 0)
		return (rv);

	rv = demo_ool_from_child();
	if (rv != 0)
		return (rv);

	rv = demo_exception();
	if (rv != 0)
		return (rv);

	rv = demo_exception_per_type();
	if (rv != 0)
		return (rv);

	rv = demo_exception_thread_level();
	if (rv != 0)
		return (rv);

	rv = demo_exception_resume();
	if (rv != 0)
		return (rv);

	rv = demo_bootstrap_publish();
	if (rv != 0)
		return (rv);

	rv = demo_msg_strictness();
	if (rv != 0)
		return (rv);

	rv = demo_lsmp_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_vmmap_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_launchctl_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_selfkill_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_compute_kill_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_parent_managed_kill();
	if (rv != 0)
		return (rv);

	rv = demo_top_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_stale_taskport();
	if (rv != 0)
		return (rv);

	rv = demo_spawn_argv();
	if (rv != 0)
		return (rv);

	rv = demo_macho_spawn();
	if (rv != 0)
		return (rv);

	rv = demo_darwin_spawn();
	if (rv != 0)
		return (rv);

	printf("hello.elf: all demos passed\n");
	return (0);
}
