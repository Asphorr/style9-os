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
#define	DEMO_DEAD_TAG	0x1ACEDEADu	/* DEAD_NAME nh_msgid qualifier */

#define	SPIN_LABEL	"com.style9.spinner"
#define	SPIN_PROGRAM	"loopchild"	/* syscall-free long-runner */

#define	KA_LABEL	"com.style9.guardian"
#define	KA_PROGRAM	"loopchild"	/* keep_alive restart target */

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
	case LAUNCHD_STATE_STOPPED: return ("stopped");
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
    const char *program, uint32_t flags)
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

	body->lr_flags = flags;
	body->lr_pad   = 0;
}

static void
build_byname_req(uint8_t *buf, mach_port_name_t dest, const char *label,
    uint32_t op)
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
	hdr->msgh_id      = op;

	for (i = 0; i < LAUNCHD_NAME_MAX; i++)
		body->lr_name[i] = 0;
	for (i = 0; i < LAUNCHD_NAME_MAX && label[i] != '\0'; i++)
		body->lr_name[i] = label[i];
}

static int
do_load(mach_port_name_t launchd, const char *label, const char *program,
    uint32_t flags, uint64_t *out_task_id, mach_port_name_t *out_taskport)
{
	uint8_t		req_buf[sizeof(struct mach_msg_header) +
			    sizeof(struct svc_launchctl_load_req)];
	struct {
		struct mach_msg_header			hdr;
		struct svc_launchctl_status_reply	body;
	} reply;
	int		rv;

	build_load_req(req_buf, launchd, label, program, flags);

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
	if (out_task_id != NULL)
		*out_task_id = reply.body.ls_task_id;
	if (out_taskport != NULL)
		*out_taskport = reply.body.ls_taskport;
	printf("  load '%s' (program=%s) -> %s task_id=%llu taskport=0x%x\n",
	    label, program, state_name(reply.body.ls_state),
	    (unsigned long long)reply.body.ls_task_id,
	    (unsigned)reply.body.ls_taskport);
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

	build_byname_req(req_buf, launchd, label, LAUNCHCTL_OP_UNLOAD);

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
 * do_stop / do_start: the v2 ops.  Both take a label (byname request)
 * and print the resulting state.  STOP kills the task but keeps the
 * entry (-> stopped); START respawns a stopped/exited entry (-> running).
 */
static int
do_stop(mach_port_name_t launchd, const char *label)
{
	uint8_t		req_buf[sizeof(struct mach_msg_header) +
			    sizeof(struct svc_launchctl_byname_req)];
	struct {
		struct mach_msg_header			hdr;
		struct svc_launchctl_status_reply	body;
	} reply;
	int		rv;

	build_byname_req(req_buf, launchd, label, LAUNCHCTL_OP_STOP);

	rv = mach_msg_rpc((struct mach_msg_header *)req_buf,
	    &reply.hdr, sizeof(reply), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  stop rpc rv=%d\n", rv);
		return (rv);
	}
	if (reply.body.ls_status != MACH_MSG_OK) {
		printf("  stop '%s' -> status=%d\n", label,
		    reply.body.ls_status);
		return (reply.body.ls_status);
	}
	printf("  stop '%s' -> %s (was task_id=%llu)\n",
	    label, state_name(reply.body.ls_state),
	    (unsigned long long)reply.body.ls_task_id);
	return (MACH_MSG_OK);
}

static int
do_start(mach_port_name_t launchd, const char *label)
{
	uint8_t		req_buf[sizeof(struct mach_msg_header) +
			    sizeof(struct svc_launchctl_byname_req)];
	struct {
		struct mach_msg_header			hdr;
		struct svc_launchctl_status_reply	body;
	} reply;
	int		rv;

	build_byname_req(req_buf, launchd, label, LAUNCHCTL_OP_START);

	rv = mach_msg_rpc((struct mach_msg_header *)req_buf,
	    &reply.hdr, sizeof(reply), 2000);
	if (rv != MACH_MSG_OK) {
		printf("  start rpc rv=%d\n", rv);
		return (rv);
	}
	if (reply.body.ls_status != MACH_MSG_OK) {
		printf("  start '%s' -> status=%d\n", label,
		    reply.body.ls_status);
		return (reply.body.ls_status);
	}
	printf("  start '%s' -> %s (task_id=%llu)\n",
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

/*
 * Arm a DEAD_NAME notification on `taskport` (a SEND right on a child's
 * task-self port, handed to us in the LOAD reply).  Returns a freshly
 * allocated notify port carrying the registration, or MACH_PORT_NULL if
 * anything failed (caller then falls back to the task_alive poll).  The
 * notify port is the caller's to deallocate.
 */
static mach_port_name_t
arm_dead_name(mach_port_name_t taskport)
{
	mach_port_name_t	notify;
	int			rv;

	if (taskport == MACH_PORT_NULL)
		return (MACH_PORT_NULL);

	notify = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	if (notify == MACH_PORT_NULL) {
		printf("  arm: port_allocate(notify) failed\n");
		return (MACH_PORT_NULL);
	}
	rv = mach_port_request_notification(taskport, MACH_NOTIFY_DEAD_NAME,
	    notify, DEMO_DEAD_TAG);
	if (rv != MACH_MSG_OK) {
		printf("  arm: request_notification(DEAD_NAME) rv=%d\n", rv);
		(void)mach_port_deallocate(notify);
		return (MACH_PORT_NULL);
	}
	printf("  armed DEAD_NAME on taskport=0x%x (notify=0x%x)\n",
	    (unsigned)taskport, (unsigned)notify);
	return (notify);
}

int
main(void)
{
	struct mach_notify_header	nh;
	mach_port_name_t		launchd;
	mach_port_name_t		child_taskport;
	mach_port_name_t		notify;
	uint64_t			child_task_id;
	int				i;
	int				alive;
	int				rv;

	launchd = launchd_lookup();
	if (launchd == MACH_PORT_NULL) {
		printf("launchctl: bootstrap_lookup('%s') failed\n",
		    SVC_LAUNCHD_NAME);
		return (1);
	}
	printf("launchctl: connected to '%s' (port=0x%x)\n",
	    SVC_LAUNCHD_NAME, (unsigned)launchd);

	child_task_id  = 0;
	child_taskport = MACH_PORT_NULL;
	notify         = MACH_PORT_NULL;

	if (do_list(launchd, "initial") != MACH_MSG_OK)
		return (2);

	if (do_load(launchd, DEMO_LABEL, DEMO_PROGRAM, 0,
	    &child_task_id, &child_taskport) != MACH_MSG_OK)
		return (3);

	/*
	 * Arm the death watch NOW, while the child is alive -- DEAD_NAME
	 * is a one-shot that can only be registered before the port dies.
	 * The kernel handed us a SEND on the child's task-self port in the
	 * LOAD reply (ls_taskport); when the child is reaped after UNLOAD
	 * the RECEIVE drop fires the notification onto `notify`.
	 */
	notify = arm_dead_name(child_taskport);

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

	/*
	 * Death by notification (the v2 path): block on the notify port
	 * instead of yield-polling task_alive.  UNLOAD's
	 * task_request_terminate set t_killed + woke echod's parked
	 * mach_msg_recv; the resumed thread retires, becomes a zombie,
	 * and is reaped.  task_deref removes it from task_list (so
	 * task_alive already reads false) and then port_release_task_self
	 * drops the task-self port's RECEIVE right -- which, with our SEND
	 * still outstanding, fires MACH_NOTIFY_DEAD_NAME onto `notify`.
	 * Our recv parks us, yielding the CPU so all of that can happen
	 * while we wait.
	 */
	if (notify != MACH_PORT_NULL) {
		rv = mach_msg_recv_timed(notify, &nh.hdr, sizeof(nh), 4000);
		if (rv != MACH_MSG_OK) {
			printf("  DEAD_NAME wait rv=%d (notification missed)\n",
			    rv);
		} else if (nh.hdr.msgh_id != (uint32_t)MACH_NOTIFY_DEAD_NAME ||
		    nh.nh_msgid != DEMO_DEAD_TAG) {
			printf("  DEAD_NAME unexpected id=%u tag=0x%x\n",
			    (unsigned)nh.hdr.msgh_id, (unsigned)nh.nh_msgid);
		} else {
			printf("  DEAD_NAME for task_id=%llu: notified "
			    "(death by notification, no poll)\n",
			    (unsigned long long)child_task_id);
		}
		/*
		 * Single confirming probe (not a poll): task__chain_remove
		 * runs before port_release_task_self, so by the time the
		 * notification lands the id is already gone from task_list.
		 */
		printf("  task_alive(%llu) confirm: %s\n",
		    (unsigned long long)child_task_id,
		    task_alive(child_task_id) ? "yes (unexpected!)" :
		    "no (consistent)");
		(void)mach_port_deallocate(notify);
		(void)mach_port_deallocate(child_taskport);
	} else if (child_task_id != 0) {
		/*
		 * Fallback when no taskport was handed back (older kernel)
		 * or arming failed: the v1 bounded yield-poll on task_alive.
		 */
		alive = 1;
		for (i = 0; i < 64 && alive; i++) {
			(void)yield();
			alive = task_alive(child_task_id);
		}
		printf("  task_alive(%llu) after unload: %s (waited %d "
		    "yields) [poll fallback]\n",
		    (unsigned long long)child_task_id,
		    alive ? "yes (kill failed)" : "no (kill landed)", i);
	}

	/*
	 * v2 STOP / START demo on a separate, non-keepalive service so it
	 * does not entangle with the echod DEAD_NAME watch above.  Loads a
	 * syscall-free spinner, STOPs it (kill, entry survives as stopped),
	 * then STARTs it (respawn -> running), confirming each transition
	 * via LIST.  Final UNLOAD reaps it.
	 */
	printf("\nlaunchctl v2 STOP/START demo:\n");
	if (do_load(launchd, SPIN_LABEL, SPIN_PROGRAM, 0, NULL, NULL) ==
	    MACH_MSG_OK) {
		(void)do_list(launchd, "spinner loaded");
		(void)do_stop(launchd, SPIN_LABEL);
		(void)do_list(launchd, "after stop");
		(void)do_start(launchd, SPIN_LABEL);
		(void)do_list(launchd, "after start");
		(void)do_unload(launchd, SPIN_LABEL);
	}

	/*
	 * v2 keep_alive demo: load a keep_alive job, then SIMULATE A CRASH
	 * by killing its task directly through the capability handed back
	 * in the load reply -- NOT launchd STOP, which intentionally
	 * suppresses restart.  The launchd worker observes the DEAD_NAME
	 * and respawns it; the post-crash LIST should show it RUNNING again
	 * under a fresh task_id (compare against the printed original).
	 */
	printf("\nlaunchctl keep_alive demo:\n");
	{
		uint64_t		ka_task;
		mach_port_name_t	ka_taskport;

		ka_task     = 0;
		ka_taskport = MACH_PORT_NULL;
		if (do_load(launchd, KA_LABEL, KA_PROGRAM,
		    LAUNCHD_LOAD_FLAG_KEEPALIVE, &ka_task, &ka_taskport) ==
		    MACH_MSG_OK && ka_taskport != MACH_PORT_NULL) {
			printf("  guardian task_id=%llu; simulating crash via "
			    "task_kill(taskport=0x%x)\n",
			    (unsigned long long)ka_task,
			    (unsigned)ka_taskport);
			(void)task_kill(ka_taskport);
			(void)mach_port_deallocate(ka_taskport);
			/* Let the worker observe DEAD_NAME + respawn. */
			for (i = 0; i < 128; i++)
				(void)yield();
			(void)do_list(launchd,
			    "after crash (expect running, NEW task_id)");
			(void)do_unload(launchd, KA_LABEL);
		}
	}

	(void)mach_port_deallocate(launchd);
	return (0);
}
