/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "kmem.h"
#include "kprintf.h"
#include "memmap.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "shell.h"
#include "stress.h"
#include "task.h"
#include "thread.h"
#include "tty.h"

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
