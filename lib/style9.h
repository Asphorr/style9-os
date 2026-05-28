/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _STYLE9_H_
#define	_STYLE9_H_

/*
 * libstyle9 -- user-side runtime library.
 *
 * Every ring-3 program in this tree links against libstyle9.  It pulls
 * in the bare-types, the syscall stubs, the small handful of libc-shaped
 * helpers (string, memory, bump-allocator, printf), and the Mach IPC
 * wrappers that turn an integer syscall return into something a C
 * program wants to deal with.
 *
 * Layout mirrors the kernel's BSD-flavoured headers: one header per
 * library, all declarations centralised here so a user program writes
 * `#include <style9.h>` and gets the full surface.  Implementations are
 * sliced across lib/style9_*.c by topic.
 *
 * Calling convention:  the `_start` glue in crt0.S calls `main()`, then
 * `exit(rv)` with the return value.  Programs implement `int main(void)`.
 */

/* ---- bare types ----------------------------------------------------- */

typedef unsigned long		size_t;
typedef long			ssize_t;
typedef unsigned long		uintptr_t;
typedef long			intptr_t;

typedef unsigned char		uint8_t;
typedef unsigned short		uint16_t;
typedef unsigned int		uint32_t;
typedef unsigned long long	uint64_t;

typedef signed char		int8_t;
typedef short			int16_t;
typedef int			int32_t;
typedef long long		int64_t;

#define	NULL			((void *)0)

/* ---- syscall ABI (mirrors kern/syscall.h) -------------------------- */

#define	SYS_PRINT		0
#define	SYS_EXIT		1
#define	SYS_YIELD		2
#define	SYS_PORT_ALLOC		3
#define	SYS_PORT_DEALLOC	4
#define	SYS_MSG_SEND		5
#define	SYS_MSG_RECV		6
#define	SYS_MSG_RECV_TIMED	7
#define	SYS_MSG_RPC		8
#define	SYS_SPAWN		9	/* phase 1b */
#define	SYS_TASK_ALIVE		10	/* phase 2  */

#define	SYS_E_NOSYS		(-1)
#define	SYS_E_FAULT		(-2)
#define	SYS_E_INVAL		(-3)

/*
 * Raw `syscall` instruction wrappers.  Each variant takes the syscall
 * number plus the right number of arguments and returns the kernel's
 * rax value verbatim.  Higher-level wrappers (`write`, `mach_msg_send`,
 * ...) live below and turn these into something C-typed.
 */
long	syscall0(long nr);
long	syscall1(long nr, long a0);
long	syscall2(long nr, long a0, long a1);
long	syscall3(long nr, long a0, long a1, long a2);
long	syscall4(long nr, long a0, long a1, long a2, long a3);

/* ---- process ------------------------------------------------------- */

void	exit(int code) __attribute__((noreturn));
long	yield(void);

/*
 * Spawn the named program (looks up in the kernel's progreg).  Returns
 * the new task's id on success, or a negative SYS_E_* code.  Fire-and-
 * forget: combine with task_alive() to wait for the child.
 */
long	spawn(const char *name);

/*
 * task_alive: 1 if a task with this id is still running, 0 otherwise.
 * yield-spin around this to wait for a spawned child without baking in
 * a real notification path.
 */
int	task_alive(uint64_t task_id);

/* ---- I/O ----------------------------------------------------------- */

ssize_t	write(const char *buf, size_t len);
void	putchar(char c);
void	puts(const char *s);
int	printf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));

/* ---- string -------------------------------------------------------- */

size_t	strlen(const char *s);
int	strcmp(const char *a, const char *b);
int	strncmp(const char *a, const char *b, size_t n);
char	*strcpy(char *dst, const char *src);
char	*strncpy(char *dst, const char *src, size_t n);

/* ---- memory -------------------------------------------------------- */

void	*memcpy(void *dst, const void *src, size_t n);
void	*memset(void *dst, int c, size_t n);
int	memcmp(const void *a, const void *b, size_t n);

/*
 * Bump-allocator malloc.  Backed by a fixed-size .bss arena (4 KiB
 * per program by default; raise STYLE9_HEAP_BYTES at compile time to
 * extend it).  free() is a NOP -- callers that need real reuse should
 * track lifetimes themselves until libstyle9 grows a freeing allocator.
 */
void	*malloc(size_t n);
void	free(void *p);

/* ---- Mach ABI (mirrors mach/port.h) -------------------------------- */

typedef uint32_t		mach_port_name_t;

#define	MACH_PORT_NULL			((mach_port_name_t)0)
#define	MACH_PORT_DEAD			((mach_port_name_t)0xFFFFFFFFu)

#define	MACH_PORT_RIGHT_RECEIVE		0x01u
#define	MACH_PORT_RIGHT_SEND		0x02u
#define	MACH_PORT_RIGHT_SEND_ONCE	0x04u

#define	MACH_PORT_TASK_SELF		((mach_port_name_t)1)
#define	MACH_PORT_BOOTSTRAP		((mach_port_name_t)2)

#define	MACH_MSG_TYPE_MOVE_RECEIVE	16
#define	MACH_MSG_TYPE_MOVE_SEND		17
#define	MACH_MSG_TYPE_MOVE_SEND_ONCE	18
#define	MACH_MSG_TYPE_COPY_SEND		19
#define	MACH_MSG_TYPE_MAKE_SEND		20
#define	MACH_MSG_TYPE_MAKE_SEND_ONCE	21

#define	MACH_MSG_PORT_DESCRIPTOR	0
#define	MACH_MSG_OOL_DESCRIPTOR		1
#define	MACH_MSG_VIRTUAL_COPY		0
#define	MACH_MSG_PHYSICAL_COPY		1
#define	MACH_MSG_OOL_MAX_BYTES		(1u << 20)	/* 1 MiB             */
#define	MACH_MSGH_BITS_COMPLEX		0x80000000u
#define	MACH_MSGH_BITS(remote, local)	\
	(((uint32_t)(remote) & 0xFFu) | (((uint32_t)(local) & 0xFFu) << 8))

#define	MACH_MSG_OK			0
#define	MACH_E_INVAL			1
#define	MACH_E_NAME			2
#define	MACH_E_RIGHT			3
#define	MACH_E_DEAD			4
#define	MACH_E_NOSPACE			5
#define	MACH_E_NOMSG			6
#define	MACH_E_TOOSMALL			7
#define	MACH_E_NOMEM			8
#define	MACH_E_TIMEOUT			9

#define	MACH_TIMEOUT_NONE		((uint64_t)0)
#define	MACH_TIMEOUT_FOREVER		((uint64_t)~0ull)

struct mach_msg_header {
	uint32_t		msgh_bits;
	uint32_t		msgh_size;
	mach_port_name_t	msgh_remote;
	mach_port_name_t	msgh_local;
	uint32_t		msgh_voucher;
	uint32_t		msgh_id;
};

struct mach_msg_body {
	uint32_t		msgh_descriptor_count;
};

struct mach_msg_port_descriptor {
	uint8_t			type;		/* == MACH_MSG_PORT_DESCRIPTOR */
	uint8_t			disposition;
	uint8_t			pad1;
	uint8_t			pad2;
	mach_port_name_t	name;
};

/*
 * Packed to keep alignment at 4 instead of the 8 the uint64_t address
 * field would force; otherwise a 4-byte gap appears between body and
 * an OOL descriptor in a wire struct, which the kernel walker would
 * read as a bogus port descriptor.  See mach/port.h for the full
 * rationale.
 */
struct mach_msg_ool_descriptor {
	uint8_t			type;		/* == MACH_MSG_OOL_DESCRIPTOR */
	uint8_t			copy;		/* MACH_MSG_PHYSICAL_COPY     */
	uint8_t			deallocate;	/* reserved, set 0            */
	uint8_t			pad;
	uint32_t		size;		/* bytes to ferry              */
	uint64_t		address;	/* sender VA / receiver VA     */
} __attribute__((packed));

/* Op codes the kernel exposes today on its well-known ports. */
#define	TASK_OP_GET_INFO		1
#define	BOOTSTRAP_OP_LOOKUP		1
#define	BOOTSTRAP_REPLY_NOT_FOUND	0xFFFFFFFFu
#define	BOOTSTRAP_NAME_MAX		32

struct task_info_reply {
	uint64_t	tir_task_id;
	uint32_t	tir_nthreads;
	uint32_t	tir_pad;
	char		tir_name[32];
};

struct bootstrap_lookup_request {
	char	blr_name[BOOTSTRAP_NAME_MAX];
};

/* ---- Mach wrappers ------------------------------------------------- */

mach_port_name_t mach_port_allocate(uint8_t rights);
int		mach_port_deallocate(mach_port_name_t name);
int		mach_msg_send(const struct mach_msg_header *msg);
int		mach_msg_recv(mach_port_name_t name,
		    struct mach_msg_header *buf, size_t buf_size);
int		mach_msg_recv_timed(mach_port_name_t name,
		    struct mach_msg_header *buf, size_t buf_size,
		    uint64_t timeout_ms);
int		mach_msg_rpc(struct mach_msg_header *req,
		    struct mach_msg_header *reply, size_t reply_size,
		    uint64_t timeout_ms);

/*
 * One-shot bootstrap lookup helper.  Builds the request, RPCs it to
 * MACH_PORT_BOOTSTRAP, returns the SEND name the kernel handed back
 * in our space.  Returns MACH_PORT_NULL if the service is not
 * registered (or any error occurred).  The caller must
 * mach_port_deallocate() the returned name when done.
 */
mach_port_name_t bootstrap_lookup(const char *service);

/* ---- dev/NAME generic-driver protocol (mirrors dev/dev_proto.h) --- */

#define	DEV_OP_INFO		1
#define	DEV_OP_OPEN_STREAM	2
#define	DEV_OP_WRITE		3

#define	DEV_KIND_NONE		0
#define	DEV_KIND_STREAM_RX	1
#define	DEV_KIND_STREAM_TX	2
#define	DEV_KIND_CHAR		3
#define	DEV_KIND_BLOCK		4

#define	DEV_F_READABLE		0x01u
#define	DEV_F_WRITABLE		0x02u
#define	DEV_F_STREAM		0x04u

#define	DEV_NAME_MAX		16
#define	DEV_WRITE_MAX		256

struct dev_info_reply {
	char		dir_name[DEV_NAME_MAX];
	uint32_t	dir_kind;
	uint32_t	dir_flags;
};

struct dev_write_request {
	uint32_t	dwr_len;
	uint32_t	dwr_pad;
	uint8_t		dwr_data[DEV_WRITE_MAX];
};

struct dev_write_reply {
	int32_t		dwr_rv;
	uint32_t	dwr_written;
};

/*
 * dev_open_stream: bootstrap_lookup("dev/<name>") + RPC DEV_OP_OPEN_STREAM
 * + extract the port_descriptor + deallocate the control port.  Returns
 * the SEND right naming the driver's stream port, or MACH_PORT_NULL on
 * any failure.  Caller mach_msg_recv()s on the returned name to pull
 * bytes from the driver.  Caller must mach_port_deallocate() when done.
 */
mach_port_name_t dev_open_stream(const char *short_name);

/*
 * dev_info: query the named driver for its kind + capability flags.
 * Returns MACH_MSG_OK on success and fills *out; any negative MACH_E_*
 * otherwise.  Lightweight probe before opening a stream.
 */
int		 dev_info(const char *short_name, struct dev_info_reply *out);

/*
 * dev_write: send DEV_OP_WRITE to the driver named "dev/<short_name>".
 * Up to DEV_WRITE_MAX bytes; returns the number of bytes the driver
 * acknowledged, or a negative MACH_E_*.  Used by future TX consumers
 * (e.g. a network logger speaking dev/uart).
 */
ssize_t		 dev_write(const char *short_name,
		    const void *buf, size_t len);

/* ---- kernel-side services (mirrors mach/services.h) --------------- */

#define	SVC_CLOCK_NAME		"clock"
#define	CLOCK_OP_GET		1

struct svc_clock_reply {
	uint64_t	cr_uptime_ms;
	uint64_t	cr_uptime_us;
	uint64_t	cr_ticks;
};

#define	SVC_STATS_NAME		"stats"
#define	STATS_OP_GET		1

struct svc_stats_reply {
	uint64_t	sr_pmm_used_pages;
	uint64_t	sr_kmem_cached_pages;
	uint64_t	sr_kernel_inuse;
	uint64_t	sr_task_count;
	uint64_t	sr_thread_count;
	uint64_t	sr_ctx_switches;
	uint64_t	sr_pmm_total_pages;
};

#define	SVC_TASKS_NAME		"tasks"
#define	TASKS_OP_LIST		1
#define	SVC_TASKS_MAX		16
#define	SVC_TASKS_NAME_MAX	24

struct svc_tasks_entry {
	uint64_t	te_task_id;
	uint32_t	te_nthreads;
	uint32_t	te_pad;
	char		te_name[SVC_TASKS_NAME_MAX];
};

struct svc_tasks_reply {
	uint32_t		tr_count;
	uint32_t		tr_pad;
	struct svc_tasks_entry	tr_entries[SVC_TASKS_MAX];
};

#endif /* !_STYLE9_H_ */
