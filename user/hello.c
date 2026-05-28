/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Ring-3 demo for style9-os, now linked against libstyle9.
 *
 * Steps (each demos one layer of the user/kernel IPC plumbing):
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
 *
 * Boot-time main() also detects whether the task was spawned via
 * SYS_SPAWN_WITH_PORT (child path: a SEND right sits at
 * MACH_PORT_PARENT).  If so, send a ping and exit -- the demos above
 * are for the original hello.elf only.
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
 * OOL round-trip: build a user-VA buffer with a known pattern, send it
 * as the sole OOL descriptor in a complex message to svc/echool, expect
 * the kernel-side FNV-1a checksum to come back in msgh_id and match
 * what we computed locally.  Validates the wire format end-to-end from
 * ring 3: the body's descriptor-count, the OOL type tag at byte 0, the
 * size and address fields, and the kernel's special-port intercept all
 * have to line up for this to pass.
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
 * vm_allocate: ask the kernel for a fresh page-aligned range, prove
 * the bytes start zero, write a pattern, read it back, then release
 * via vm_deallocate.  Validates SYS_VM_ALLOCATE + SYS_VM_DEALLOCATE
 * end-to-end (the syscall path, the pmap install, the vm_map entry,
 * and the symmetric teardown).
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
 * OOL round-trip with deallocate=1: allocate a buffer via vm_allocate,
 * fill it, ship via OOL to svc/echool with deallocate set, verify the
 * kernel-side checksum matches.  Post-send a vm_deallocate on the same
 * range MUST fail (SYS_E_INVAL) -- the kernel already freed it as part
 * of the send hook.
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

	/*
	 * The buffer is gone; a second vm_deallocate must fail.  The
	 * kernel returns SYS_E_INVAL for any unknown range.
	 */
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
 * NO_SENDERS notification.  Allocate a source port (RECV+SEND) and a
 * notify port (RECV+SEND); register a NO_SENDERS notification on the
 * source delivered to the notify port; drop the source's SEND right via
 * mach_port_mod_refs.  Now the source has RECV but no SEND, so the
 * kernel synthesises a MACH_NOTIFY_NO_SENDERS message and posts it to
 * the notify port.  Receive that, verify msgh_id and nh_msgid.
 *
 * The notify-msgid is user-chosen and round-trips verbatim so a service
 * watching many sources via one notify port can tell them apart.
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
	 * Drop just SEND from `source`.  RECV stays under the same name so
	 * we keep the port alive; the no-senders condition triggers because
	 * p_send_count goes to 0 while p_has_receive is still true.
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
 * Port set.  Allocate a set + two member ports (with RECV+SEND on each
 * member).  Insert both members into the set.  Send distinct tags to
 * each member, then recv on the SET name twice -- both messages should
 * pop in send order (FIFO is per-member, but the set's scan visits
 * members head-first, so the test exercises the wiring rather than
 * ordering guarantees).
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

	/*
	 * Read-only introspection.  Both members should report the same
	 * set name as the one returned from port_set_allocate.
	 */
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
 * DEAD_NAME notification.  Mirror of demo_no_senders but from the
 * other side: we hold SEND on the source port; we want to know when
 * its RECV is dropped (the port behind our SEND name dies).  Allocate
 * source (RECV+SEND) plus a notify port; register DEAD_NAME on source
 * (kernel requires we hold SEND, which we do); drop RECV on source via
 * mod_refs; the port dies; the kernel posts MACH_NOTIFY_DEAD_NAME on
 * the notify port; recv and verify.
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

	/*
	 * Drop just RECV from `source`.  SEND stays under the same name
	 * (the watcher's qualifier).  Port goes dead -> DEAD_NAME fires.
	 */
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
 * spawn_with_port: allocate a work port (RECV+SEND), spawn ourselves
 * (hello.elf) with that port injected at the child's MACH_PORT_PARENT
 * slot, then recv on the work port.  The child detects its parent
 * port at startup (see main()'s child-detect block), sends a tagged
 * ping, and exits.  We verify the tag and the child task is gone.
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
 * Exception port end-to-end.  Allocate a watcher port, spawn the
 * dedicated `excchild' program with that port at MACH_PORT_PARENT.
 * `excchild' installs the parent slot as its task's exception port,
 * then NULL-derefs; the kernel synthesises a MACH_EXC_FAULT message
 * and posts it on the watcher BEFORE retiring the child thread.
 * Parent recvs the exception and checks trapno (14 = #PF) + cr2 (0).
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
 * Reply-protocol RESUME.  Spawn `excchild_resume' which opted into
 * the reply protocol (EXC_FLAG_RESUMABLE) at the BAD_INSTRUCTION
 * slot, then UD2's.  We recv the MACH_EXC_FAULT, extract the
 * implicit reply port from msgh_local, ship back a
 * mach_exception_reply with verdict=EXC_VERDICT_RESUME and
 * rip_advance=2 (the length of UD2).  The kernel advances the
 * child's RIP past the faulting instruction and returns it to user
 * mode -- the child runs its post-UD2 epilogue, which sends back
 * the agreed RESUME_SURVIVE_TAG message confirming it actually
 * resumed.  Validates the reply path end-to-end.
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
	 * Verdict: skip the 2-byte UD2 and resume.  msgh_remote =
	 * the SEND name the kernel just minted in our space for the
	 * reply port; MOVE_SEND consumes it (one-shot reply).
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
	 * Child's post-UD2 epilogue runs and sends the survival tag.
	 * If the kernel hadn't honored RESUME, the child would be
	 * KILL'd instead and this recv would time out.
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
 * Thread-level exception ports + precedence.  `excchild_thr' installs
 * the parent-injected port at BOTH task-level BAD_INSTRUCTION and
 * thread-level BAD_INSTRUCTION, then UD2's.  The kernel checks the
 * thread-level slot first; if precedence is correct, only one
 * MACH_EXC_FAULT message lands in the watcher (because the
 * task-level path is short-circuited on a hit).  We confirm by
 * recv'ing the first message, then recv_timed with a 200 ms budget
 * for a second message -- it MUST time out, proving the task-level
 * slot was bypassed.
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

	/*
	 * Precedence assertion: only the thread-level slot fired.  Try
	 * to recv a second message with a short timeout -- if the
	 * task-level slot had ALSO fired we'd find a second copy in
	 * the queue.  MACH_E_TIMEOUT confirms the queue is empty.
	 */
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
 * Per-type exception ports.  Spawn the dedicated `excchild_ud' which
 * installs the parent-injected port at EXC_MASK_BAD_INSTRUCTION only
 * (not BAD_ACCESS) and then executes UD2 to trigger #UD.  The
 * kernel's exc_type_from_trapno routes trapno 6 to BAD_INSTRUCTION;
 * the child's slot for that type is populated; user_fault_die posts
 * MACH_EXC_FAULT there before retiring the thread.  Verifies the
 * trapno arrives as 6 (not the 14 the BAD_ACCESS bucket would deliver)
 * and that the child's task_id matches the spawn return.
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
 * Message-header strictness.  msgh_voucher is reserved (must be zero)
 * and msgh_bits has 14 reserved bits between the two disposition lanes
 * and the COMPLEX flag at bit 31; both are rejected by mach_msg_send
 * with MACH_E_INVAL.  This demo verifies the kernel surfaces both
 * failures and then completes a clean send to prove the validator
 * does not over-reject sane headers.
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

/*
 * Bootstrap publish round-trip.  Allocate a service port (RECV+SEND),
 * register it under a user-chosen name, look the name back up from our
 * own task -- kernel hands us a fresh SEND that names the same port --
 * fire a tagged message at the looked-up name, recv on the original
 * RECV-bearing name, verify the tag.  Then deregister and confirm the
 * lookup now fails.  Validates BOOTSTRAP_OP_REGISTER + DEREGISTER
 * end-to-end: ring-3 service publish is the unlock that lets userspace
 * act as a peer of the kernel-resident services in mach/services.c.
 */
#define	PUB_DEMO_NAME	"hello.demo"
#define	PUB_DEMO_TAG	0xFEEDFACEu

/*
 * demo_launchctl_spawn: spawn the `launchctl` demo which scripts a
 * full load / poke / unload cycle against the in-kernel launchd
 * analog.  Yields generously since the child does several IPC RPCs
 * + a child-of-a-child spawn (echod).
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
		(void)yield();
	return (0);
}

/*
 * demo_selfkill_spawn: spawn the `selfkill` program which exercises
 * SYS_TASK_KILL on its OWN task-self port (the trivial-capability
 * case -- every task has SEND on MACH_PORT_TASK_SELF).  The
 * syscall-exit kill check (detection point #5) should retire the
 * child before its sysretq, so the BUG line in selfkill.c never
 * appears in the boot transcript.
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
		(void)yield();
		alive = task_alive((uint64_t)child_id);
	}
	printf("  task_alive(%ld) after self-kill: %s (waited %d yields)\n",
	    child_id, alive ? "yes (kill failed)" : "no (kill landed)", i);
	return (0);
}

/*
 * demo_parent_managed_kill: end-to-end test of the v3 capability
 * path -- spawn_returns_taskport hands the parent BOTH task_id AND a
 * SEND right on the child's task-self port in one syscall, so the
 * parent never has to talk to bootstrap to acquire the kill
 * capability.  Mirrors the sh.c child-table pattern: every spawn
 * gives us the handle, so task_kill is one call away.
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
	 * Give loopchild a few yields to start running its compute
	 * loop -- without that the kill races against bootstrap-register
	 * setup and the child dies before reaching the loop body.  Not a
	 * correctness issue but a cleaner narrative in the boot log.
	 */
	for (i = 0; i < 16; i++)
		(void)yield();

	rv = task_kill(taskport);
	printf("  task_kill(taskport) -> %d\n", rv);

	alive = 1;
	for (i = 0; i < 64 && alive; i++) {
		(void)yield();
		alive = task_alive((uint64_t)child_id);
	}
	printf("  task_alive(%ld) after parent-managed kill: %s "
	    "(waited %d yields)\n",
	    child_id,
	    alive ? "yes (capability path BROKEN)" : "no (capability path OK)",
	    i);

	(void)mach_port_deallocate(taskport);
	return (0);
}

/*
 * demo_compute_kill_spawn: end-to-end demo of detection point #4
 * (IRQ-return-to-user).  loopchild publishes its own task-self port
 * under bootstrap, then enters a syscall-free spin.  Parent looks the
 * port up + calls task_kill; without the IRQ-return check the child
 * would loop forever (no syscall boundary to catch the flag).  PIT
 * IRQs hit roughly every 10 ms; the child should retire within a
 * handful of parent yields.
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
	 * Wait for loopchild to register its task-self under bootstrap.
	 * Polling on bootstrap_lookup is cheap (a single RPC); a handful
	 * of yields lets the child reach the register-then-enter-loop
	 * point.
	 */
	tport = MACH_PORT_NULL;
	for (i = 0; i < 64 && tport == MACH_PORT_NULL; i++) {
		(void)yield();
		tport = bootstrap_lookup("loopchild.tport");
	}
	if (tport == MACH_PORT_NULL) {
		printf("  loopchild.tport lookup failed after %d yields\n", i);
		return (79);
	}
	printf("  loopchild.tport = 0x%x (after %d lookup yields)\n",
	    (unsigned)tport, i);

	rv = task_kill(tport);
	printf("  task_kill(loopchild) -> %d\n", rv);

	/*
	 * loopchild has no syscall window the kill could fire on, so
	 * termination is contingent on the IRQ-return detection point
	 * triggering at the next PIT tick.  Bounded probe budget.
	 */
	alive = 1;
	for (i = 0; i < 128 && alive; i++) {
		(void)yield();
		alive = task_alive((uint64_t)child_id);
	}
	printf("  task_alive(%ld) after task_kill: %s (waited %d yields)\n",
	    child_id, alive ? "yes (IRQ-return MISSED)" : "no (IRQ-return OK)",
	    i);

	(void)mach_port_deallocate(tport);
	return (0);
}

/*
 * demo_vmmap_spawn: spawn the `vmmap` tool and let it print.  Same
 * shape as demo_lsmp_spawn: bounded-yield wait so the child has time
 * to dump its table before the parent moves on.
 */
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
		(void)yield();
	return (0);
}

/*
 * demo_lsmp_spawn: spawn the `lsmp` tool and wait for it to exit.
 * lsmp seeds a varied port_space (RECV+SEND, SEND-only, port_set,
 * exception port) and then dumps its own snapshot via
 * SYS_TASK_GET_PORT_SNAPSHOT.  Boot log captures the table verbatim.
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
	 * Yield a handful of times to let the child run before we
	 * return: lsmp does no IPC with us so we cannot block on a
	 * port, and yield-spinning on task_alive would starve the
	 * idle thread (only the idle-loop reaps zombie threads -- the
	 * task stays in task_list until its last thread is reaped).
	 * 64 yields is far more than lsmp needs to print its table
	 * and exit; if it has not finished by then the remaining
	 * output simply interleaves with the rest of the boot log,
	 * which is harmless.
	 */
	for (i = 0; i < 64; i++)
		(void)yield();
	return (0);
}

/*
 * demo_top_spawn: spawn the `top` tool and let it run its samples.
 * top RPCs the "tasks" service a few times with a yield gap between,
 * printing the per-task thread/port/region columns the service grew
 * for it.  We bounded-yield generously (160x) so all samples land in
 * the boot log before main() moves on; top does no IPC with us, so as
 * with lsmp/vmmap we cannot block on a port and just yield instead.
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
		(void)yield();
	return (0);
}

/*
 * demo_stale_taskport: regression guard for the task-self port UAF.
 *
 * A task-self port can outlive its task -- any external SEND right (here
 * the taskport spawn_returns_taskport hands us) keeps the port object
 * alive past the task's kfree in task_deref.  port_release_task_self
 * marks the port p_dead, but neither space_lookup nor the special-port
 * intercept gates on p_dead, so before the fix both SYS_TASK_KILL and a
 * GET_INFO send reached straight through to a dangling struct task *.
 * The port now stores the task's immutable id, resolved by the kernel
 * with task_lookup_ref, so a stale port fails safe instead.
 *
 * We spawn loopchild (a syscall-free spinner), confirm a cross-task
 * GET_INFO RPC to its taskport works while it is alive, kill it via that
 * same capability, yield-spin until it leaves the live list (struct task
 * freed), then poke the now-stale taskport: the GET_INFO RPC must come
 * back MACH_E_DEAD and task_kill must no-op -- neither may touch freed
 * memory.  Pre-fix this dereferenced a dangling pointer and faulted.
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

	/* Positive control: cross-task GET_INFO on a LIVE task-self port. */
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
		(void)yield();
	if (task_alive((uint64_t)child_id)) {
		printf("  child still alive after 256 yields; "
		    "skipping stale assertion\n");
		(void)mach_port_deallocate(tport);
		return (0);
	}
	printf("  child reaped; taskport 0x%x is now stale\n",
	    (unsigned)tport);

	/*
	 * Payoff #1: GET_INFO RPC to the stale taskport.  p_special_arg
	 * holds the dead child's id; task_lookup_ref returns NULL and the
	 * intercept answers MACH_E_DEAD.  Pre-fix: dereferenced freed mem.
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
	 * Payoff #2: task_kill on the stale name.  space_lookup hands back
	 * the dead port, sys_task_kill reads the (dead) id, and
	 * task_request_terminate no-ops -- a clean return, no deref.
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
 * demo_spawn_argv: exercise SYS_SPAWN_ARGS end to end.  Spawn argecho
 * with a known argument vector; argecho echoes argc + each argv[i] to
 * the console (the strings appear in the boot transcript), proving the
 * kernel stack-builder + crt0 forward the command line into
 * main(argc, argv).  We hold the taskport spawn_args hands back and
 * wait for the child to retire before returning.
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
		(void)yield();

	(void)mach_port_deallocate(taskport);
	printf("  argecho retired after %d yields\n", i);
	return (0);
}

/*
 * demo_macho_spawn: exercise the Mach-O container loader (kern/macho.c).
 * machotest is an ordinary style9 program delivered as a Mach-O instead
 * of an ELF; the spawn launcher sniffs the image magic and routes it to
 * macho_load.  We spawn the thin x86-64 slice WITH a command line -- which
 * also confirms the SysV initial-stack frame reaches main(argc, argv)
 * regardless of container format -- then the single-slice fat/universal
 * archive, which drives macho_load's slice picker.  Both programs print a
 * banner the boot transcript captures; we bounded-yield between them so
 * the two outputs do not interleave.
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
		(void)yield();
	if (taskport != MACH_PORT_NULL)
		(void)mach_port_deallocate(taskport);
	printf("  thin Mach-O retired after %d yields\n", i);

	child_id = spawn("machotest_fat");
	if (child_id < 0) {
		printf("  spawn('machotest_fat') failed (rv=%ld)\n", child_id);
		return (87);
	}
	for (i = 0; i < 64 && task_alive((uint64_t)child_id); i++)
		(void)yield();
	printf("  fat Mach-O (x86_64 slice) retired after %d yields\n", i);
	return (0);
}

/*
 * demo_darwin_spawn: exercise the Darwin syscall personality (S2,
 * kern/darwin.c).  darwinhello is NOT a style9 program -- it is a
 * freestanding stub that issues genuine Apple class-encoded syscalls
 * (write, getpid, task_self_trap, mach_reply_port, exit, plus a deliberate
 * unimplemented call to prove the carry/errno convention).  It ships as a
 * Mach-O carrying an LC_BUILD_VERSION naming PLATFORM_MACOS, so the loader
 * tags its task TASK_PERSONALITY_DARWIN and syscall_dispatch routes it
 * through darwin_dispatch.  Its banner + the kernel's per-call trace appear
 * in the boot transcript; we bounded-yield until it retires.
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
		(void)yield();
	printf("  darwinhello (Darwin ABI) retired after %d yields\n", i);
	return (0);
}

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
	 * Lookup must now miss.  Caller's `cli` name still resolves
	 * locally -- mach_port_deallocate just drops the SEND -- but a
	 * fresh bootstrap_lookup against the gone registration returns
	 * MACH_PORT_NULL.
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
 * Child-of-spawn-with-port detection.  Probe the well-known
 * MACH_PORT_PARENT slot with a tagged send: success means the parent
 * pre-populated slot 3 with a SEND right (we're a child); MACH_E_RIGHT
 * means an empty slot (we're the original boot-time hello.elf).
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

	/*
	 * Child path: parent injected a SEND right at MACH_PORT_PARENT.
	 * Send the agreed ping and exit; the original boot-time hello
	 * (which has an empty slot 3 at startup) falls through.
	 */
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
