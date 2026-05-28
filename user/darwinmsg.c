/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * darwinmsg -- a freestanding ring-3 program that does a real mach_msg()
 * round-trip the Darwin way (S3 of the XNU-binary ladder).  It links against
 * no libc and no crt0: _start issues genuine Apple class-encoded `syscall`s
 * by hand, and defines its OWN mach_msg_header_t -- whose 24-byte layout is
 * byte-exact with the kernel's struct mach_msg_header, which is the whole
 * point of "binary-exact".
 *
 * It allocates a reply port (mach_reply_port, a Mach trap), builds a tiny
 * inline message addressed to that port, and issues the classic combined
 * mach_msg_trap with MACH_SEND_MSG | MACH_RCV_MSG -- send to the port, then
 * receive from it -- so one trap drives a real trip through the kernel's
 * message queue.  Success is the kern_return_t coming back KERN_SUCCESS and
 * msgh_id surviving the queue unchanged.
 *
 * elf2macho's `macos` mode wraps this ELF as a Mach-O carrying an
 * LC_BUILD_VERSION naming PLATFORM_MACOS, so macho_load tags the task
 * TASK_PERSONALITY_DARWIN and the kernel routes mach_msg through
 * darwin_dispatch (kern/darwin.c).
 */

#include <stddef.h>
#include <stdint.h>

/* Darwin class-encoded syscall numbers (high byte = class). */
#define	SYS_write		0x2000004UL	/* class 2, BSD nr 4   */
#define	SYS_exit		0x2000001UL	/* class 2, BSD nr 1   */
#define	MACH_reply_port		0x100001AUL	/* class 1, trap 26    */
#define	MACH_msg		0x100001FUL	/* class 1, trap 31    */

/* mach_msg options + dispositions (Darwin <mach/message.h>). */
#define	MACH_SEND_MSG		0x00000001U
#define	MACH_RCV_MSG		0x00000002U
#define	MACH_MSG_SUCCESS	0
#define	MACH_MSG_TYPE_COPY_SEND	19
#define	MACH_MSGH_BITS(r, l)	((uint32_t)(((r) & 0xFFU) | (((l) & 0xFFU) << 8)))

#define	MAGIC_ID		0x4D534733	/* 'M','S','G','3' round-trip tag */

/*
 * Darwin mach_msg_header_t -- the binary-exact 24-byte layout the kernel's
 * struct mach_msg_header mirrors (bits@0, size@4, remote@8, local@12,
 * voucher@16, id@20).
 */
typedef struct {
	uint32_t	msgh_bits;
	uint32_t	msgh_size;
	uint32_t	msgh_remote_port;
	uint32_t	msgh_local_port;
	uint32_t	msgh_voucher_port;
	int32_t		msgh_id;
} mach_msg_header_t;

static const char banner[] =
    "darwinmsg: Darwin task doing a real mach_msg() round-trip on its own port\n";
static const char okmsg[] =
    "darwinmsg: mach_msg send+recv OK -- KERN_SUCCESS, msgh_id survived the queue\n";
static const char bugmsg[] =
    "darwinmsg: BUG -- mach_msg round-trip failed or msgh_id mismatch\n";

/* Raw Darwin syscalls via the `syscall` instruction (SysV/Darwin convention). */
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
dwrite(const char *s, unsigned long n)
{

	(void)dsys3(SYS_write, 1, (long)s, (long)n);
}

__attribute__((noreturn))
void
_start(void)
{
	mach_msg_header_t	m;
	unsigned int		p;
	long			kr;

	dwrite(banner, sizeof(banner) - 1);

	/* A reply port carries both a receive and a send right in this space. */
	p = (unsigned int)dsys0(MACH_reply_port);

	m.msgh_bits         = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	m.msgh_size         = sizeof(m);
	m.msgh_remote_port  = p;
	m.msgh_local_port   = 0;
	m.msgh_voucher_port = 0;
	m.msgh_id           = MAGIC_ID;

	/* Classic combined mach_msg: send to p, then receive from p. */
	kr = dmachmsg(&m, MACH_SEND_MSG | MACH_RCV_MSG, sizeof(m), sizeof(m),
	    p, 0);

	if (kr == MACH_MSG_SUCCESS && m.msgh_id == MAGIC_ID)
		dwrite(okmsg, sizeof(okmsg) - 1);
	else
		dwrite(bugmsg, sizeof(bugmsg) - 1);

	(void)dsys3(SYS_exit, 0, 0, 0);
	__builtin_unreachable();
}
