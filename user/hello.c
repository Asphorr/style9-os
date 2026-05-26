/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * Ring-3 demo for style9-os.
 *
 *	1. Print a hello banner via SYS_PRINT.
 *	2. Allocate a Mach port (RECEIVE | SEND) via SYS_PORT_ALLOC.
 *	3. Send a self-message carrying a tagged msgh_id via SYS_MSG_SEND.
 *	4. Receive it via SYS_MSG_RECV (blocking recv on the same port).
 *	5. Print confirmation that the round-trip succeeded.
 *	6. Probe the new timeout path: SYS_MSG_RECV_TIMED on an empty port
 *	   with a 50 ms deadline, expect an error return (E_TIMEOUT).
 *	7. Issue a TASK_OP_GET_INFO RPC to MACH_PORT_TASK_SELF, print the
 *	   task id and name the kernel returns.
 *	8. SYS_PORT_DEALLOC + SYS_EXIT.
 *
 * This exercises the full IPC plumbing across the user/kernel boundary:
 * port_alloc + mach_msg_send + mach_msg_recv_block + mach_msg_recv_timed
 * + mach_msg_rpc, all from ring 3.
 */

typedef unsigned long		size_t;
typedef long			ssize_t;
typedef unsigned int		uint32_t;
typedef unsigned char		uint8_t;

/* Must agree with kern/syscall.h. */
#define	SYS_PRINT		0
#define	SYS_EXIT		1
#define	SYS_YIELD		2
#define	SYS_PORT_ALLOC		3
#define	SYS_PORT_DEALLOC	4
#define	SYS_MSG_SEND		5
#define	SYS_MSG_RECV		6
#define	SYS_MSG_RECV_TIMED	7
#define	SYS_MSG_RPC		8

/* MACH_E_TIMEOUT from kern/port.h -- recv_timed returns this on deadline. */
#define	MACH_E_TIMEOUT		9

/* Well-known name + op codes for the task_self port (kern/port.h). */
#define	MACH_PORT_TASK_SELF	((uint32_t)1)
#define	TASK_OP_GET_INFO	1

/* Mirror of struct task_info_reply -- 48 bytes following the header. */
struct task_info_reply {
	unsigned long long	tir_task_id;
	uint32_t		tir_nthreads;
	uint32_t		tir_pad;
	char			tir_name[32];
};

/* Must agree with kern/port.h. */
#define	MACH_PORT_NULL			((uint32_t)0)
#define	MACH_PORT_RIGHT_RECEIVE		0x01u
#define	MACH_PORT_RIGHT_SEND		0x02u

#define	MACH_MSG_TYPE_COPY_SEND		19
#define	MACH_MSGH_BITS(remote, local)	\
	(((uint32_t)(remote) & 0xFFu) | (((uint32_t)(local) & 0xFFu) << 8))

struct mach_msg_header {
	uint32_t	msgh_bits;
	uint32_t	msgh_size;
	uint32_t	msgh_remote;
	uint32_t	msgh_local;
	uint32_t	msgh_voucher;
	uint32_t	msgh_id;
};

/* ---- syscall wrappers ------------------------------------------------- */

static long
syscall1(long nr, long a0)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0)
	    : "rcx", "r11", "memory");
	return (ret);
}

static long
syscall2(long nr, long a0, long a1)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1)
	    : "rcx", "r11", "memory");
	return (ret);
}

static long
syscall3(long nr, long a0, long a1, long a2)
{
	long	ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1), "d"(a2)
	    : "rcx", "r11", "memory");
	return (ret);
}

static long
syscall4(long nr, long a0, long a1, long a2, long a3)
{
	register long	r10 __asm__("r10") = a3;
	long		ret;

	__asm__ __volatile__ ("syscall"
	    : "=a"(ret)
	    : "0"(nr), "D"(a0), "S"(a1), "d"(a2), "r"(r10)
	    : "rcx", "r11", "memory");
	return (ret);
}

/* ---- libc-shaped helpers --------------------------------------------- */

static long
write1(const char *s, size_t n)
{

	return (syscall2(SYS_PRINT, (long)s, (long)n));
}

static size_t
strlen(const char *s)
{
	size_t	n;

	n = 0;
	while (s[n] != '\0')
		n++;
	return (n);
}

static void
puts1(const char *s)
{

	(void)write1(s, strlen(s));
}

/* Tiny hex printer: writes 4 bytes of an msgh_id as "0xXXXXXXXX\n". */
static void
print_id(const char *prefix, uint32_t id)
{
	char		buf[32];
	const char	hex[] = "0123456789ABCDEF";
	size_t		i;
	size_t		p;

	p = 0;
	while (prefix[p] != '\0') {
		buf[p] = prefix[p];
		p++;
	}
	buf[p++] = '0';
	buf[p++] = 'x';
	for (i = 0; i < 8; i++) {
		buf[p++] = hex[(id >> ((7 - i) * 4)) & 0xFu];
	}
	buf[p++] = '\n';
	(void)write1(buf, p);
}

/* ---- demo ------------------------------------------------------------ */

#define	DEMO_TAG	0xCAFEBABEu

__attribute__((noreturn))
void
_start(void)
{
	struct mach_msg_header	tx;
	struct mach_msg_header	rx;
	long			name_or_err;
	long			rv;
	uint32_t		name;

	puts1("hello from hello.elf (loaded by kernel ELF parser, ring 3)\n");

	name_or_err = syscall1(SYS_PORT_ALLOC,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (name_or_err <= 0) {
		puts1("  port_alloc failed\n");
		syscall1(SYS_EXIT, 1);
	}
	name = (uint32_t)name_or_err;
	print_id("  allocated port = ", name);

	tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	tx.msgh_size    = sizeof(tx);
	tx.msgh_remote  = name;
	tx.msgh_local   = MACH_PORT_NULL;
	tx.msgh_voucher = 0;
	tx.msgh_id      = DEMO_TAG;

	rv = syscall1(SYS_MSG_SEND, (long)&tx);
	if (rv != 0) {
		puts1("  msg_send failed\n");
		syscall1(SYS_EXIT, 2);
	}
	puts1("  self-send queued\n");

	rv = syscall3(SYS_MSG_RECV, (long)name, (long)&rx, (long)sizeof(rx));
	if (rv != 0) {
		puts1("  msg_recv failed\n");
		syscall1(SYS_EXIT, 3);
	}
	print_id("  recv'd msgh_id = ", rx.msgh_id);

	if (rx.msgh_id != DEMO_TAG) {
		puts1("  TAG MISMATCH -- IPC corrupted\n");
		syscall1(SYS_EXIT, 4);
	}
	puts1("  mach_msg round-trip via SYSCALL: OK\n");

	/*
	 * Step 6: timeout probe.  Empty port (we just drained it above),
	 * 50 ms deadline -- recv_timed must return a non-zero error (the
	 * kernel surfaces MACH_E_TIMEOUT).  We don't insist on the exact
	 * code from ring 3, only that the call returns rather than parks
	 * forever; the kernel-side stress_rpc test asserts the precise
	 * error code.
	 */
	rv = syscall4(SYS_MSG_RECV_TIMED, (long)name, (long)&rx,
	    (long)sizeof(rx), 50);
	if (rv == 0) {
		puts1("  recv_timed unexpectedly returned a message\n");
		syscall1(SYS_EXIT, 5);
	}
	if (rv != MACH_E_TIMEOUT) {
		print_id("  recv_timed odd rv = ", (uint32_t)rv);
	} else {
		puts1("  recv_timed returned E_TIMEOUT after 50 ms: OK\n");
	}

	/*
	 * Step 7: task_self RPC.  msgh_id = TASK_OP_GET_INFO, the kernel
	 * synchronously fills a 48-byte task_info_reply right after the
	 * mach header in our recv buffer.  Reuses `rx` for the reply.
	 */
	{
		struct task_info_reply	*info;
		struct {
			struct mach_msg_header	hdr;
			struct task_info_reply	body;
		} reply_pkt;
		size_t			 i;

		tx.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		tx.msgh_size    = sizeof(tx);
		tx.msgh_remote  = MACH_PORT_TASK_SELF;
		tx.msgh_local   = MACH_PORT_NULL;	/* rpc fills in */
		tx.msgh_voucher = 0;
		tx.msgh_id      = TASK_OP_GET_INFO;

		rv = syscall4(SYS_MSG_RPC, (long)&tx, (long)&reply_pkt,
		    (long)sizeof(reply_pkt), (long)1000);
		if (rv != 0) {
			puts1("  task_self GET_INFO rpc failed\n");
			print_id("    rv = ", (uint32_t)rv);
			syscall1(SYS_EXIT, 6);
		}

		info = &reply_pkt.body;
		puts1("  task_self GET_INFO ok: task name='");
		for (i = 0; i < sizeof(info->tir_name) &&
		    info->tir_name[i] != '\0'; i++) {
			char ch = info->tir_name[i];
			(void)write1(&ch, 1);
		}
		print_id("' tir_task_id = ", (uint32_t)info->tir_task_id);
	}

	(void)syscall1(SYS_PORT_DEALLOC, (long)name);

	syscall1(SYS_EXIT, 0);
	for (;;)
		;
}
