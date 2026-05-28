/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * launchctl -- userspace CLI for the in-kernel launchd analog.
 *
 * v1 is a scripted demo (no argv passing across spawn yet): runs a
 * scripted load / list / use-the-service / unload cycle against the
 * `launchd` bootstrap service, exercising every wire op.  Maps 1:1
 * onto Darwin's `launchctl` subcommands:
 *
 *	list                       LAUNCHCTL_OP_LIST
 *	load /path/foo.plist       LAUNCHCTL_OP_LOAD  (name + program)
 *	unload /path/foo.plist     LAUNCHCTL_OP_UNLOAD
 *
 * The middle of the demo also bootstraps_lookup the loaded service's
 * own port ("echo") and exercises it with an RPC so the LIST output
 * reflects a service that's not just been spawned but is actually
 * doing work.
 */

#include "style9.h"

#define	DEMO_LABEL	"com.style9.echod"
#define	DEMO_PROGRAM	"echod"
#define	ECHO_RPC_ROUNDS	3u

/* ---- helpers --------------------------------------------------------- */

static mach_port_name_t
launchd_lookup(void)
{

	return (bootstrap_lookup(SVC_LAUNCHD_NAME));
}

static const char *
state_name(uint32_t s)
{

	switch (s) {
	case LAUNCHD_STATE_RUNNING: return ("running");
	case LAUNCHD_STATE_EXITED:  return ("exited");
	case LAUNCHD_STATE_FAILED:  return ("failed");
	default:                    return ("?");
	}
}

/* ---- ops ------------------------------------------------------------- */

/*
 * RPC LAUNCHCTL_OP_LIST.  Allocates the full list-reply buffer on
 * the stack (520 bytes) so caller does not have to know how big a
 * list-reply gets.
 */
static int
do_list(mach_port_name_t launchd, const char *banner)
{
	struct mach_msg_header	req;
	struct {
		struct mach_msg_header		hdr;
		struct svc_launchctl_list_reply	body;
	} reply;
	uint32_t		i;
	int			rv;

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = launchd;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = LAUNCHCTL_OP_LIST;

	rv = mach_msg_rpc(&req, &reply.hdr, sizeof(reply), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  launchctl list rpc rv=%d\n", rv);
		return (rv);
	}
	if (reply.body.ll_count > LAUNCHD_MAX_SERVICES)
		reply.body.ll_count = LAUNCHD_MAX_SERVICES;

	printf("  %s -- %u service%s loaded:\n",
	    banner, reply.body.ll_count,
	    reply.body.ll_count == 1 ? "" : "s");
	printf("    %-24s %-12s %-12s %s\n",
	    "label", "program", "state", "task_id");
	if (reply.body.ll_count == 0)
		printf("    (none)\n");
	for (i = 0; i < reply.body.ll_count; i++) {
		struct svc_launchctl_entry *e = &reply.body.ll_entries[i];

		printf("    %-24s %-12s %-12s %llu\n",
		    e->le_name, e->le_program, state_name(e->le_state),
		    (unsigned long long)e->le_task_id);
	}
	return (MACH_MSG_OK);
}

/*
 * Build a LAUNCHCTL_OP_LOAD message in `buf` with inline-body
 * carrying a svc_launchctl_load_req populated from (label, program).
 * Caller fills msgh_remote.
 */
static void
build_load_req(uint8_t *buf, mach_port_name_t dest, const char *label,
    const char *program)
{
	struct mach_msg_header		*hdr;
	struct svc_launchctl_load_req	*body;
	size_t				 i;

	hdr  = (struct mach_msg_header *)buf;
	body = (struct svc_launchctl_load_req *)
	    (buf + sizeof(struct mach_msg_header));

	hdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	hdr->msgh_size    = sizeof(*hdr) + sizeof(*body);
	hdr->msgh_remote  = dest;
	hdr->msgh_local   = MACH_PORT_NULL;
	hdr->msgh_voucher = 0;
	hdr->msgh_id      = LAUNCHCTL_OP_LOAD;

	for (i = 0; i < LAUNCHD_NAME_MAX; i++)
		body->lr_name[i] = 0;
	for (i = 0; i < LAUNCHD_NAME_MAX && label[i] != '\0'; i++)
		body->lr_name[i] = label[i];

	for (i = 0; i < LAUNCHD_PROGRAM_MAX; i++)
		body->lr_program[i] = 0;
	for (i = 0; i < LAUNCHD_PROGRAM_MAX && program[i] != '\0'; i++)
		body->lr_program[i] = program[i];
}

static void
build_unload_req(uint8_t *buf, mach_port_name_t dest, const char *label)
{
	struct mach_msg_header		*hdr;
	struct svc_launchctl_byname_req	*body;
	size_t				 i;

	hdr  = (struct mach_msg_header *)buf;
	body = (struct svc_launchctl_byname_req *)
	    (buf + sizeof(struct mach_msg_header));

	hdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	hdr->msgh_size    = sizeof(*hdr) + sizeof(*body);
	hdr->msgh_remote  = dest;
	hdr->msgh_local   = MACH_PORT_NULL;
	hdr->msgh_voucher = 0;
	hdr->msgh_id      = LAUNCHCTL_OP_UNLOAD;

	for (i = 0; i < LAUNCHD_NAME_MAX; i++)
		body->lr_name[i] = 0;
	for (i = 0; i < LAUNCHD_NAME_MAX && label[i] != '\0'; i++)
		body->lr_name[i] = label[i];
}

static int
do_load(mach_port_name_t launchd, const char *label, const char *program)
{
	uint8_t		req_buf[sizeof(struct mach_msg_header) +
			    sizeof(struct svc_launchctl_load_req)];
	struct {
		struct mach_msg_header			hdr;
		struct svc_launchctl_status_reply	body;
	} reply;
	int		rv;

	build_load_req(req_buf, launchd, label, program);

	rv = mach_msg_rpc((struct mach_msg_header *)req_buf,
	    &reply.hdr, sizeof(reply), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  load rpc rv=%d\n", rv);
		return (rv);
	}
	if (reply.body.ls_status != MACH_MSG_OK) {
		printf("  load '%s' -> failed (status=%d, state=%s)\n",
		    label, reply.body.ls_status,
		    state_name(reply.body.ls_state));
		return (reply.body.ls_status);
	}
	printf("  load '%s' (program=%s) -> %s task_id=%llu\n",
	    label, program, state_name(reply.body.ls_state),
	    (unsigned long long)reply.body.ls_task_id);
	return (MACH_MSG_OK);
}

static int
do_unload(mach_port_name_t launchd, const char *label)
{
	uint8_t		req_buf[sizeof(struct mach_msg_header) +
			    sizeof(struct svc_launchctl_byname_req)];
	struct {
		struct mach_msg_header			hdr;
		struct svc_launchctl_status_reply	body;
	} reply;
	int		rv;

	build_unload_req(req_buf, launchd, label);

	rv = mach_msg_rpc((struct mach_msg_header *)req_buf,
	    &reply.hdr, sizeof(reply), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  unload rpc rv=%d\n", rv);
		return (rv);
	}
	if (reply.body.ls_status != MACH_MSG_OK) {
		printf("  unload '%s' -> status=%d\n", label,
		    reply.body.ls_status);
		return (reply.body.ls_status);
	}
	printf("  unload '%s' -> ok (was %s, task_id=%llu)\n",
	    label, state_name(reply.body.ls_state),
	    (unsigned long long)reply.body.ls_task_id);
	return (MACH_MSG_OK);
}

/*
 * Exercise the now-running echod service via the regular bootstrap-
 * lookup + RPC path.  Each round drives one recv-in-echod's-loop
 * iteration, so by the time we finish (ECHO_RPC_ROUNDS), echod has
 * served that many rounds toward its self-imposed exit limit.
 */
static void
poke_echo(uint32_t rounds)
{
	struct mach_msg_header	req;
	struct mach_msg_header	reply;
	mach_port_name_t	svc;
	uint32_t		i;
	int			rv;

	svc = bootstrap_lookup("echo");
	if (svc == MACH_PORT_NULL) {
		printf("  bootstrap_lookup('echo') failed -- skipping pokes\n");
		return;
	}
	for (i = 0; i < rounds; i++) {
		req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		req.msgh_size    = sizeof(req);
		req.msgh_remote  = svc;
		req.msgh_local   = MACH_PORT_NULL;
		req.msgh_voucher = 0;
		req.msgh_id      = 0xE0E0u + i;

		rv = mach_msg_rpc(&req, &reply, sizeof(reply), 2000);
		if (rv != MACH_MSG_OK) {
			printf("  echo round %u rpc rv=%d\n", i, rv);
			break;
		}
		if (reply.msgh_id != req.msgh_id) {
			printf("  echo round %u tag mismatch "
			    "(sent 0x%x got 0x%x)\n",
			    i, (unsigned)req.msgh_id,
			    (unsigned)reply.msgh_id);
			break;
		}
	}
	printf("  poke 'echo' x %u rounds: ok\n", i);
	(void)mach_port_deallocate(svc);
}

int
main(void)
{
	mach_port_name_t	launchd;
	int			i;

	launchd = launchd_lookup();
	if (launchd == MACH_PORT_NULL) {
		printf("launchctl: bootstrap_lookup('%s') failed\n",
		    SVC_LAUNCHD_NAME);
		return (1);
	}
	printf("launchctl: connected to '%s' (port=0x%x)\n",
	    SVC_LAUNCHD_NAME, (unsigned)launchd);

	if (do_list(launchd, "initial") != MACH_MSG_OK)
		return (2);

	if (do_load(launchd, DEMO_LABEL, DEMO_PROGRAM) != MACH_MSG_OK)
		return (3);

	if (do_list(launchd, "after-load") != MACH_MSG_OK)
		return (4);

	/*
	 * Give echod a few yields to actually start serving before we
	 * start poking it.  Without this the lookup race can see the
	 * service registered but the daemon not yet in mach_msg_recv.
	 */
	for (i = 0; i < 16; i++)
		(void)yield();

	poke_echo(ECHO_RPC_ROUNDS);

	if (do_list(launchd, "after-poke") != MACH_MSG_OK)
		return (5);

	if (do_unload(launchd, DEMO_LABEL) != MACH_MSG_OK)
		return (6);

	if (do_list(launchd, "after-unload") != MACH_MSG_OK)
		return (7);

	(void)mach_port_deallocate(launchd);
	return (0);
}
