/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * launchctl -- userspace CLI for the in-kernel launchd analog.
 *
 * A scripted demo, taking no arguments: load / list / use the service /
 * unload against the `launchd' bootstrap service, then stop/start,
 * keepalive and respawn-throttle scenes.  The ops map onto Darwin's
 * launchctl subcommands:
 *
 *	list                       LAUNCHCTL_OP_LIST
 *	load /path/foo.plist       LAUNCHCTL_OP_LOAD  (name + program)
 *	unload /path/foo.plist     LAUNCHCTL_OP_UNLOAD
 *	stop / start <label>       LAUNCHCTL_OP_STOP / LAUNCHCTL_OP_START
 *
 * Between load and unload it looks up the loaded service's own port
 * ("echo") and RPCs it, so the daemon is shown doing work.
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

#define	THR_LABEL	"com.style9.flaky"
#define	THR_PROGRAM	"crasher"	/* exits immediately -> throttle */

#define	MS_LABEL	"com.style9.svcecho"
#define	MS_PROGRAM	"svcecho"	/* a Mach service, checks in */
#define	MS_CRASH	0x5EC0DEADu	/* svcecho exits on this */
#define	MS_WAIT_MS	4000u

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
	case LAUNCHD_STATE_RUNNING:   return ("running");
	case LAUNCHD_STATE_EXITED:    return ("exited");
	case LAUNCHD_STATE_FAILED:    return ("failed");
	case LAUNCHD_STATE_STOPPED:   return ("stopped");
	case LAUNCHD_STATE_THROTTLED: return ("throttled");
	default:                      return ("?");
	}
}

/* ---- ops ------------------------------------------------------------- */

/* RPC LAUNCHCTL_OP_LIST into a full-size (520-byte body) stack reply. */
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
 * Build a LAUNCHCTL_OP_LOAD message in `buf' to `dest', with an inline
 * svc_launchctl_load_req from (label, program, flags).
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
 * do_stop / do_start take a label and print the resulting state.  STOP
 * kills the task but keeps the entry (-> stopped); START respawns a
 * stopped or exited entry (-> running).
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
 * RPC the running echod `rounds' times through the ordinary bootstrap
 * lookup, checking each reply echoes its msgh_id.
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
	if (i == rounds)
		printf("  poke 'echo' x %u rounds: ok\n", i);
	else
		printf("  poke 'echo': FAIL after %u of %u rounds\n", i,
		    rounds);
	(void)mach_port_deallocate(svc);
}

/*
 * Arm a DEAD_NAME notification on `taskport' (a send right on the
 * child's task port, from the LOAD reply).  Returns a new notify port,
 * which the caller deallocates, or MACH_PORT_NULL on failure (the caller
 * then polls task_alive).
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

/* A bare request to `svc`, with a SEND made from `reply` if there is one. */
static void
ms_req(struct mach_msg_header *h, mach_port_name_t svc,
    mach_port_name_t reply, uint32_t id)
{

	h->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    reply != MACH_PORT_NULL ? MACH_MSG_TYPE_MAKE_SEND : 0);
	h->msgh_size    = sizeof(*h);
	h->msgh_remote  = svc;
	h->msgh_local   = reply;
	h->msgh_voucher = 0;
	h->msgh_id      = id;
}

/* Send `id` to `svc` and wait for its echo on `reply`. */
static int
ms_ask(mach_port_name_t svc, mach_port_name_t reply, uint32_t id)
{
	struct mach_msg_header	req;
	struct mach_msg_header	ans;

	ms_req(&req, svc, reply, id);
	return (mach_msg_send(&req) == MACH_MSG_OK &&
	    mach_msg_recv_timed(reply, &ans, sizeof(ans), MS_WAIT_MS) ==
	    MACH_MSG_OK && ans.msgh_id == id);
}

/*
 * A Mach service's port outlives its job.  launchd makes the port and the
 * job checks the receive right in; when the job dies, PORT_DESTROYED
 * brings the right back to launchd with its queue, for the instance
 * keep_alive starts next.  The crash request and one behind it go in
 * together: the dying instance reads only the first, and the respawned
 * one answers the second, sent to the name looked up before the crash.
 */
static void
machservice_demo(mach_port_name_t launchd)
{
	struct mach_msg_header	req;
	mach_port_name_t	svc;
	mach_port_name_t	reply;
	mach_port_name_t	taskport;
	uint64_t		first;
	int			ok;

	printf("\nlaunchctl Mach service demo:\n");
	first    = 0;
	taskport = MACH_PORT_NULL;
	if (do_load(launchd, MS_LABEL, MS_PROGRAM,
	    LAUNCHD_LOAD_FLAG_KEEPALIVE | LAUNCHD_LOAD_FLAG_MACHSERVICE,
	    &first, &taskport) != MACH_MSG_OK) {
		printf("launchctl: FAIL the Mach service did not load\n");
		return;
	}
	svc   = bootstrap_lookup(MS_LABEL);
	reply = mach_port_allocate(MACH_PORT_RIGHT_RECEIVE |
	    MACH_PORT_RIGHT_SEND);
	ok = svc != MACH_PORT_NULL && reply != MACH_PORT_NULL &&
	    ms_ask(svc, reply, 0x100);
	if (ok) {
		ms_req(&req, svc, MACH_PORT_NULL, MS_CRASH);
		ok = mach_msg_send(&req) == MACH_MSG_OK &&
		    ms_ask(svc, reply, 0x200) && !task_alive(first);
	}
	if (ok)
		printf("launchctl: PASS a Mach service that crashed kept its "
		    "port: the request queued behind the crash was answered "
		    "by the respawned instance, through the name looked up "
		    "before it\n");
	else
		printf("launchctl: FAIL a request queued behind a Mach "
		    "service's crash went unanswered, or the old instance "
		    "answered it\n");
	(void)do_list(launchd, "after the service's crash (expect running, "
	    "NEW task_id)");
	(void)do_unload(launchd, MS_LABEL);
	if (svc != MACH_PORT_NULL)
		(void)mach_port_deallocate(svc);
	if (reply != MACH_PORT_NULL)
		(void)mach_port_deallocate(reply);
	if (taskport != MACH_PORT_NULL)
		(void)mach_port_deallocate(taskport);
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
	 * Arm the death watch while the child is alive: DEAD_NAME can only
	 * be registered before the port dies.  When the child is reaped
	 * after UNLOAD, its task port's receive right goes and the
	 * notification lands on `notify'.
	 */
	notify = arm_dead_name(child_taskport);

	if (do_list(launchd, "after-load") != MACH_MSG_OK)
		return (4);

	/* Give echod a few turns to start serving before the first poke. */
	for (i = 0; i < 16; i++)
		(void)poll_turn();

	poke_echo(ECHO_RPC_ROUNDS);

	if (do_list(launchd, "after-poke") != MACH_MSG_OK)
		return (5);

	if (do_unload(launchd, DEMO_LABEL) != MACH_MSG_OK)
		return (6);

	if (do_list(launchd, "after-unload") != MACH_MSG_OK)
		return (7);

	/*
	 * Wait on the notify port instead of polling task_alive.  UNLOAD's
	 * kill wakes echod's parked receive; the thread retires and the
	 * task is reaped.  task_deref takes it off the task list (so
	 * task_alive already says no), then port_release_task_self drops
	 * the task port's receive right, which with our send right still
	 * held fires MACH_NOTIFY_DEAD_NAME onto `notify'.
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
		 * One confirming probe: task__chain_remove runs before
		 * port_release_task_self, so the id is already gone.
		 */
		printf("  task_alive(%llu) confirm: %s\n",
		    (unsigned long long)child_task_id,
		    task_alive(child_task_id) ? "yes (unexpected!)" :
		    "no (consistent)");
		(void)mach_port_deallocate(notify);
		(void)mach_port_deallocate(child_taskport);
	} else if (child_task_id != 0) {
		/* No task port or arming failed: bounded poll of task_alive. */
		alive = 1;
		for (i = 0; i < 64 && alive; i++) {
			(void)poll_turn();
			alive = task_alive(child_task_id);
		}
		printf("  task_alive(%llu) after unload: %s (waited %d "
		    "turns) [poll fallback]\n",
		    (unsigned long long)child_task_id,
		    alive ? "yes (kill failed)" : "no (kill landed)", i);
	}

	/*
	 * STOP / START on a separate, non-keepalive job: load a
	 * syscall-free spinner, stop it (entry survives as stopped), start
	 * it again, LIST after each step, then unload.
	 */
	printf("\nlaunchctl STOP/START demo:\n");
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
	 * Keepalive: load a keepalive job, then simulate a crash by killing
	 * its task through the task port from the load reply (not STOP,
	 * which suppresses restart).  launchd sees the DEAD_NAME and
	 * respawns it; LIST should show it running under a new task_id.
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
				(void)poll_turn();
			(void)do_list(launchd,
			    "after crash (expect running, NEW task_id)");
			(void)do_unload(launchd, KA_LABEL);
		}
	}

	/*
	 * Respawn throttle: a keepalive job whose program (crasher) exits
	 * at once.  After a few fast exits launchd parks it in `throttled'.
	 * Wait out the crash loop, LIST, unload.  (START would revive it
	 * and clear the fast-exit count.)
	 */
	printf("\nlaunchctl respawn-throttle demo:\n");
	if (do_load(launchd, THR_LABEL, THR_PROGRAM,
	    LAUNCHD_LOAD_FLAG_KEEPALIVE, NULL, NULL) == MACH_MSG_OK) {
		for (i = 0; i < 256; i++)
			(void)poll_turn();
		(void)do_list(launchd, "after crash loop (expect throttled)");
		(void)do_unload(launchd, THR_LABEL);
	}

	machservice_demo(launchd);

	(void)mach_port_deallocate(launchd);
	return (0);
}
