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
#include "dev_subsystem.h"
#include "klog.h"
#include "kmem.h"
#include "kprintf.h"
#include "memmap.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "services.h"
#include "shell.h"
#include "stress.h"
#include "task.h"
#include "thread.h"
#include "tty.h"
#include "vm.h"

static int	cmd_help(int, char **);
static int	cmd_mem(int, char **);
static int	cmd_uptime(int, char **);
static int	cmd_pmap(int, char **);
static int	cmd_memmap(int, char **);
static int	cmd_clear(int, char **);
static int	cmd_echo(int, char **);
static int	cmd_panic(int, char **);
static int	cmd_port(int, char **);
static int	cmd_task(int, char **);
static int	cmd_thread(int, char **);
static int	cmd_sched(int, char **);
static int	cmd_yield(int, char **);
static int	cmd_stress(int, char **);
static int	cmd_crash(int, char **);
static int	cmd_mach(int, char **);
static int	cmd_vmmap(int, char **);
static int	cmd_log(int, char **);
static int	cmd_dev(int, char **);
static int	cmd_disk(int, char **);

static int	streq(const char *, const char *);
static int	parse_uint(const char *s, unsigned int *out);

/*
 * Command table.  Order is the order 'help' prints; keep the dangerous
 * ones (panic, crash) at the bottom so the safe set scrolls into view
 * first.
 */
const struct shell_cmd	shell_cmds[] = {
	{ "help",   "list commands",                          cmd_help   },
	{ "?",      "alias for help",                         cmd_help   },
	{ "mem",    "pmm + kmem stats",                       cmd_mem    },
	{ "memmap", "firmware-supplied physical map",         cmd_memmap },
	{ "pmap",   "kernel pmap state",                      cmd_pmap   },
	{ "uptime", "kernel uptime",                          cmd_uptime },
	{ "clear",  "clear the screen",                       cmd_clear  },
	{ "echo",   "echo arguments",                         cmd_echo   },
	{ "port",   "port <list|new|send N|recv N|pingpong>", cmd_port   },
	{ "task",   "list tasks",                             cmd_task   },
	{ "thread", "list threads",                           cmd_thread },
	{ "sched",  "scheduler state + ctx switches",         cmd_sched  },
	{ "yield",  "yield to next thread (then return)",     cmd_yield  },
	{ "stress", "stress <mem|boundary|timer|port|thread|preempt>", cmd_stress },
	{ "mach",   "mach <ls|clock|stats|tasks> (bootstrap-served RPCs)",
		cmd_mach   },
	{ "vmmap",  "kernel_task vm_map entries",             cmd_vmmap  },
	{ "log",    "log <tail|debug|info|warn|error> [text]", cmd_log   },
	{ "dev",    "dev <ls|info NAME|write uart TEXT>",     cmd_dev    },
	{ "disk",   "disk <ls|info NAME|read NAME LBA|write NAME LBA TEXT|sync NAME>",
		cmd_disk   },
	{ "crash",  "crash <dfree|wild|assert|unmapped|nonc>", cmd_crash  },
	{ "panic",  "deliberate panic (tests panic path)",    cmd_panic  },
};
const size_t	shell_ncmds = sizeof(shell_cmds) / sizeof(shell_cmds[0]);

static int
cmd_help(int argc, char *argv[])
{
	size_t	i;

	(void)argc;
	(void)argv;

	kprintf("commands:\n");
	for (i = 0; i < shell_ncmds; i++)
		kprintf("  %-8s  %s\n",
		    shell_cmds[i].sc_name, shell_cmds[i].sc_help);

	return (0);
}

static int
cmd_mem(int argc, char *argv[])
{

	(void)argc;
	(void)argv;

	pmm_stats();
	kmem_stats();
	return (0);
}

static int
cmd_memmap(int argc, char *argv[])
{

	(void)argc;
	(void)argv;

	memmap_print();
	return (0);
}

static int
cmd_pmap(int argc, char *argv[])
{

	(void)argc;
	(void)argv;

	pmap_stats();
	return (0);
}

static int
cmd_uptime(int argc, char *argv[])
{
	uint64_t	ms, s;

	(void)argc;
	(void)argv;

	ms = clock_uptime_ms();
	s  = ms / 1000;
	kprintf("up %llu s (%llu ms, %llu ticks @ %llu Hz)\n",
	    (unsigned long long)s,
	    (unsigned long long)ms,
	    (unsigned long long)clock_ticks(),
	    (unsigned long long)clock_hz());
	return (0);
}

static int
cmd_clear(int argc, char *argv[])
{

	(void)argc;
	(void)argv;

	tty_clear();
	return (0);
}

static int
cmd_echo(int argc, char *argv[])
{
	int	i;

	for (i = 1; i < argc; i++) {
		if (i > 1)
			tty_putc(' ');
		tty_puts(argv[i]);
	}
	tty_putc('\n');
	return (0);
}

static int
cmd_panic(int argc, char *argv[])
{

	(void)argc;
	(void)argv;

	panic("user-requested panic from shell");
	/* NOTREACHED */
}

static int
cmd_stress(int argc, char *argv[])
{
	unsigned int	n;

	if (argc < 2) {
		kprintf("usage: stress <mem [N] | boundary | timer>\n");
		return (1);
	}

	if (streq(argv[1], "mem")) {
		n = 10000;
		if (argc >= 3 && parse_uint(argv[2], &n) != 0) {
			kprintf("stress mem: bad iteration count '%s'\n",
			    argv[2]);
			return (1);
		}
		return (stress_mem(n));
	}
	if (streq(argv[1], "boundary"))
		return (stress_mem_boundary());
	if (streq(argv[1], "timer"))
		return (stress_timer(2));
	if (streq(argv[1], "port")) {
		n = 1000;
		if (argc >= 3 && parse_uint(argv[2], &n) != 0) {
			kprintf("stress port: bad iteration count '%s'\n",
			    argv[2]);
			return (1);
		}
		return (stress_port(n));
	}
	if (streq(argv[1], "thread")) {
		n = 200;
		if (argc >= 3 && parse_uint(argv[2], &n) != 0) {
			kprintf("stress thread: bad iteration count '%s'\n",
			    argv[2]);
			return (1);
		}
		return (stress_thread(n));
	}
	if (streq(argv[1], "preempt")) {
		unsigned int workers = 4;
		unsigned int ms = 1000;
		if (argc >= 3 && parse_uint(argv[2], &workers) != 0) {
			kprintf("stress preempt: bad worker count '%s'\n",
			    argv[2]);
			return (1);
		}
		if (argc >= 4 && parse_uint(argv[3], &ms) != 0) {
			kprintf("stress preempt: bad ms '%s'\n",
			    argv[3]);
			return (1);
		}
		return (stress_preempt(workers, ms));
	}
	if (streq(argv[1], "sendonce")) {
		n = 500;
		if (argc >= 3 && parse_uint(argv[2], &n) != 0) {
			kprintf("stress sendonce: bad count '%s'\n",
			    argv[2]);
			return (1);
		}
		return (stress_sendonce(n));
	}
	if (streq(argv[1], "portset")) {
		unsigned int members = 4, per_member = 100;
		if (argc >= 3 && parse_uint(argv[2], &members) != 0) {
			kprintf("stress portset: bad member count '%s'\n",
			    argv[2]);
			return (1);
		}
		if (argc >= 4 && parse_uint(argv[3], &per_member) != 0) {
			kprintf("stress portset: bad per-member '%s'\n",
			    argv[3]);
			return (1);
		}
		return (stress_portset(members, per_member));
	}
	if (streq(argv[1], "intertask")) {
		n = 200;
		if (argc >= 3 && parse_uint(argv[2], &n) != 0) {
			kprintf("stress intertask: bad count '%s'\n",
			    argv[2]);
			return (1);
		}
		return (stress_intertask(n));
	}
	if (streq(argv[1], "all")) {
		int rv;
		rv = stress_mem(10000);
		if (rv != 0)
			return (rv);
		rv = stress_mem_boundary();
		if (rv != 0)
			return (rv);
		rv = stress_timer(2);
		if (rv != 0)
			return (rv);
		rv = stress_port(1000);
		if (rv != 0)
			return (rv);
		rv = stress_thread(200);
		return (rv);
	}

	kprintf("stress: unknown subcommand '%s'\n", argv[1]);
	return (1);
}

static int
cmd_task(int argc, char *argv[])
{

	(void)argc;
	(void)argv;
	task_list_print();
	return (0);
}

static int
cmd_thread(int argc, char *argv[])
{

	(void)argc;
	(void)argv;
	sched_print();
	return (0);
}

static int
cmd_sched(int argc, char *argv[])
{

	(void)argc;
	(void)argv;
	kprintf("context switches: %llu, runq len: %zu\n",
	    (unsigned long long)sched_context_switches(),
	    sched_runq_len());
	return (0);
}

static int
cmd_yield(int argc, char *argv[])
{
	uint64_t	before, after;

	(void)argc;
	(void)argv;

	before = sched_context_switches();
	thread_yield();
	after = sched_context_switches();
	kprintf("yielded; ctx switches %llu -> %llu (delta %llu)\n",
	    (unsigned long long)before,
	    (unsigned long long)after,
	    (unsigned long long)(after - before));
	return (0);
}

/*
 * Tiny "echo server" demo: client sends a request containing its
 * reply port as a port descriptor, server flips a couple of bytes
 * and sends back via that reply port.  All inside kernel_space so
 * we don't need tasks yet -- the same code will work unchanged once
 * each task gets its own port_space.
 */
struct demo_msg {
	struct mach_msg_header	hdr;
	struct mach_msg_body	body;
	struct mach_msg_port_descriptor reply_pd;
	uint32_t		payload;
};

static int
cmd_port(int argc, char *argv[])
{
	mach_port_name_t	server, client_reply;
	struct demo_msg		req, recv_buf;
	struct mach_msg_header	reply;
	int			rv;
	unsigned int		n;

	if (argc < 2) {
		kprintf("usage: port <list|new|pingpong [N]>\n");
		return (1);
	}

	if (streq(argv[1], "list")) {
		port_space_print(kernel_space);
		return (0);
	}

	if (streq(argv[1], "new")) {
		mach_port_name_t name = port_allocate(kernel_space,
		    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
		if (name == MACH_PORT_NULL) {
			kprintf("port new: allocation failed\n");
			return (1);
		}
		kprintf("port new: name=%u (RECV+SEND)\n", name);
		return (0);
	}

	if (streq(argv[1], "pingpong")) {
		n = 1;
		if (argc >= 3 && parse_uint(argv[2], &n) != 0) {
			kprintf("port pingpong: bad count '%s'\n", argv[2]);
			return (1);
		}

		server = port_allocate(kernel_space,
		    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
		client_reply = port_allocate(kernel_space,
		    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
		if (server == MACH_PORT_NULL ||
		    client_reply == MACH_PORT_NULL) {
			kprintf("port pingpong: allocation failed\n");
			return (1);
		}

		kprintf("port pingpong: server=%u reply=%u, %u rounds\n",
		    server, client_reply, n);

		for (unsigned i = 0; i < n; i++) {
			/* --- client side: build request ------------- */
			req.hdr.msgh_bits = MACH_MSGH_BITS(
			    MACH_MSG_TYPE_COPY_SEND, 0) |
			    MACH_MSGH_BITS_COMPLEX;
			req.hdr.msgh_size    = sizeof(req);
			req.hdr.msgh_remote  = server;
			req.hdr.msgh_local   = MACH_PORT_NULL;
			req.hdr.msgh_voucher = 0;
			req.hdr.msgh_id      = 0x1000 + i;
			req.body.msgh_descriptor_count = 1;
			req.reply_pd.name        = client_reply;
			req.reply_pd.disposition = MACH_MSG_TYPE_MAKE_SEND;
			req.reply_pd.type        = MACH_MSG_PORT_DESCRIPTOR;
			req.reply_pd.pad1        = 0;
			req.reply_pd.pad2        = 0;
			req.payload              = 0xCAFE0000u | i;

			rv = mach_msg_send(kernel_space, &req.hdr);
			if (rv != MACH_MSG_OK) {
				kprintf("port pingpong: send #%u: %s\n",
				    i, mach_msg_strerror(rv));
				goto out;
			}

			/* --- server side: receive --------------------- */
			rv = mach_msg_recv(kernel_space, server,
			    &recv_buf.hdr, sizeof(recv_buf));
			if (rv != MACH_MSG_OK) {
				kprintf("port pingpong: recv #%u: %s\n",
				    i, mach_msg_strerror(rv));
				goto out;
			}

			/* server now holds a SEND right on client's reply */
			mach_port_name_t reply_name =
			    recv_buf.reply_pd.name;
			uint32_t echoed = recv_buf.payload ^ 0xFFFFFFFFu;

			/* --- server side: send reply ------------------ */
			reply.msgh_bits    = MACH_MSGH_BITS(
			    MACH_MSG_TYPE_MOVE_SEND, 0);
			reply.msgh_size    = sizeof(reply) + 4;
			reply.msgh_remote  = reply_name;
			reply.msgh_local   = MACH_PORT_NULL;
			reply.msgh_voucher = 0;
			reply.msgh_id      = recv_buf.hdr.msgh_id + 1;
			/* Pad-out: a malformed-size protection check */

			/* For simplicity send just the header (no extra). */
			reply.msgh_size = sizeof(reply);
			rv = mach_msg_send(kernel_space, &reply);
			if (rv != MACH_MSG_OK) {
				kprintf("port pingpong: server send #%u: "
				    "%s\n", i, mach_msg_strerror(rv));
				goto out;
			}

			/* --- client side: receive reply --------------- */
			rv = mach_msg_recv(kernel_space, client_reply,
			    &reply, sizeof(reply));
			if (rv != MACH_MSG_OK) {
				kprintf("port pingpong: client recv #%u: "
				    "%s\n", i, mach_msg_strerror(rv));
				goto out;
			}

			if (reply.msgh_id != req.hdr.msgh_id + 1)
				kprintf("port pingpong: id mismatch "
				    "%u vs %u\n",
				    reply.msgh_id, req.hdr.msgh_id + 1);
			(void)echoed;
		}

		kprintf("port pingpong: %u rounds OK\n", n);

out:
		port_deallocate(kernel_space, server);
		port_deallocate(kernel_space, client_reply);
		return (rv);
	}

	kprintf("port: unknown subcommand '%s'\n", argv[1]);
	return (1);
}

/*
 * Deliberate crash injection.  Each variant trips a distinct kernel
 * check, so a "pass" here is the panic-path producing a readable
 * autopsy -- not the kernel surviving.
 */
static int
cmd_crash(int argc, char *argv[])
{
	void			*p;
	volatile uint64_t	*up;

	if (argc < 2) {
		kprintf("usage: crash <dfree|wild|assert|unmapped|nonc>\n");
		return (1);
	}

	if (streq(argv[1], "dfree")) {
		p = kmalloc(64);
		kfree(p);
		kprintf("crash dfree: freeing %p a second time...\n", p);
		kfree(p);
		return (0);			/* unreachable */
	}

	if (streq(argv[1], "wild")) {
		/* Point 64 bytes into a real allocation -- header magic mismatch. */
		p = kmalloc(128);
		kprintf("crash wild: kfree(%p + 64)...\n", p);
		kfree((uint8_t *)p + 64);
		return (0);			/* unreachable */
	}

	if (streq(argv[1], "assert")) {
		kprintf("crash assert: tripping KASSERT(0)...\n");
		KASSERT(0, "deliberate assertion from shell");
		return (0);			/* unreachable */
	}

	/*
	 * Canonical higher-half address that lives in an empty PML4
	 * slot (the boot identity map only fills PML4[0]).  Touching
	 * it produces a #PF with a clear unmapped-page signature.
	 * Note: writing through NULL would NOT crash here -- the boot
	 * identity map covers VA 0.
	 */
	if (streq(argv[1], "unmapped")) {
		up = (volatile uint64_t *)0xFFFF800000DEAD00ULL;
		kprintf("crash unmapped: writing 0xDEAD to %p...\n",
		    (void *)up);
		*up = 0xDEAD;
		return (0);			/* unreachable */
	}

	/*
	 * Non-canonical address: bit 47 disagrees with bits 48-63.
	 * Dereference triggers #GP (vector 13) rather than #PF.
	 */
	if (streq(argv[1], "nonc")) {
		up = (volatile uint64_t *)0x0000800000000000ULL;
		kprintf("crash nonc: writing to non-canonical %p...\n",
		    (void *)up);
		*up = 0xDEAD;
		return (0);			/* unreachable */
	}

	kprintf("crash: unknown variant '%s'\n", argv[1]);
	return (1);
}

/* ---- helpers ------------------------------------------------------------ */

static int
streq(const char *a, const char *b)
{

	while (*a != '\0' && *b != '\0') {
		if (*a != *b)
			return (0);
		a++;
		b++;
	}
	return (*a == '\0' && *b == '\0');
}

static int
parse_uint(const char *s, unsigned int *out)
{
	unsigned int	v;

	if (s == NULL || *s == '\0')
		return (-1);

	v = 0;
	while (*s != '\0') {
		if (*s < '0' || *s > '9')
			return (-1);
		v = v * 10 + (unsigned int)(*s - '0');
		s++;
	}
	*out = v;
	return (0);
}

/* ---- `mach' command: bootstrap + service RPC demo --------------------- */

/*
 * Resolve a service name via the bootstrap port and return a SEND name
 * for it in kernel_space.  The caller must port_deallocate when done.
 * Returns MACH_PORT_NULL on failure.
 */
static mach_port_name_t
mach_lookup(const char *svc_name)
{
	struct {
		struct mach_msg_header			hdr;
		struct bootstrap_lookup_request		body;
	} req;
	struct {
		struct mach_msg_header			hdr;
		struct mach_msg_body			body;
		struct mach_msg_port_descriptor		pd;
	} reply;
	size_t	i;
	int	rv;

	for (i = 0; i < BOOTSTRAP_NAME_MAX; i++)
		req.body.blr_name[i] = 0;
	for (i = 0; svc_name[i] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; i++)
		req.body.blr_name[i] = svc_name[i];

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = MACH_PORT_BOOTSTRAP;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = BOOTSTRAP_OP_LOOKUP;

	rv = mach_msg_rpc(kernel_space, &req.hdr, &reply.hdr,
	    sizeof(reply), 1000);
	if (rv != MACH_MSG_OK)
		return (MACH_PORT_NULL);
	if (reply.hdr.msgh_id == BOOTSTRAP_REPLY_NOT_FOUND ||
	    !(reply.hdr.msgh_bits & MACH_MSGH_BITS_COMPLEX))
		return (MACH_PORT_NULL);
	return (reply.pd.name);
}

static int
mach_ls(void)
{
	char			names[BOOTSTRAP_MAX_SERVICES][BOOTSTRAP_NAME_MAX];
	mach_port_name_t	knames[BOOTSTRAP_MAX_SERVICES];
	size_t			n, i;

	n = bootstrap_snapshot(names, knames, BOOTSTRAP_MAX_SERVICES);
	kprintf("bootstrap services: %zu registered\n", n);
	for (i = 0; i < n; i++)
		kprintf("  %-16s -> kernel name %u\n", names[i],
		    (unsigned)knames[i]);
	return (0);
}

static int
mach_call_clock(void)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header		hdr;
		struct svc_clock_reply		body;
	} reply;
	mach_port_name_t		svc;
	int				rv;

	svc = mach_lookup(SVC_CLOCK_NAME);
	if (svc == MACH_PORT_NULL) {
		kprintf("mach: clock lookup failed\n");
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = CLOCK_OP_GET;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply),
	    1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("mach clock: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}

	kprintf("clock: uptime %llu ms / %llu us, ticks=%llu\n",
	    (unsigned long long)reply.body.cr_uptime_ms,
	    (unsigned long long)reply.body.cr_uptime_us,
	    (unsigned long long)reply.body.cr_ticks);
	return (0);
}

static int
mach_call_stats(void)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header		hdr;
		struct svc_stats_reply		body;
	} reply;
	mach_port_name_t		svc;
	int				rv;

	svc = mach_lookup(SVC_STATS_NAME);
	if (svc == MACH_PORT_NULL) {
		kprintf("mach: stats lookup failed\n");
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = STATS_OP_GET;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply),
	    1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("mach stats: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}

	kprintf("stats: pmm_used=%llu kmem_cached=%llu kernel_inuse=%llu\n",
	    (unsigned long long)reply.body.sr_pmm_used_pages,
	    (unsigned long long)reply.body.sr_kmem_cached_pages,
	    (unsigned long long)reply.body.sr_kernel_inuse);
	kprintf("       tasks=%llu threads=%llu ctx_switches=%llu\n",
	    (unsigned long long)reply.body.sr_task_count,
	    (unsigned long long)reply.body.sr_thread_count,
	    (unsigned long long)reply.body.sr_ctx_switches);
	return (0);
}

static int
mach_call_tasks(void)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header		hdr;
		struct svc_tasks_reply		body;
	} reply;
	mach_port_name_t		svc;
	uint32_t			i;
	int				rv;

	svc = mach_lookup(SVC_TASKS_NAME);
	if (svc == MACH_PORT_NULL) {
		kprintf("mach: tasks lookup failed\n");
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = TASKS_OP_LIST;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply),
	    1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("mach tasks: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}

	kprintf("tasks via Mach: %u live\n",
	    (unsigned)reply.body.tr_count);
	for (i = 0; i < reply.body.tr_count && i < SVC_TASKS_MAX; i++) {
		kprintf("  id=%llu  threads=%u  name='%s'\n",
		    (unsigned long long)reply.body.tr_entries[i].te_task_id,
		    (unsigned)reply.body.tr_entries[i].te_nthreads,
		    reply.body.tr_entries[i].te_name);
	}
	return (0);
}

static int
cmd_mach(int argc, char *argv[])
{

	if (argc < 2 || streq(argv[1], "ls"))
		return (mach_ls());
	if (streq(argv[1], "clock"))
		return (mach_call_clock());
	if (streq(argv[1], "stats"))
		return (mach_call_stats());
	if (streq(argv[1], "tasks"))
		return (mach_call_tasks());

	kprintf("mach: unknown subcommand '%s'\n", argv[1]);
	kprintf("mach: usage: mach [ls|clock|stats|tasks]\n");
	return (1);
}

static int
cmd_vmmap(int argc, char *argv[])
{

	(void)argc;
	(void)argv;

	if (kernel_task == NULL || kernel_task->t_map == NULL) {
		kprintf("vmmap: kernel_task->t_map is NULL\n");
		return (1);
	}
	vm_map_print(kernel_task->t_map);
	return (0);
}

/* ---- `log' command -- klog tail + RPC writes -------------------------- */

static void
log_concat_args(char *dst, size_t cap, int argc, char *argv[])
{
	int	i;
	size_t	o;

	o = 0;
	for (i = 2; i < argc && o + 1 < cap; i++) {
		const char *s = argv[i];
		if (i > 2 && o + 1 < cap)
			dst[o++] = ' ';
		while (*s != '\0' && o + 1 < cap)
			dst[o++] = *s++;
	}
	dst[o] = '\0';
}

static int
log_tail(void)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header		hdr;
		struct klog_tail_reply		body;
	} reply;
	mach_port_name_t		svc;
	uint32_t			i;
	int				rv;

	svc = mach_lookup(SVC_KLOG_NAME);
	if (svc == MACH_PORT_NULL) {
		kprintf("log: klog lookup failed\n");
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = KLOG_OP_TAIL;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply),
	    1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("log tail: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}

	kprintf("klog tail: %u entries\n", (unsigned)reply.body.ktr_count);
	for (i = 0; i < reply.body.ktr_count; i++) {
		const struct klog_entry *e = &reply.body.ktr_entries[i];
		kprintf("  [%llu %s %s] %s\n",
		    (unsigned long long)e->ke_uptime_ms,
		    klog_level_name(e->ke_level),
		    e->ke_src,
		    e->ke_text);
	}
	return (0);
}

static int
log_write_via_mach(uint8_t level, const char *text)
{
	struct {
		struct mach_msg_header		hdr;
		struct klog_write_request	body;
	} req;
	struct mach_msg_header		reply;
	mach_port_name_t		svc;
	size_t				i;
	int				rv;

	svc = mach_lookup(SVC_KLOG_NAME);
	if (svc == MACH_PORT_NULL) {
		kprintf("log: klog lookup failed\n");
		return (1);
	}

	req.body.kwr_level = level;
	for (i = 0; i < sizeof(req.body.kwr_pad); i++)
		req.body.kwr_pad[i] = 0;
	for (i = 0; i < KLOG_SRC_MAX; i++)
		req.body.kwr_src[i] = 0;
	req.body.kwr_src[0] = 's';
	req.body.kwr_src[1] = 'h';
	for (i = 0; i < KLOG_LINE_MAX; i++)
		req.body.kwr_text[i] = 0;
	for (i = 0; text[i] != '\0' && i < KLOG_LINE_MAX - 1; i++)
		req.body.kwr_text[i] = text[i];

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = KLOG_OP_WRITE;

	rv = mach_msg_rpc(kernel_space, &req.hdr, &reply, sizeof(reply),
	    1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("log write: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}
	return (0);
}

static int
cmd_log(int argc, char *argv[])
{
	char	buf[KLOG_LINE_MAX];

	if (argc < 2) {
		return (log_tail());
	}
	if (streq(argv[1], "tail"))
		return (log_tail());

	if (argc < 3) {
		kprintf("log: usage: log <tail|debug|info|warn|error> [text]\n");
		return (1);
	}
	log_concat_args(buf, sizeof(buf), argc, argv);

	if (streq(argv[1], "debug"))
		return (log_write_via_mach(KLOG_LEVEL_DEBUG, buf));
	if (streq(argv[1], "info"))
		return (log_write_via_mach(KLOG_LEVEL_INFO,  buf));
	if (streq(argv[1], "warn"))
		return (log_write_via_mach(KLOG_LEVEL_WARN,  buf));
	if (streq(argv[1], "error"))
		return (log_write_via_mach(KLOG_LEVEL_ERROR, buf));

	kprintf("log: unknown level '%s'\n", argv[1]);
	return (1);
}

/* ---- dev control-port consumer ----------------------------------------- */

/*
 * Look up dev/<name> in the bootstrap port and return the resulting
 * SEND right's name in kernel_space.  Caller port_deallocate's it when
 * done.  Counterpart of mach_lookup() above, with the "dev/" prefix
 * applied so cmd_dev callers can use bare device names.
 */
static mach_port_name_t
dev_lookup(const char *short_name)
{
	char	full[BOOTSTRAP_NAME_MAX];
	size_t	i, j;

	for (i = 0; i < DEV_PREFIX_LEN; i++)
		full[i] = DEV_PREFIX[i];
	for (j = 0; short_name[j] != '\0' && i < BOOTSTRAP_NAME_MAX - 1; j++)
		full[i++] = short_name[j];
	full[i] = '\0';
	return (mach_lookup(full));
}

static int
dev_call_info(const char *name)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header	hdr;
		struct dev_info_reply	body;
	} reply;
	const char		*kind_name;
	mach_port_name_t	 svc;
	int			 rv;

	svc = dev_lookup(name);
	if (svc == MACH_PORT_NULL) {
		kprintf("dev info: '%s' not registered\n", name);
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = DEV_OP_INFO;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply), 1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("dev info: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}

	switch (reply.body.dir_kind) {
	case DEV_KIND_STREAM_RX: kind_name = "stream-rx"; break;
	case DEV_KIND_STREAM_TX: kind_name = "stream-tx"; break;
	case DEV_KIND_CHAR:      kind_name = "char";      break;
	case DEV_KIND_BLOCK:     kind_name = "block";     break;
	default:                 kind_name = "?";         break;
	}
	kprintf("  name=%-12s kind=%-10s flags=%c%c%c\n",
	    reply.body.dir_name, kind_name,
	    (reply.body.dir_flags & DEV_F_READABLE) ? 'r' : '-',
	    (reply.body.dir_flags & DEV_F_WRITABLE) ? 'w' : '-',
	    (reply.body.dir_flags & DEV_F_STREAM)   ? 's' : '-');
	return (0);
}

static int
dev_ls(void)
{
	char	names[8][DEV_NAME_MAX];
	size_t	n, i;

	n = dev_list_names(names, 8);
	if (n == 0) {
		kprintf("dev: no devices registered\n");
		return (0);
	}
	kprintf("dev: %zu device(s)\n", n);
	for (i = 0; i < n; i++)
		(void)dev_call_info(names[i]);
	return (0);
}

static int
dev_write(const char *name, const char *text)
{
	struct {
		struct mach_msg_header		hdr;
		struct dev_write_request	body;
	} req;
	struct {
		struct mach_msg_header		hdr;
		struct dev_write_reply		body;
	} reply;
	mach_port_name_t	svc;
	size_t			i, len;
	int			rv;

	svc = dev_lookup(name);
	if (svc == MACH_PORT_NULL) {
		kprintf("dev write: '%s' not registered\n", name);
		return (1);
	}

	req.body.dwr_pad = 0;
	for (i = 0; i < DEV_WRITE_MAX; i++)
		req.body.dwr_data[i] = 0;
	for (len = 0; text[len] != '\0' && len < DEV_WRITE_MAX - 1; len++)
		req.body.dwr_data[len] = (uint8_t)text[len];
	req.body.dwr_data[len++] = '\n';
	req.body.dwr_len = (uint32_t)len;

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = DEV_OP_WRITE;

	rv = mach_msg_rpc(kernel_space, &req.hdr, &reply.hdr, sizeof(reply),
	    1000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("dev write: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}
	kprintf("dev write %s: %u bytes\n", name,
	    (unsigned)reply.body.dwr_written);
	return (0);
}

static int
cmd_dev(int argc, char *argv[])
{

	if (argc < 2 || streq(argv[1], "ls"))
		return (dev_ls());
	if (streq(argv[1], "info")) {
		if (argc < 3) {
			kprintf("dev info: usage: dev info NAME\n");
			return (1);
		}
		return (dev_call_info(argv[2]));
	}
	if (streq(argv[1], "write")) {
		if (argc < 4) {
			kprintf("dev write: usage: dev write NAME TEXT\n");
			return (1);
		}
		return (dev_write(argv[2], argv[3]));
	}
	kprintf("dev: unknown subcommand '%s'\n", argv[1]);
	return (1);
}

/* ---- disk: BLOCK-device consumer using dev/disk* control ports ---- */

static int
disk_call_geom(const char *name)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header	hdr;
		struct dev_geom_reply	body;
	} reply;
	mach_port_name_t	svc;
	int			rv;

	svc = dev_lookup(name);
	if (svc == MACH_PORT_NULL) {
		kprintf("disk: '%s' not registered\n", name);
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = DEV_OP_GEOM;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply), 2000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("disk geom: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}
	if (reply.body.dgr_rv != MACH_MSG_OK) {
		kprintf("disk geom: drive rv=%d\n", reply.body.dgr_rv);
		return (1);
	}
	{
		uint64_t bytes;
		bytes = reply.body.dgr_total_sectors *
		    (uint64_t)reply.body.dgr_sector_bytes;
		kprintf("  %-8s  sector=%u total=%llu sectors (%llu MiB)  "
		    "ext=%s\n  model=%s\n",
		    name,
		    (unsigned)reply.body.dgr_sector_bytes,
		    (unsigned long long)reply.body.dgr_total_sectors,
		    (unsigned long long)(bytes >> 20),
		    (reply.body.dgr_flags & 1) ? "yes" : "no",
		    reply.body.dgr_model);
	}
	return (0);
}

static int
disk_ls(void)
{
	char	names[8][DEV_NAME_MAX];
	size_t	n, i, shown;

	n = dev_list_names(names, 8);
	shown = 0;
	for (i = 0; i < n; i++) {
		/* Only show dev/disk* entries. */
		if (names[i][0] != 'd' || names[i][1] != 'i' ||
		    names[i][2] != 's' || names[i][3] != 'k')
			continue;
		if (shown == 0)
			kprintf("disks:\n");
		(void)disk_call_geom(names[i]);
		shown++;
	}
	if (shown == 0)
		kprintf("disk: no block devices registered\n");
	return (0);
}

static int
parse_u64(const char *s, uint64_t *out)
{
	uint64_t	v;
	size_t		i;

	if (s == NULL || s[0] == '\0')
		return (-1);
	v = 0;
	for (i = 0; s[i] != '\0'; i++) {
		if (s[i] < '0' || s[i] > '9')
			return (-1);
		v = v * 10 + (uint64_t)(s[i] - '0');
	}
	*out = v;
	return (0);
}

static void
hexdump_sector(const uint8_t *data, size_t nbytes, uint64_t lba)
{
	size_t	i, j;

	for (i = 0; i < nbytes; i += 16) {
		kprintf("  %08llx  ",
		    (unsigned long long)(lba * 512 + i));
		for (j = 0; j < 16 && i + j < nbytes; j++)
			kprintf("%02x ", (unsigned)data[i + j]);
		kprintf(" ");
		for (j = 0; j < 16 && i + j < nbytes; j++) {
			uint8_t c = data[i + j];
			kprintf("%c", (c >= 32 && c < 127) ? c : '.');
		}
		kprintf("\n");
	}
}

static int
disk_read(const char *name, uint64_t lba)
{
	struct {
		struct mach_msg_header		hdr;
		struct dev_block_io_req		body;
	} req;
	struct {
		struct mach_msg_header		hdr;
		struct dev_block_read_reply	body;
	} reply;
	mach_port_name_t	svc;
	int			rv;

	svc = dev_lookup(name);
	if (svc == MACH_PORT_NULL) {
		kprintf("disk read: '%s' not registered\n", name);
		return (1);
	}

	req.body.dbr_lba   = lba;
	req.body.dbr_count = 1;
	req.body.dbr_pad   = 0;

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = DEV_OP_READ_BLOCK;

	rv = mach_msg_rpc(kernel_space, &req.hdr, &reply.hdr,
	    sizeof(reply), 5000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("disk read: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}
	if (reply.body.dbr_rv != MACH_MSG_OK) {
		kprintf("disk read: drive rv=%d\n", reply.body.dbr_rv);
		return (1);
	}

	kprintf("disk read %s lba=%llu (%u sector):\n", name,
	    (unsigned long long)lba, (unsigned)reply.body.dbr_count);
	hexdump_sector(reply.body.dbr_data,
	    (size_t)reply.body.dbr_count * 512, lba);
	return (0);
}

static int
disk_write(const char *name, uint64_t lba, const char *text)
{
	struct {
		struct mach_msg_header		hdr;
		struct dev_block_write_req	body;
	} req;
	struct {
		struct mach_msg_header		hdr;
		struct dev_block_io_reply	body;
	} reply;
	mach_port_name_t	svc;
	size_t			i, tlen;
	int			rv;

	svc = dev_lookup(name);
	if (svc == MACH_PORT_NULL) {
		kprintf("disk write: '%s' not registered\n", name);
		return (1);
	}

	req.body.dbw_lba   = lba;
	req.body.dbw_count = 1;
	req.body.dbw_pad   = 0;
	for (i = 0; i < DEV_BLOCK_MAX_BYTES; i++)
		req.body.dbw_data[i] = 0;
	for (tlen = 0; text[tlen] != '\0' && tlen < 511; tlen++)
		req.body.dbw_data[tlen] = (uint8_t)text[tlen];
	req.body.dbw_data[tlen] = '\n';

	req.hdr.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.hdr.msgh_size    = sizeof(req);
	req.hdr.msgh_remote  = svc;
	req.hdr.msgh_local   = MACH_PORT_NULL;
	req.hdr.msgh_voucher = 0;
	req.hdr.msgh_id      = DEV_OP_WRITE_BLOCK;

	rv = mach_msg_rpc(kernel_space, &req.hdr, &reply.hdr,
	    sizeof(reply), 5000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("disk write: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}
	if (reply.body.dbr_rv != MACH_MSG_OK) {
		kprintf("disk write: drive rv=%d\n", reply.body.dbr_rv);
		return (1);
	}
	kprintf("disk write %s lba=%llu: %u sector(s) committed\n",
	    name, (unsigned long long)lba,
	    (unsigned)reply.body.dbr_sectors);
	return (0);
}

static int
disk_sync(const char *name)
{
	struct mach_msg_header		req;
	struct {
		struct mach_msg_header		hdr;
		struct dev_block_io_reply	body;
	} reply;
	mach_port_name_t	svc;
	int			rv;

	svc = dev_lookup(name);
	if (svc == MACH_PORT_NULL) {
		kprintf("disk sync: '%s' not registered\n", name);
		return (1);
	}

	req.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	req.msgh_size    = sizeof(req);
	req.msgh_remote  = svc;
	req.msgh_local   = MACH_PORT_NULL;
	req.msgh_voucher = 0;
	req.msgh_id      = DEV_OP_SYNC;

	rv = mach_msg_rpc(kernel_space, &req, &reply.hdr, sizeof(reply), 5000);
	(void)port_deallocate(kernel_space, svc);
	if (rv != MACH_MSG_OK) {
		kprintf("disk sync: rpc rv=%s\n", mach_msg_strerror(rv));
		return (1);
	}
	if (reply.body.dbr_rv != MACH_MSG_OK) {
		kprintf("disk sync: drive rv=%d\n", reply.body.dbr_rv);
		return (1);
	}
	kprintf("disk sync %s: cache flushed\n", name);
	return (0);
}

static int
cmd_disk(int argc, char *argv[])
{
	uint64_t	lba;

	if (argc < 2 || streq(argv[1], "ls"))
		return (disk_ls());

	if (streq(argv[1], "info")) {
		if (argc < 3) {
			kprintf("disk info: usage: disk info NAME\n");
			return (1);
		}
		return (disk_call_geom(argv[2]));
	}
	if (streq(argv[1], "read")) {
		if (argc < 4 || parse_u64(argv[3], &lba) != 0) {
			kprintf("disk read: usage: disk read NAME LBA\n");
			return (1);
		}
		return (disk_read(argv[2], lba));
	}
	if (streq(argv[1], "write")) {
		if (argc < 5 || parse_u64(argv[3], &lba) != 0) {
			kprintf("disk write: usage: disk write NAME LBA TEXT\n");
			return (1);
		}
		return (disk_write(argv[2], lba, argv[4]));
	}
	if (streq(argv[1], "sync")) {
		if (argc < 3) {
			kprintf("disk sync: usage: disk sync NAME\n");
			return (1);
		}
		return (disk_sync(argv[2]));
	}
	kprintf("disk: unknown subcommand '%s'\n", argv[1]);
	return (1);
}
