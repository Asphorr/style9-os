/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * darwinmsg -- a freestanding ring-3 program that does mach_msg() the
 * Darwin way.  No libc, no crt0: _start issues Apple class-encoded
 * `syscall's by hand and defines its own mach_msg_header_t, whose 24-byte
 * layout must match the kernel's struct mach_msg_header byte for byte.
 *
 * It allocates a reply port (the mach_reply_port trap) and sends to it,
 * checking what XNU's receive would hand back: the header turned round
 * with the trailer after it, a reply through the send-once right its
 * request carried, a buffer with no room for the trailer, a reply
 * disposition with no reply port.  Then a trap the kernel lacks, and a
 * complex send, which is refused.
 *
 * elf2macho's `macos' mode tags it for PLATFORM_MACOS, so the task is
 * TASK_PERSONALITY_DARWIN and mach_msg goes through darwin_dispatch
 * (kern/darwin.c).
 */

#include <stddef.h>
#include <stdint.h>

/* Darwin class-encoded syscall numbers (high byte = class). */
#define	SYS_write		0x2000004UL	/* class 2, BSD nr 4   */
#define	SYS_exit		0x2000001UL	/* class 2, BSD nr 1   */
#define	MACH_vm_allocate	0x100000AUL	/* class 1, trap 10    */
#define	MACH_reply_port		0x100001AUL	/* class 1, trap 26    */
#define	MACH_msg		0x100001FUL	/* class 1, trap 31    */

#define	KERN_INVALID_ARGUMENT	4

/* mach_msg options, returns and dispositions (Darwin <mach/message.h>). */
#define	MACH_SEND_MSG		0x00000001U
#define	MACH_RCV_MSG		0x00000002U
#define	MACH_SEND_TIMEOUT	0x00000010U
#define	MACH_RCV_TIMEOUT	0x00000100U
#define	MACH_RCV_TRAILER_ELEMENTS(n)	((uint32_t)((n) & 0xFU) << 24)
#define	MACH_MSG_SUCCESS	0
#define	MACH_SEND_INVALID_DEST	0x10000003
#define	MACH_SEND_INVALID_TYPE	0x1000000F
#define	MACH_RCV_TIMED_OUT	0x10004003
#define	MACH_RCV_TOO_LARGE	0x10004004
#define	MACH_RCV_INVALID_TRAILER	0x1000400F
#define	MACH_MSG_TYPE_PORT_SEND		17	/* rights as received */
#define	MACH_MSG_TYPE_PORT_SEND_ONCE	18
#define	MACH_MSG_TYPE_MOVE_SEND_ONCE	18
#define	MACH_MSG_TYPE_COPY_SEND		19
#define	MACH_MSG_TYPE_MAKE_SEND_ONCE	21
#define	MACH_MSGH_BITS_COMPLEX	0x80000000U
#define	MACH_MSGH_BITS(r, l)	((uint32_t)(((r) & 0xFFU) | (((l) & 0xFFU) << 8)))

#define	MAGIC_ID		0x4D534733	/* 'M','S','G','3' round-trip tag */
#define	REPLY_ID		0x4D534734

/*
 * Darwin mach_msg_header_t, 24 bytes, mirrored by the kernel's struct
 * mach_msg_header (bits@0, size@4, remote@8, local@12, voucher@16, id@20).
 */
typedef struct {
	uint32_t	msgh_bits;
	uint32_t	msgh_size;
	uint32_t	msgh_remote_port;
	uint32_t	msgh_local_port;
	uint32_t	msgh_voucher_port;
	int32_t		msgh_id;
} mach_msg_header_t;

typedef struct {
	uint32_t	msgh_trailer_type;
	uint32_t	msgh_trailer_size;
} mach_msg_trailer_t;

/* A bare message as its receiver gets it: the header, then the trailer. */
typedef struct {
	mach_msg_header_t	h;
	mach_msg_trailer_t	t;
} rcv_msg_t;

static const char banner[] =
    "darwinmsg: Darwin task doing mach_msg() on its own port\n";
static const char pass_trip[] =
    "darwinmsg: PASS a round trip on its own port comes back as XNU "
    "gives it: the header turned round, the 8-byte trailer after\n";
static const char fail_trip[] =
    "darwinmsg: FAIL a round trip on its own port did not come back as "
    "XNU gives it\n";
static const char pass_reply[] =
    "darwinmsg: PASS a reply goes back through the send-once right its "
    "request carried, and only once\n";
static const char fail_reply[] =
    "darwinmsg: FAIL a reply through the request's send-once right went "
    "wrong\n";
static const char pass_large[] =
    "darwinmsg: PASS no room for the trailer is MACH_RCV_TOO_LARGE and "
    "the message is gone; a trailer with elements is refused\n";
static const char fail_large[] =
    "darwinmsg: FAIL a buffer with no room for the trailer, or a trailer "
    "with elements, was taken\n";
static const char pass_noreply[] =
    "darwinmsg: PASS a reply disposition with no reply port carries no "
    "right\n";
static const char fail_noreply[] =
    "darwinmsg: FAIL a reply disposition with no reply port came out as "
    "a right\n";
static const char pass_trap[] =
    "darwinmsg: PASS a trap the kernel lacks answers "
    "KERN_INVALID_ARGUMENT, not success\n";
static const char fail_trap[] =
    "darwinmsg: FAIL a missing trap claimed to succeed\n";
static const char pass_cplx[] =
    "darwinmsg: PASS a complex send is refused and nothing is queued\n";
static const char fail_cplx[] =
    "darwinmsg: FAIL a complex send was not refused cleanly\n";

static int	failures;

/* Raw Darwin syscalls via the `syscall' instruction. */
static inline long
dsys0(unsigned long nr)
{
	long	ret;

	__asm__ __volatile__("syscall"
	    : "=a"(ret)
	    : "a"(nr)
	    : "rcx", "r11", "memory");
	return (ret);
}

static inline long
dsys3(unsigned long nr, long a0, long a1, long a2)
{
	long	ret;

	__asm__ __volatile__("syscall"
	    : "=a"(ret)
	    : "a"(nr), "D"(a0), "S"(a1), "d"(a2)
	    : "rcx", "r11", "memory");
	return (ret);
}

static inline long
dmachmsg(void *msg, unsigned long option, unsigned long send_size,
    unsigned long rcv_size, unsigned int rcv_name, unsigned long timeout)
{
	register long	r10 __asm__("r10") = (long)rcv_size;
	register long	r8  __asm__("r8")  = (long)rcv_name;
	register long	r9  __asm__("r9")  = (long)timeout;
	long		ret;

	__asm__ __volatile__("syscall"
	    : "=a"(ret)
	    : "a"(MACH_msg), "D"(msg), "S"(option), "d"(send_size),
	      "r"(r10), "r"(r8), "r"(r9)
	    : "rcx", "r11", "memory");
	return (ret);
}

static void
say(const char *s)
{
	unsigned long	n;

	for (n = 0; s[n] != '\0'; n++)
		continue;
	(void)dsys3(SYS_write, 1, (long)s, (long)n);
}

static void
check(int ok, const char *pass, const char *fail)
{

	say(ok ? pass : fail);
	if (!ok)
		failures++;
}

/* A bare message from nobody in particular to `to`. */
static void
bare(rcv_msg_t *m, uint32_t bits, unsigned int to, unsigned int reply,
    int32_t id)
{

	m->h.msgh_bits         = bits;
	m->h.msgh_size         = sizeof(m->h);
	m->h.msgh_remote_port  = to;
	m->h.msgh_local_port   = reply;
	m->h.msgh_voucher_port = 0;
	m->h.msgh_id           = id;
	m->t.msgh_trailer_type = 0xDEAD;
	m->t.msgh_trailer_size = 0xDEAD;
}

__attribute__((noreturn))
void
_start(void)
{
	rcv_msg_t	m;
	unsigned int	p;
	unsigned int	r;
	long		kr;
	int		ok;

	say(banner);

	/* A reply port carries both a receive and a send right in this space. */
	p = (unsigned int)dsys0(MACH_reply_port);

	/*
	 * 1. A combined send and receive on p.  What comes back names the
	 *    port it came in on in msgh_local, no reply right, and the
	 *    right it came through as a PORT_* type; the trailer follows.
	 */
	bare(&m, MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), p, 0, MAGIC_ID);
	kr = dmachmsg(&m, MACH_SEND_MSG | MACH_SEND_TIMEOUT | MACH_RCV_MSG,
	    sizeof(m.h), sizeof(m), p, 0);
	check(kr == MACH_MSG_SUCCESS && m.h.msgh_id == MAGIC_ID &&
	    m.h.msgh_size == sizeof(m.h) &&
	    m.h.msgh_bits == MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND) &&
	    m.h.msgh_remote_port == 0 && m.h.msgh_local_port == p &&
	    m.t.msgh_trailer_type == 0 && m.t.msgh_trailer_size == 8,
	    pass_trip, fail_trip);

	/*
	 * 2. The MIG shape: the request carries a send-once right made from
	 *    p's receive right, and arrives with it in msgh_remote.  The
	 *    reply goes back through it, which uses it up.
	 */
	bare(&m, MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    MACH_MSG_TYPE_MAKE_SEND_ONCE), p, p, MAGIC_ID);
	kr = dmachmsg(&m, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(m.h),
	    sizeof(m), p, 0);
	r = m.h.msgh_remote_port;
	ok = kr == MACH_MSG_SUCCESS && r != 0 &&
	    m.h.msgh_bits == MACH_MSGH_BITS(MACH_MSG_TYPE_PORT_SEND_ONCE,
	    MACH_MSG_TYPE_PORT_SEND);
	if (ok) {
		bare(&m, MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0), r, 0,
		    REPLY_ID);
		kr = dmachmsg(&m, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(m.h),
		    sizeof(m), p, 0);
		ok = kr == MACH_MSG_SUCCESS && m.h.msgh_id == REPLY_ID &&
		    m.h.msgh_bits == MACH_MSGH_BITS(0,
		    MACH_MSG_TYPE_PORT_SEND_ONCE);
	}
	if (ok) {
		bare(&m, MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND_ONCE, 0), r, 0,
		    REPLY_ID);
		ok = dmachmsg(&m, MACH_SEND_MSG, sizeof(m.h), 0, 0, 0) ==
		    MACH_SEND_INVALID_DEST;
	}
	check(ok, pass_reply, fail_reply);

	/*
	 * 3. A buffer that holds the message but not the trailer: too
	 *    large, and the message is destroyed rather than left queued.
	 *    A trailer with elements is refused, not filled with guesses.
	 */
	bare(&m, MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), p, 0, MAGIC_ID);
	ok = dmachmsg(&m, MACH_SEND_MSG, sizeof(m.h), 0, 0, 0) ==
	    MACH_MSG_SUCCESS &&
	    dmachmsg(&m, MACH_RCV_MSG, 0, sizeof(m.h), p, 0) ==
	    MACH_RCV_TOO_LARGE &&
	    dmachmsg(&m, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(m), p,
	    0) == MACH_RCV_TIMED_OUT &&
	    dmachmsg(&m, MACH_RCV_MSG | MACH_RCV_TIMEOUT |
	    MACH_RCV_TRAILER_ELEMENTS(3), 0, sizeof(m), p, 0) ==
	    MACH_RCV_INVALID_TRAILER;
	check(ok, pass_large, fail_large);

	/*
	 * 4. A reply disposition with no reply port names no right: none
	 *    arrives, and the kernel must not go looking for one.
	 */
	bare(&m, MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND,
	    MACH_MSG_TYPE_MAKE_SEND_ONCE), p, 0, MAGIC_ID);
	kr = dmachmsg(&m, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(m.h),
	    sizeof(m), p, 0);
	check(kr == MACH_MSG_SUCCESS && m.h.msgh_remote_port == 0 &&
	    m.h.msgh_bits == MACH_MSGH_BITS(0, MACH_MSG_TYPE_PORT_SEND),
	    pass_noreply, fail_noreply);

	/* mach_vm_allocate is not a trap here; it must not say it worked. */
	check(dsys0(MACH_vm_allocate) == KERN_INVALID_ARGUMENT, pass_trap,
	    fail_trap);

	/* A complex send is refused before it reaches p's queue. */
	bare(&m, MACH_MSGH_BITS_COMPLEX |
	    MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0), p, 0, MAGIC_ID);
	kr = dmachmsg(&m, MACH_SEND_MSG, sizeof(m.h), 0, 0, 0);
	check(kr == MACH_SEND_INVALID_TYPE &&
	    dmachmsg(&m, MACH_RCV_MSG | MACH_RCV_TIMEOUT, 0, sizeof(m), p,
	    0) == MACH_RCV_TIMED_OUT, pass_cplx, fail_cplx);

	(void)dsys3(SYS_exit, failures != 0, 0, 0);
	__builtin_unreachable();
}
