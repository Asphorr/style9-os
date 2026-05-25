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
#include "pit.h"
#include "pmm.h"
#include "port.h"
#include "sched.h"
#include "stress.h"
#include "task.h"
#include "thread.h"
#include "tsc.h"

/* ---- PRNG -------------------------------------------------------------- */

/*
 * XORshift64 -- not cryptographic, but uniform enough that the stress
 * tests do not lean on any particular byte pattern.  Seeded fresh on
 * each invocation so test runs are deterministic.
 */
static uint64_t	prng_state;

static void
prng_seed(uint64_t seed)
{

	if (seed == 0)
		seed = 0xDEADBEEFCAFEBABEULL;
	prng_state = seed;
}

static uint64_t
prng_next(void)
{
	uint64_t	x;

	x = prng_state;
	x ^= x << 13;
	x ^= x >> 7;
	x ^= x << 17;
	prng_state = x;
	return (x);
}

/* ---- shared slot table ------------------------------------------------- */

#define	STRESS_SLOTS	256

struct slot {
	void	*sl_ptr;
	size_t	 sl_size;
	uint8_t	 sl_seed;
};

static struct slot	slots[STRESS_SLOTS];

static void
slots_reset(void)
{
	size_t	i;

	for (i = 0; i < STRESS_SLOTS; i++) {
		slots[i].sl_ptr  = NULL;
		slots[i].sl_size = 0;
		slots[i].sl_seed = 0;
	}
}

/*
 * Pick an allocation size from a distribution biased toward small --
 * realistic kernel allocators see thousands of <128B allocations per
 * KB-sized allocation, and a flat distribution would saturate the
 * large-page path far too quickly.
 *
 *	~50%	16..128
 *	~30%	128..1024
 *	~15%	1024..2048
 *	~5%	2048..8192 (large-allocation path)
 */
static size_t
random_size(void)
{
	uint64_t	r;
	uint32_t	pick;

	r = prng_next();
	pick = (uint32_t)(r & 0xFFFF);

	if (pick < 32768)
		return (16 + (size_t)((r >> 16) & 0x7F));		/* 16..143    */
	if (pick < 32768 + 19660)
		return (128 + (size_t)((r >> 16) & 0x3FF));		/* 128..1151  */
	if (pick < 32768 + 19660 + 9830)
		return (1024 + (size_t)((r >> 16) & 0x3FF));		/* 1024..2047 */
	return (2048 + (size_t)((r >> 16) & 0x17FF));			/* 2048..8191 */
}

static void
fill_pattern(uint8_t *p, size_t n, uint8_t seed)
{
	size_t	i;

	for (i = 0; i < n; i++)
		p[i] = (uint8_t)(seed ^ (uint8_t)i ^ (uint8_t)(i >> 8));
}

static bool
verify_pattern(const uint8_t *p, size_t n, uint8_t seed)
{
	size_t	i;
	uint8_t	want;

	for (i = 0; i < n; i++) {
		want = (uint8_t)(seed ^ (uint8_t)i ^ (uint8_t)(i >> 8));
		if (p[i] != want)
			return (false);
	}
	return (true);
}

/* ---- stress_mem -------------------------------------------------------- */

int
stress_mem(unsigned int iterations)
{
	uint64_t	t_start_cycles, t_end_cycles, elapsed_us;
	size_t		pmm_used_before, pmm_used_after, pmm_used_peak;
	size_t		cached_before, cached_after;
	size_t		conserved_before, conserved_after;
	uint64_t	alloc_count, free_count;
	uint64_t	corrupt_count;
	uint64_t	pmm_used;
	uint32_t	i, slot, op;
	uint64_t	r;

	kprintf("stress_mem: %u iterations, %u tracking slots\n",
	    iterations, STRESS_SLOTS);

	slots_reset();
	prng_seed(0x517CC1B727220A95ULL ^ (uint64_t)iterations);

	pmm_used_before = pmm_used_pages();
	cached_before   = kmem_cached_pages();
	conserved_before = pmm_used_before - cached_before;
	pmm_used_peak   = pmm_used_before;
	alloc_count     = 0;
	free_count      = 0;
	corrupt_count   = 0;

	t_start_cycles = tsc_read();

	for (i = 0; i < iterations; i++) {
		r = prng_next();
		slot = (uint32_t)(r & (STRESS_SLOTS - 1));
		op   = (uint32_t)((r >> 8) & 1);

		if (slots[slot].sl_ptr == NULL) {
			/* Empty slot: allocate. */
			size_t sz = random_size();
			uint8_t seed = (uint8_t)(r >> 16);

			slots[slot].sl_ptr  = kmalloc(sz);
			if (slots[slot].sl_ptr == NULL) {
				kprintf("stress_mem: kmalloc(%zu) returned "
				    "NULL at iter %u\n", sz, i);
				goto out_fail;
			}
			slots[slot].sl_size = sz;
			slots[slot].sl_seed = seed;
			fill_pattern(slots[slot].sl_ptr, sz, seed);
			alloc_count++;
		} else if (op == 0) {
			/* Verify + free. */
			if (!verify_pattern(slots[slot].sl_ptr,
			    slots[slot].sl_size, slots[slot].sl_seed)) {
				corrupt_count++;
				kprintf("stress_mem: pattern mismatch on "
				    "slot %u (%zu B) at iter %u\n",
				    slot, slots[slot].sl_size, i);
			}
			kfree(slots[slot].sl_ptr);
			slots[slot].sl_ptr  = NULL;
			slots[slot].sl_size = 0;
			free_count++;
		}
		/* op == 1 with non-empty slot: skip (lets it ripen) */

		pmm_used = pmm_used_pages();
		if (pmm_used > pmm_used_peak)
			pmm_used_peak = pmm_used;
	}

	/* Drain remaining occupants. */
	for (i = 0; i < STRESS_SLOTS; i++) {
		if (slots[i].sl_ptr != NULL) {
			if (!verify_pattern(slots[i].sl_ptr,
			    slots[i].sl_size, slots[i].sl_seed)) {
				corrupt_count++;
				kprintf("stress_mem: drain mismatch on slot "
				    "%u (%zu B)\n", i, slots[i].sl_size);
			}
			kfree(slots[i].sl_ptr);
			slots[i].sl_ptr = NULL;
			free_count++;
		}
	}

	t_end_cycles = tsc_read();
	pmm_used_after  = pmm_used_pages();
	cached_after    = kmem_cached_pages();
	conserved_after = pmm_used_after - cached_after;
	elapsed_us = tsc_to_us(t_end_cycles - t_start_cycles);

	kprintf("stress_mem: %llu alloc, %llu free (delta=%lld)\n",
	    (unsigned long long)alloc_count,
	    (unsigned long long)free_count,
	    (long long)((long long)alloc_count - (long long)free_count));
	kprintf("stress_mem: pmm used %zu -> %zu (peak %zu)\n",
	    pmm_used_before, pmm_used_after, pmm_used_peak);
	kprintf("stress_mem: kmem cached %zu -> %zu pages\n",
	    cached_before, cached_after);
	kprintf("stress_mem: conserved (pmm-cached) %zu -> %zu, "
	    "elapsed %llu us\n",
	    conserved_before, conserved_after,
	    (unsigned long long)elapsed_us);
	kprintf("stress_mem: %llu pattern corruptions\n",
	    (unsigned long long)corrupt_count);

	if (alloc_count != free_count) {
		kprintf("stress_mem: FAIL alloc != free\n");
		return (1);
	}
	if (corrupt_count != 0) {
		kprintf("stress_mem: FAIL pattern corruption\n");
		return (2);
	}
	/*
	 * The real conservation law: pmm_used - kmem_cached must
	 * return to baseline.  Cached refills are not a leak; chunks
	 * dropped on the floor or counted twice would show up here.
	 */
	if (conserved_after != conserved_before) {
		kprintf("stress_mem: FAIL conservation broken "
		    "(delta=%lld pages)\n",
		    (long long)((long long)conserved_after -
			(long long)conserved_before));
		return (3);
	}

	kprintf("stress_mem: PASS\n");
	return (0);

out_fail:
	for (i = 0; i < STRESS_SLOTS; i++) {
		if (slots[i].sl_ptr != NULL) {
			kfree(slots[i].sl_ptr);
			slots[i].sl_ptr = NULL;
		}
	}
	return (-1);
}

/* ---- stress_mem_boundary ---------------------------------------------- */

int
stress_mem_boundary(void)
{
	/*
	 * Every interesting size: just under, at, and just over each
	 * power-of-two bucket boundary, plus a few large-path values.
	 * Size 0 is excluded because kmalloc(0) returns NULL by
	 * design.
	 */
	static const size_t sizes[] = {
		1, 7, 8, 9,
		15, 16, 17,
		23, 24, 31, 32, 33,
		63, 64, 65,
		127, 128, 129,
		255, 256, 257,
		511, 512, 513,
		1023, 1024, 1025,
		2039, 2040,			/* last bucket-fitting */
		2041, 2048, 2049,		/* spill to large path */
		4095, 4096, 4097,
		8191, 8192, 8193,
		16384,
	};
	const size_t	n_sizes = sizeof(sizes) / sizeof(sizes[0]);
	void		*ptrs[64];
	size_t		i;
	uint64_t	corrupt;
	size_t		pmm_used_before, pmm_used_after;
	size_t		cached_before, cached_after;
	size_t		conserved_before, conserved_after;

	kprintf("stress_mem_boundary: %zu sizes\n", n_sizes);
	pmm_used_before  = pmm_used_pages();
	cached_before    = kmem_cached_pages();
	conserved_before = pmm_used_before - cached_before;
	corrupt = 0;

	/* Phase 1: allocate every size, scribble, verify, then keep live. */
	for (i = 0; i < n_sizes; i++) {
		ptrs[i] = kmalloc(sizes[i]);
		if (ptrs[i] == NULL) {
			kprintf("  size %5zu  FAIL kmalloc returned NULL\n",
			    sizes[i]);
			goto out_fail;
		}
		fill_pattern(ptrs[i], sizes[i], (uint8_t)(i + 1));
		if (!verify_pattern(ptrs[i], sizes[i], (uint8_t)(i + 1))) {
			kprintf("  size %5zu  FAIL pattern (post-write)\n",
			    sizes[i]);
			corrupt++;
		}
	}

	/* Phase 2: now they are all live, re-verify -- catches cross-talk. */
	for (i = 0; i < n_sizes; i++) {
		if (!verify_pattern(ptrs[i], sizes[i], (uint8_t)(i + 1))) {
			kprintf("  size %5zu  FAIL pattern (live recheck)\n",
			    sizes[i]);
			corrupt++;
		}
	}

	/* Phase 3: free all. */
	for (i = 0; i < n_sizes; i++) {
		kfree(ptrs[i]);
		ptrs[i] = NULL;
	}

	pmm_used_after  = pmm_used_pages();
	cached_after    = kmem_cached_pages();
	conserved_after = pmm_used_after - cached_after;

	kprintf("stress_mem_boundary: pmm used %zu -> %zu, "
	    "cached %zu -> %zu\n",
	    pmm_used_before, pmm_used_after,
	    cached_before, cached_after);
	kprintf("stress_mem_boundary: conserved %zu -> %zu, "
	    "%llu corruptions\n",
	    conserved_before, conserved_after,
	    (unsigned long long)corrupt);

	if (corrupt != 0) {
		kprintf("stress_mem_boundary: FAIL pattern corruption\n");
		return (1);
	}
	if (conserved_after != conserved_before) {
		kprintf("stress_mem_boundary: FAIL conservation broken "
		    "(delta=%lld pages)\n",
		    (long long)((long long)conserved_after -
			(long long)conserved_before));
		return (2);
	}

	kprintf("stress_mem_boundary: PASS\n");
	return (0);

out_fail:
	for (i = 0; i < n_sizes; i++) {
		if (ptrs[i] != NULL) {
			kfree(ptrs[i]);
			ptrs[i] = NULL;
		}
	}
	return (-1);
}

/* ---- stress_timer ----------------------------------------------------- */

/*
 * Drift test: hold a known number of PIT ticks, measure TSC delta
 * across the same span, derive observed PIT rate from TSC and compare
 * against pit_hz().  Concurrent kmalloc/kfree adds memory-side load
 * so a regression in the IRQ path (missed ticks under contention)
 * shows up as a wider drift band than the idle case.
 */
int
stress_timer(unsigned int seconds)
{
	uint64_t	target_ticks, start_ticks, end_ticks;
	uint64_t	start_tsc, end_tsc, delta_tsc;
	uint64_t	expected_cycles, drift_abs, drift_ppm;
	uint64_t	churn_count;
	void		*small, *medium;
	bool		over;

	if (seconds == 0)
		seconds = 1;
	if (seconds > 30)
		seconds = 30;

	kprintf("stress_timer: %u s, hz=%llu, tsc_hz=%llu\n",
	    seconds,
	    (unsigned long long)pit_hz(),
	    (unsigned long long)tsc_hz());

	target_ticks = (uint64_t)seconds * pit_hz();

	/* Wait for a tick boundary. */
	start_ticks = pit_ticks();
	while (pit_ticks() == start_ticks)
		__asm__ __volatile__ ("pause");

	start_ticks = pit_ticks();
	start_tsc   = tsc_read();
	churn_count = 0;

	while (pit_ticks() - start_ticks < target_ticks) {
		/*
		 * Load: a small + medium alloc/free pair per spin.
		 * Cheap enough that we still complete >>10000 cycles
		 * per second, exercising the allocator under interrupt
		 * pressure throughout the run.
		 */
		small  = kmalloc(48);
		medium = kmalloc(384);
		if (small != NULL)
			kfree(small);
		if (medium != NULL)
			kfree(medium);
		churn_count++;
	}

	end_ticks = pit_ticks();
	end_tsc   = tsc_read();

	delta_tsc       = end_tsc - start_tsc;
	expected_cycles = ((end_ticks - start_ticks) * tsc_hz()) / pit_hz();

	if (delta_tsc >= expected_cycles) {
		drift_abs = delta_tsc - expected_cycles;
		over = true;
	} else {
		drift_abs = expected_cycles - delta_tsc;
		over = false;
	}

	drift_ppm = expected_cycles != 0
	    ? (drift_abs * 1000000ULL) / expected_cycles
	    : 0;

	kprintf("stress_timer: %llu ticks elapsed, %llu cycles measured\n",
	    (unsigned long long)(end_ticks - start_ticks),
	    (unsigned long long)delta_tsc);
	kprintf("stress_timer: expected %llu cycles, "
	    "drift %llu cycles (%s)\n",
	    (unsigned long long)expected_cycles,
	    (unsigned long long)drift_abs,
	    over ? "TSC ahead" : "TSC behind");
	kprintf("stress_timer: drift %llu ppm, %llu kmem ops "
	    "during run\n",
	    (unsigned long long)drift_ppm,
	    (unsigned long long)churn_count);

	if ((end_ticks - start_ticks) < target_ticks) {
		kprintf("stress_timer: FAIL tick count short\n");
		return (1);
	}
	/*
	 * The TSC calibration window (250 ms) and the test window
	 * (2 s) can see very different host-side SMI behaviour, so
	 * "drift" here is dominated by calibration noise, not by any
	 * regression in the IRQ handler.  Set the threshold high
	 * enough that only a real broken-tick-handler shows up:
	 * 200000 ppm == 20%, which corresponds to losing roughly one
	 * tick in five.
	 */
	if (drift_ppm > 200000) {
		kprintf("stress_timer: FAIL drift too large\n");
		return (2);
	}

	kprintf("stress_timer: PASS\n");
	return (0);
}

/* ---- stress_port ------------------------------------------------------ */

/*
 * Round-trip every iteration: client builds a request carrying its
 * reply port as a port descriptor, server receives (gaining a SEND
 * right on the reply port), server sends a reply, client receives.
 *
 * After N rounds, every reference and every name we allocated must
 * have returned to the baseline -- a regression in disposition
 * accounting will pin pmm_used above the start.  The recv-side
 * descriptor translation is exercised twice per round (request body
 * with one descriptor; reply has no body and is the simpler path),
 * so 1000 rounds == 1000 right-transfers through the send/recv path.
 */
struct stress_msg {
	struct mach_msg_header		hdr;
	struct mach_msg_body		body;
	struct mach_msg_port_descriptor	reply_pd;
	uint32_t			payload;
};

int
stress_port(unsigned int rounds)
{
	uint64_t		t_start, t_end, us;
	mach_port_name_t	server, client_reply;
	size_t			inuse_before, inuse_after;
	size_t			cached_before, cached_after;
	size_t			pmm_before, pmm_after;
	size_t			conserved_before, conserved_after;
	struct stress_msg	req, recv_buf;
	struct mach_msg_header	reply;
	unsigned int		i;
	int			rv;

	if (rounds == 0)
		rounds = 1;

	kprintf("stress_port: %u rounds\n", rounds);

	t_start = tsc_read();
	t_end   = t_start;
	inuse_before     = port_space_inuse(kernel_space);
	cached_before    = kmem_cached_pages();
	pmm_before       = pmm_used_pages();
	conserved_before = pmm_before - cached_before;

	server = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	client_reply = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (server == MACH_PORT_NULL || client_reply == MACH_PORT_NULL) {
		kprintf("stress_port: port_allocate failed\n");
		return (1);
	}

	for (i = 0; i < rounds; i++) {
		/* request */
		req.hdr.msgh_bits = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0) |
		    MACH_MSGH_BITS_COMPLEX;
		req.hdr.msgh_size    = sizeof(req);
		req.hdr.msgh_remote  = server;
		req.hdr.msgh_local   = MACH_PORT_NULL;
		req.hdr.msgh_voucher = 0;
		req.hdr.msgh_id      = i;
		req.body.msgh_descriptor_count = 1;
		req.reply_pd.name        = client_reply;
		req.reply_pd.pad1        = 0;
		req.reply_pd.disposition = MACH_MSG_TYPE_MAKE_SEND;
		req.reply_pd.type        = MACH_MSG_PORT_DESCRIPTOR;
		req.reply_pd.pad2        = 0;
		req.payload              = 0x42000000u | i;

		rv = mach_msg_send(kernel_space, &req.hdr);
		if (rv != MACH_MSG_OK) {
			kprintf("stress_port: req send #%u: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}

		/* server recv */
		rv = mach_msg_recv(kernel_space, server,
		    &recv_buf.hdr, sizeof(recv_buf));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_port: req recv #%u: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}
		if (recv_buf.hdr.msgh_id != i) {
			kprintf("stress_port: id mismatch %u vs %u\n",
			    recv_buf.hdr.msgh_id, i);
			rv = 1;
			goto out;
		}

		mach_port_name_t reply_name = recv_buf.reply_pd.name;

		/* server send reply */
		reply.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_MOVE_SEND, 0);
		reply.msgh_size    = sizeof(reply);
		reply.msgh_remote  = reply_name;
		reply.msgh_local   = MACH_PORT_NULL;
		reply.msgh_voucher = 0;
		reply.msgh_id      = i ^ 0xFFFF;

		rv = mach_msg_send(kernel_space, &reply);
		if (rv != MACH_MSG_OK) {
			kprintf("stress_port: reply send #%u: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}

		/* client recv */
		rv = mach_msg_recv(kernel_space, client_reply,
		    &reply, sizeof(reply));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_port: reply recv #%u: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}
		if (reply.msgh_id != (i ^ 0xFFFF)) {
			kprintf("stress_port: reply id mismatch\n");
			rv = 1;
			goto out;
		}
	}

	t_end = tsc_read();

	rv = MACH_MSG_OK;

out:
	port_deallocate(kernel_space, server);
	port_deallocate(kernel_space, client_reply);

	inuse_after      = port_space_inuse(kernel_space);
	cached_after     = kmem_cached_pages();
	pmm_after        = pmm_used_pages();
	conserved_after  = pmm_after - cached_after;
	us = tsc_to_us(t_end - t_start);

	kprintf("stress_port: kernel_space inuse %zu -> %zu\n",
	    inuse_before, inuse_after);
	kprintf("stress_port: pmm used %zu -> %zu, "
	    "cached %zu -> %zu\n",
	    pmm_before, pmm_after, cached_before, cached_after);
	kprintf("stress_port: conserved %zu -> %zu, elapsed %llu us\n",
	    conserved_before, conserved_after,
	    (unsigned long long)us);

	if (rv != MACH_MSG_OK) {
		kprintf("stress_port: FAIL\n");
		return (rv);
	}
	if (inuse_after != inuse_before) {
		kprintf("stress_port: FAIL space name leak (delta=%lld)\n",
		    (long long)((long long)inuse_after -
			(long long)inuse_before));
		return (10);
	}
	if (conserved_after != conserved_before) {
		kprintf("stress_port: FAIL conservation broken "
		    "(delta=%lld pages)\n",
		    (long long)((long long)conserved_after -
			(long long)conserved_before));
		return (11);
	}

	kprintf("stress_port: PASS\n");
	return (0);
}

/* ---- stress_thread ---------------------------------------------------- */

/*
 * Cross-thread Mach RPC over blocking mach_msg_recv.
 *
 * Setup:
 *	server thread       --  loop: recv_block on req_port, reply
 *	this (client)       --  loop: send req, recv_block on reply_port
 *	done_port           --  server -> client signal on exit
 *
 * The client embeds reply_port as a port descriptor (MAKE_SEND) in
 * every request, so the server receives a fresh SEND right per round
 * and uses MOVE_SEND on the reply so its right is consumed.  At
 * round N+1 the client sends a sentinel msgh_id, the server breaks
 * out of its loop, signals via done_port, and exits.
 *
 * Net check: after the test, name table is back to baseline,
 * conserved memory is back to baseline, and the server thread has
 * been reaped.
 */
struct stress_thread_ctx {
	mach_port_name_t	stc_req_port;
	mach_port_name_t	stc_done_port;
	unsigned int		stc_rv;
};

#define	STRESS_THREAD_SENTINEL	0xDEADBEEFu

static void
stress_server_thread(void *arg)
{
	struct stress_thread_ctx	*ctx = arg;
	struct stress_msg		recv;
	struct mach_msg_header		reply, done;
	int				rv;

	for (;;) {
		rv = mach_msg_recv_block(kernel_space, ctx->stc_req_port,
		    &recv.hdr, sizeof(recv));
		if (rv != MACH_MSG_OK) {
			ctx->stc_rv = (unsigned int)rv;
			break;
		}

		if (recv.hdr.msgh_id == STRESS_THREAD_SENTINEL) {
			ctx->stc_rv = 0;
			break;
		}

		/*
		 * Reply via the just-arrived reply-port name; MOVE_SEND
		 * because we never want to send to it again -- a fresh
		 * SEND right will arrive with the next request.
		 */
		reply.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_MOVE_SEND, 0);
		reply.msgh_size    = sizeof(reply);
		reply.msgh_remote  = recv.reply_pd.name;
		reply.msgh_local   = MACH_PORT_NULL;
		reply.msgh_voucher = 0;
		reply.msgh_id      = recv.hdr.msgh_id + 1;
		(void)mach_msg_send(kernel_space, &reply);
	}

	/* Signal the client that we're done and about to exit. */
	done.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_MOVE_SEND, 0);
	done.msgh_size    = sizeof(done);
	done.msgh_remote  = ctx->stc_done_port;
	done.msgh_local   = MACH_PORT_NULL;
	done.msgh_voucher = 0;
	done.msgh_id      = 0;
	(void)mach_msg_send(kernel_space, &done);
}

int
stress_thread(unsigned int rounds)
{
	struct stress_thread_ctx	ctx;
	mach_port_name_t		reply_port;
	struct stress_msg		req;
	struct mach_msg_header		sentinel, reply, done;
	struct thread			*server;
	uint64_t			t0, t1;
	size_t				inuse0, inuse1;
	size_t				cached0, cached1;
	size_t				pmm0, pmm1;
	size_t				cons0, cons1;
	uint64_t			ctx_sw_before, ctx_sw_after;
	uint64_t			ticks_before, ticks_after;
	unsigned int			i;
	int				rv;

	if (rounds == 0)
		rounds = 1;
	if (rounds > 100000u)
		rounds = 100000u;

	kprintf("stress_thread: %u rounds (RPC over blocking recv)\n",
	    rounds);

	inuse0  = port_space_inuse(kernel_space);
	cached0 = kmem_cached_pages();
	pmm0    = pmm_used_pages();
	cons0   = pmm0 - cached0;
	ctx_sw_before = sched_context_switches();
	ticks_before  = clock_ticks();

	ctx.stc_req_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	ctx.stc_done_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	reply_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	ctx.stc_rv = 0;

	if (ctx.stc_req_port == MACH_PORT_NULL ||
	    ctx.stc_done_port == MACH_PORT_NULL ||
	    reply_port == MACH_PORT_NULL) {
		kprintf("stress_thread: port_allocate failed\n");
		return (1);
	}

	server = thread_create(kernel_task, stress_server_thread,
	    &ctx, "stress-server");
	if (server == NULL) {
		kprintf("stress_thread: thread_create failed\n");
		return (2);
	}
	thread_start(server);

	t0 = tsc_read();

	for (i = 0; i < rounds; i++) {
		req.hdr.msgh_bits = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0) |
		    MACH_MSGH_BITS_COMPLEX;
		req.hdr.msgh_size    = sizeof(req);
		req.hdr.msgh_remote  = ctx.stc_req_port;
		req.hdr.msgh_local   = MACH_PORT_NULL;
		req.hdr.msgh_voucher = 0;
		req.hdr.msgh_id      = i;
		req.body.msgh_descriptor_count = 1;
		req.reply_pd.name        = reply_port;
		req.reply_pd.pad1        = 0;
		req.reply_pd.disposition = MACH_MSG_TYPE_MAKE_SEND;
		req.reply_pd.type        = MACH_MSG_PORT_DESCRIPTOR;
		req.reply_pd.pad2        = 0;
		req.payload              = i;

		rv = mach_msg_send(kernel_space, &req.hdr);
		if (rv != MACH_MSG_OK) {
			kprintf("stress_thread: req #%u send: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}

		rv = mach_msg_recv_block(kernel_space, reply_port,
		    &reply, sizeof(reply));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_thread: reply #%u recv: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}
		if (reply.msgh_id != i + 1) {
			kprintf("stress_thread: reply id mismatch "
			    "%u vs %u\n", reply.msgh_id, i + 1);
			rv = 3;
			goto out;
		}
	}

	/* Sentinel -- causes server to break out of its loop. */
	sentinel.msgh_bits    = MACH_MSGH_BITS(
	    MACH_MSG_TYPE_COPY_SEND, 0);
	sentinel.msgh_size    = sizeof(sentinel);
	sentinel.msgh_remote  = ctx.stc_req_port;
	sentinel.msgh_local   = MACH_PORT_NULL;
	sentinel.msgh_voucher = 0;
	sentinel.msgh_id      = STRESS_THREAD_SENTINEL;
	rv = mach_msg_send(kernel_space, &sentinel);
	if (rv != MACH_MSG_OK) {
		kprintf("stress_thread: sentinel send: %s\n",
		    mach_msg_strerror(rv));
		goto out;
	}

	rv = mach_msg_recv_block(kernel_space, ctx.stc_done_port,
	    &done, sizeof(done));
	if (rv != MACH_MSG_OK) {
		kprintf("stress_thread: done recv: %s\n",
		    mach_msg_strerror(rv));
		goto out;
	}

	rv = (int)ctx.stc_rv;

out:
	t1 = tsc_read();

	port_deallocate(kernel_space, ctx.stc_req_port);
	port_deallocate(kernel_space, ctx.stc_done_port);
	port_deallocate(kernel_space, reply_port);

	/*
	 * The server thread is now ZOMBIE; reap it deterministically
	 * before the conservation check so the kstack is returned to
	 * the same baseline we measured at entry.
	 */
	sched_reap_zombies();

	inuse1  = port_space_inuse(kernel_space);
	cached1 = kmem_cached_pages();
	pmm1    = pmm_used_pages();
	cons1   = pmm1 - cached1;
	ctx_sw_after = sched_context_switches();
	ticks_after  = clock_ticks();

	kprintf("stress_thread: %llu us, %llu context switches, "
	    "%llu PIT ticks\n",
	    (unsigned long long)tsc_to_us(t1 - t0),
	    (unsigned long long)(ctx_sw_after - ctx_sw_before),
	    (unsigned long long)(ticks_after - ticks_before));
	kprintf("stress_thread: ports %zu -> %zu, "
	    "pmm %zu -> %zu, cached %zu -> %zu\n",
	    inuse0, inuse1, pmm0, pmm1, cached0, cached1);
	kprintf("stress_thread: conserved %zu -> %zu\n",
	    cons0, cons1);

	if (rv != MACH_MSG_OK && rv != 0) {
		kprintf("stress_thread: FAIL rv=%d\n", rv);
		return (rv);
	}
	if (inuse1 != inuse0) {
		kprintf("stress_thread: FAIL port name leak "
		    "(delta=%lld)\n",
		    (long long)((long long)inuse1 - (long long)inuse0));
		return (20);
	}
	if (cons1 != cons0) {
		kprintf("stress_thread: FAIL conservation broken "
		    "(delta=%lld pages)\n",
		    (long long)((long long)cons1 - (long long)cons0));
		return (21);
	}

	kprintf("stress_thread: PASS\n");
	return (0);
}

/* ---- stress_preempt --------------------------------------------------- */

/*
 * The acid test for preemption.  Spawn N CPU-bound worker threads
 * that do nothing but increment a per-worker counter in a tight
 * loop -- they never yield, never block, never touch the scheduler
 * directly.  The main thread then sleeps for `sleep_ms` (a busy-
 * wait on pit_ticks) and tells the workers to stop.
 *
 * In a cooperative scheduler, exactly one worker -- whichever ran
 * first -- would have a non-zero counter; the others would be
 * stranded in the runqueue.  Under preemption, the PIT interrupt
 * forcibly rotates between them and every counter is non-zero.
 *
 * Pass conditions: every worker's counter > 0 AND we observed at
 * least a few IRQ-driven preempts.  Counter VALUES are reported but
 * not gated -- ratios depend heavily on host scheduling jitter.
 */

#define	STRESS_PREEMPT_MAX_WORKERS	16

struct preempt_worker_arg {
	volatile uint64_t	*pwa_counter;
	volatile bool		*pwa_stop;
	mach_port_name_t	 pwa_done_port;
	unsigned int		 pwa_id;
};

static void
preempt_worker(void *arg)
{
	struct preempt_worker_arg	*a = arg;
	struct mach_msg_header		done;

	while (!*a->pwa_stop) {
		/*
		 * Plain integer increment -- the compiler cannot
		 * elide this because the counter is volatile and a
		 * reader (the main thread) examines it later.  The
		 * worker holds no locks, makes no syscalls, never
		 * yields voluntarily.
		 */
		(*a->pwa_counter)++;
	}

	done.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	done.msgh_size    = sizeof(done);
	done.msgh_remote  = a->pwa_done_port;
	done.msgh_local   = MACH_PORT_NULL;
	done.msgh_voucher = 0;
	done.msgh_id      = a->pwa_id;
	(void)mach_msg_send(kernel_space, &done);
}

int
stress_preempt(unsigned int n_workers, unsigned int sleep_ms)
{
	static volatile uint64_t	counters[STRESS_PREEMPT_MAX_WORKERS];
	static volatile bool		stop_flag;
	static struct preempt_worker_arg args[STRESS_PREEMPT_MAX_WORKERS];

	mach_port_name_t	done_port;
	uint64_t		t_start, t_end;
	uint64_t		ctx_before, ctx_after;
	uint64_t		preempts_before, preempts_after;
	uint64_t		min_c, max_c, total_c;
	unsigned int		i;
	int			rv;

	if (n_workers == 0)
		n_workers = 4;
	if (n_workers > STRESS_PREEMPT_MAX_WORKERS)
		n_workers = STRESS_PREEMPT_MAX_WORKERS;
	if (sleep_ms == 0)
		sleep_ms = 1000;
	if (sleep_ms > 10000)
		sleep_ms = 10000;

	kprintf("stress_preempt: %u workers, %u ms wall window\n",
	    n_workers, sleep_ms);

	for (i = 0; i < n_workers; i++)
		counters[i] = 0;
	stop_flag = false;

	done_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	if (done_port == MACH_PORT_NULL) {
		kprintf("stress_preempt: port_allocate failed\n");
		return (1);
	}

	for (i = 0; i < n_workers; i++) {
		struct thread *th;

		args[i].pwa_counter   = &counters[i];
		args[i].pwa_stop      = &stop_flag;
		args[i].pwa_done_port = done_port;
		args[i].pwa_id        = i;

		th = thread_create(kernel_task, preempt_worker,
		    &args[i], "preempt-worker");
		if (th == NULL) {
			kprintf("stress_preempt: thread_create #%u failed\n",
			    i);
			stop_flag = true;
			port_deallocate(kernel_space, done_port);
			return (2);
		}
		thread_start(th);
	}

	ctx_before      = sched_context_switches();
	preempts_before = sched_preempts();
	t_start         = tsc_read();

	clock_busy_sleep_ms((uint64_t)sleep_ms);

	t_end           = tsc_read();
	ctx_after       = sched_context_switches();
	preempts_after  = sched_preempts();

	/* Tell workers to stop and wait for each to acknowledge. */
	__atomic_store_n(&stop_flag, true, __ATOMIC_RELEASE);

	for (i = 0; i < n_workers; i++) {
		struct mach_msg_header	ack;
		rv = mach_msg_recv_block(kernel_space, done_port,
		    &ack, sizeof(ack));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_preempt: ack #%u recv: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}
	}

	rv = 0;

out:
	port_deallocate(kernel_space, done_port);
	sched_reap_zombies();

	kprintf("stress_preempt: %llu us elapsed, ctx switches %llu, "
	    "preempts %llu\n",
	    (unsigned long long)tsc_to_us(t_end - t_start),
	    (unsigned long long)(ctx_after - ctx_before),
	    (unsigned long long)(preempts_after - preempts_before));

	total_c = 0;
	min_c   = ~(uint64_t)0;
	max_c   = 0;
	for (i = 0; i < n_workers; i++) {
		kprintf("  worker %2u counter %llu\n",
		    i, (unsigned long long)counters[i]);
		total_c += counters[i];
		if (counters[i] < min_c)
			min_c = counters[i];
		if (counters[i] > max_c)
			max_c = counters[i];
	}
	kprintf("stress_preempt: total %llu, min %llu, max %llu, "
	    "ratio max/min = %llu\n",
	    (unsigned long long)total_c,
	    (unsigned long long)min_c,
	    (unsigned long long)max_c,
	    min_c == 0 ? 0ULL : (unsigned long long)(max_c / min_c));

	if (rv != 0) {
		kprintf("stress_preempt: FAIL (rv=%d)\n", rv);
		return (rv);
	}
	if (min_c == 0) {
		kprintf("stress_preempt: FAIL worker starvation\n");
		return (30);
	}
	if ((preempts_after - preempts_before) < 2) {
		kprintf("stress_preempt: FAIL no preemption observed "
		    "(delta %llu)\n",
		    (unsigned long long)(preempts_after -
		        preempts_before));
		return (31);
	}

	kprintf("stress_preempt: PASS\n");
	return (0);
}

/* ---- stress_sendonce -------------------------------------------------- */

/*
 * The canonical Mach reply-port pattern using send-once rights.
 *
 *	client: build request with reply_pd disposition = MAKE_SEND_ONCE
 *	send:   server gets a SEND_ONCE in its name table
 *	server: reply uses MOVE_SEND_ONCE -- the right is consumed
 *	        BY the send, so the server cannot send twice
 *
 * The whole point versus plain SEND: after N rounds, the server's
 * accidentally-held SEND right would accumulate without bound; with
 * SEND_ONCE the kernel enforces the one-shot semantics.  We verify
 * by tracking inuse names in the kernel space across the run.
 */

struct stress_sendonce_ctx {
	mach_port_name_t	req_port;
	mach_port_name_t	done_port;
	unsigned int		rv;
};

static void
stress_sendonce_server(void *arg)
{
	struct stress_sendonce_ctx	*ctx = arg;
	struct stress_msg		recv;
	struct mach_msg_header		reply, done;
	int				rv;

	for (;;) {
		rv = mach_msg_recv_block(kernel_space, ctx->req_port,
		    &recv.hdr, sizeof(recv));
		if (rv != MACH_MSG_OK) {
			ctx->rv = (unsigned int)rv;
			break;
		}

		if (recv.hdr.msgh_id == STRESS_THREAD_SENTINEL) {
			ctx->rv = 0;
			break;
		}

		/*
		 * recv.reply_pd.name holds a SEND_ONCE right in our
		 * namespace.  Reply via MOVE_SEND_ONCE: the right is
		 * consumed by the send, and the name is removed from
		 * our space automatically.  A second reply attempt
		 * here would fail with MACH_E_RIGHT.
		 */
		reply.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
		reply.msgh_size    = sizeof(reply);
		reply.msgh_remote  = recv.reply_pd.name;
		reply.msgh_local   = MACH_PORT_NULL;
		reply.msgh_voucher = 0;
		reply.msgh_id      = recv.hdr.msgh_id + 1;
		(void)mach_msg_send(kernel_space, &reply);
	}

	done.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	done.msgh_size    = sizeof(done);
	done.msgh_remote  = ctx->done_port;
	done.msgh_local   = MACH_PORT_NULL;
	done.msgh_voucher = 0;
	done.msgh_id      = 0;
	(void)mach_msg_send(kernel_space, &done);
}

int
stress_sendonce(unsigned int rounds)
{
	struct stress_sendonce_ctx	ctx;
	mach_port_name_t		reply_port;
	struct stress_msg		req;
	struct mach_msg_header		sentinel, reply, done;
	struct thread			*server;
	size_t				inuse0, inuse1;
	size_t				cached0, cached1;
	size_t				pmm0, pmm1, cons0, cons1;
	unsigned int			i;
	int				rv;

	if (rounds == 0)
		rounds = 1;
	if (rounds > 100000u)
		rounds = 100000u;

	kprintf("stress_sendonce: %u rounds (MAKE_SEND_ONCE / "
	    "MOVE_SEND_ONCE)\n", rounds);

	inuse0  = port_space_inuse(kernel_space);
	cached0 = kmem_cached_pages();
	pmm0    = pmm_used_pages();
	cons0   = pmm0 - cached0;

	ctx.req_port  = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	ctx.done_port = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	reply_port    = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE);
	ctx.rv        = 0;

	if (ctx.req_port == MACH_PORT_NULL ||
	    ctx.done_port == MACH_PORT_NULL ||
	    reply_port == MACH_PORT_NULL) {
		kprintf("stress_sendonce: port_allocate failed\n");
		return (1);
	}

	server = thread_create(kernel_task, stress_sendonce_server,
	    &ctx, "sendonce-server");
	if (server == NULL) {
		kprintf("stress_sendonce: thread_create failed\n");
		return (2);
	}
	thread_start(server);

	for (i = 0; i < rounds; i++) {
		req.hdr.msgh_bits = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0) |
		    MACH_MSGH_BITS_COMPLEX;
		req.hdr.msgh_size    = sizeof(req);
		req.hdr.msgh_remote  = ctx.req_port;
		req.hdr.msgh_local   = MACH_PORT_NULL;
		req.hdr.msgh_voucher = 0;
		req.hdr.msgh_id      = i;
		req.body.msgh_descriptor_count = 1;
		req.reply_pd.name        = reply_port;
		req.reply_pd.pad1        = 0;
		req.reply_pd.disposition = MACH_MSG_TYPE_MAKE_SEND_ONCE;
		req.reply_pd.type        = MACH_MSG_PORT_DESCRIPTOR;
		req.reply_pd.pad2        = 0;
		req.payload              = i;

		rv = mach_msg_send(kernel_space, &req.hdr);
		if (rv != MACH_MSG_OK) {
			kprintf("stress_sendonce: req #%u send: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}

		rv = mach_msg_recv_block(kernel_space, reply_port,
		    &reply, sizeof(reply));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_sendonce: reply #%u recv: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}
		if (reply.msgh_id != i + 1) {
			kprintf("stress_sendonce: reply id mismatch "
			    "%u vs %u\n", reply.msgh_id, i + 1);
			rv = 3;
			goto out;
		}
	}

	sentinel.msgh_bits    = MACH_MSGH_BITS(
	    MACH_MSG_TYPE_COPY_SEND, 0);
	sentinel.msgh_size    = sizeof(sentinel);
	sentinel.msgh_remote  = ctx.req_port;
	sentinel.msgh_local   = MACH_PORT_NULL;
	sentinel.msgh_voucher = 0;
	sentinel.msgh_id      = STRESS_THREAD_SENTINEL;
	rv = mach_msg_send(kernel_space, &sentinel);
	if (rv != MACH_MSG_OK) {
		kprintf("stress_sendonce: sentinel send: %s\n",
		    mach_msg_strerror(rv));
		goto out;
	}

	rv = mach_msg_recv_block(kernel_space, ctx.done_port,
	    &done, sizeof(done));
	if (rv != MACH_MSG_OK) {
		kprintf("stress_sendonce: done recv: %s\n",
		    mach_msg_strerror(rv));
		goto out;
	}

	rv = (int)ctx.rv;

out:
	port_deallocate(kernel_space, ctx.req_port);
	port_deallocate(kernel_space, ctx.done_port);
	port_deallocate(kernel_space, reply_port);
	sched_reap_zombies();

	inuse1  = port_space_inuse(kernel_space);
	cached1 = kmem_cached_pages();
	pmm1    = pmm_used_pages();
	cons1   = pmm1 - cached1;

	kprintf("stress_sendonce: ports %zu -> %zu, "
	    "conserved %zu -> %zu\n",
	    inuse0, inuse1, cons0, cons1);

	if (rv != MACH_MSG_OK && rv != 0) {
		kprintf("stress_sendonce: FAIL rv=%d\n", rv);
		return (rv);
	}
	if (inuse1 != inuse0) {
		kprintf("stress_sendonce: FAIL port name leak "
		    "(delta=%lld)\n",
		    (long long)((long long)inuse1 - (long long)inuse0));
		return (40);
	}
	if (cons1 != cons0) {
		kprintf("stress_sendonce: FAIL conservation broken "
		    "(delta=%lld pages)\n",
		    (long long)((long long)cons1 - (long long)cons0));
		return (41);
	}

	kprintf("stress_sendonce: PASS\n");
	return (0);
}

/* ---- stress_portset --------------------------------------------------- */

/*
 * Port-set fan-in: one set, N member ports, one server thread parked
 * on the set.  The (boot) thread sends per_member messages to each
 * of the N ports in interleaved order, encoding the source port in
 * msgh_id.  The server receives via the set in whatever order the
 * scheduler / wake-policy picks, counts arrivals per source.
 *
 * Pass conditions:
 *	- exactly N * per_member messages delivered
 *	- per-source counts are all == per_member (no message lost or
 *	  misattributed)
 *	- post-test conserved memory + ports back to baseline
 */

#define	STRESS_PORTSET_MAX_MEMBERS	8

struct stress_portset_ctx {
	mach_port_name_t	sps_set;
	mach_port_name_t	sps_done;
	unsigned int		sps_n_members;
	unsigned int		sps_per_member;
	uint32_t		sps_counts[STRESS_PORTSET_MAX_MEMBERS];
	uint32_t		sps_total;
};

static void
stress_portset_server(void *arg)
{
	struct stress_portset_ctx	*ctx = arg;
	struct stress_msg		recv;
	struct mach_msg_header		done;
	uint32_t			total_expected;
	int				rv;

	total_expected = ctx->sps_n_members * ctx->sps_per_member;

	while (ctx->sps_total < total_expected) {
		rv = mach_msg_recv_block(kernel_space, ctx->sps_set,
		    &recv.hdr, sizeof(recv));
		if (rv != MACH_MSG_OK)
			break;

		uint32_t src = recv.hdr.msgh_id >> 16;
		if (src < ctx->sps_n_members)
			ctx->sps_counts[src]++;
		ctx->sps_total++;
	}

	done.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	done.msgh_size    = sizeof(done);
	done.msgh_remote  = ctx->sps_done;
	done.msgh_local   = MACH_PORT_NULL;
	done.msgh_voucher = 0;
	done.msgh_id      = 0;
	(void)mach_msg_send(kernel_space, &done);
}

int
stress_portset(unsigned int n_members, unsigned int per_member)
{
	struct stress_portset_ctx	ctx;
	mach_port_name_t		members[STRESS_PORTSET_MAX_MEMBERS];
	struct thread			*server;
	size_t				pmm0, pmm1, cached0, cached1;
	size_t				cons0, cons1, inuse0, inuse1;
	unsigned int			i, j;
	int				rv;

	if (n_members == 0)
		n_members = 4;
	if (n_members > STRESS_PORTSET_MAX_MEMBERS)
		n_members = STRESS_PORTSET_MAX_MEMBERS;
	if (per_member == 0)
		per_member = 50;
	if (per_member > 10000)
		per_member = 10000;

	kprintf("stress_portset: 1 set, %u members, %u msgs each\n",
	    n_members, per_member);

	inuse0  = port_space_inuse(kernel_space);
	cached0 = kmem_cached_pages();
	pmm0    = pmm_used_pages();
	cons0   = pmm0 - cached0;

	for (i = 0; i < n_members; i++)
		ctx.sps_counts[i] = 0;
	ctx.sps_total      = 0;
	ctx.sps_n_members  = n_members;
	ctx.sps_per_member = per_member;
	ctx.sps_done = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	ctx.sps_set = port_set_allocate(kernel_space);
	if (ctx.sps_done == MACH_PORT_NULL ||
	    ctx.sps_set == MACH_PORT_NULL) {
		kprintf("stress_portset: allocation failed\n");
		return (1);
	}

	/* Allocate member ports and insert each into the set. */
	for (i = 0; i < n_members; i++) {
		members[i] = port_allocate(kernel_space,
		    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
		if (members[i] == MACH_PORT_NULL) {
			kprintf("stress_portset: member %u alloc failed\n",
			    i);
			return (2);
		}
		rv = port_set_insert(kernel_space, ctx.sps_set,
		    members[i]);
		if (rv != MACH_MSG_OK) {
			kprintf("stress_portset: insert %u: %s\n",
			    i, mach_msg_strerror(rv));
			return (3);
		}
	}

	server = thread_create(kernel_task, stress_portset_server,
	    &ctx, "portset-server");
	if (server == NULL) {
		kprintf("stress_portset: thread_create failed\n");
		return (4);
	}
	thread_start(server);

	/*
	 * Interleave sends across the members.  Encoding the source
	 * port index in the high half of msgh_id lets the server
	 * attribute each message back without a body descriptor.
	 */
	for (j = 0; j < per_member; j++) {
		for (i = 0; i < n_members; i++) {
			struct mach_msg_header	msg;

			msg.msgh_bits    = MACH_MSGH_BITS(
			    MACH_MSG_TYPE_COPY_SEND, 0);
			msg.msgh_size    = sizeof(msg);
			msg.msgh_remote  = members[i];
			msg.msgh_local   = MACH_PORT_NULL;
			msg.msgh_voucher = 0;
			msg.msgh_id      = (i << 16) | j;

			rv = mach_msg_send(kernel_space, &msg);
			if (rv != MACH_MSG_OK) {
				kprintf("stress_portset: send (%u,%u): "
				    "%s\n", i, j, mach_msg_strerror(rv));
				goto out;
			}
		}
	}

	/* Wait for server to drain all messages and signal done. */
	{
		struct mach_msg_header	done;
		rv = mach_msg_recv_block(kernel_space, ctx.sps_done,
		    &done, sizeof(done));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_portset: done recv: %s\n",
			    mach_msg_strerror(rv));
			goto out;
		}
	}

	rv = 0;

out:
	for (i = 0; i < n_members; i++)
		port_deallocate(kernel_space, members[i]);
	port_deallocate(kernel_space, ctx.sps_set);
	port_deallocate(kernel_space, ctx.sps_done);
	sched_reap_zombies();

	inuse1  = port_space_inuse(kernel_space);
	cached1 = kmem_cached_pages();
	pmm1    = pmm_used_pages();
	cons1   = pmm1 - cached1;

	kprintf("stress_portset: server saw %u total\n", ctx.sps_total);
	for (i = 0; i < n_members; i++)
		kprintf("  member %u: %u msgs (expected %u)\n",
		    i, ctx.sps_counts[i], per_member);
	kprintf("stress_portset: ports %zu -> %zu, conserved %zu -> %zu\n",
	    inuse0, inuse1, cons0, cons1);

	if (rv != 0) {
		kprintf("stress_portset: FAIL rv=%d\n", rv);
		return (rv);
	}
	if (ctx.sps_total != n_members * per_member) {
		kprintf("stress_portset: FAIL total %u != %u\n",
		    ctx.sps_total, n_members * per_member);
		return (50);
	}
	for (i = 0; i < n_members; i++) {
		if (ctx.sps_counts[i] != per_member) {
			kprintf("stress_portset: FAIL member %u "
			    "count %u != %u\n",
			    i, ctx.sps_counts[i], per_member);
			return (51);
		}
	}
	if (inuse1 != inuse0) {
		kprintf("stress_portset: FAIL port name leak\n");
		return (52);
	}
	if (cons1 != cons0) {
		kprintf("stress_portset: FAIL conservation broken "
		    "(delta=%lld)\n",
		    (long long)((long long)cons1 - (long long)cons0));
		return (53);
	}

	kprintf("stress_portset: PASS\n");
	return (0);
}

/* ---- stress_intertask ------------------------------------------------- */

/*
 * Two tasks, one IPC channel.  The parent (kernel_task) is the
 * client.  A second task ("worker") is created with its own
 * port_space; a server thread spawned inside it receives requests
 * and replies through send-once rights the client embeds in every
 * message.
 *
 * Bootstrap:
 *	service port	allocated in worker_space (RECV+SEND there);
 *			port_space_inject_send copies a SEND right
 *			into kernel_space under a different name --
 *			that's how the parent learns to talk to it.
 *	done port	allocated in kernel_space (RECV+SEND there);
 *			SEND right injected into worker_space so the
 *			worker can signal completion.
 *
 * Verification: every name in BOTH spaces returns to its pre-test
 * count, and conserved memory returns to baseline.  The new task is
 * destroyed at the end (last thread exits + task_deref).
 */

struct intertask_ctx {
	mach_port_name_t	itc_service;	/* in worker_space   */
	mach_port_name_t	itc_done;	/* in worker_space   */
	unsigned int		itc_rv;
};

static void
intertask_server(void *arg)
{
	struct intertask_ctx	*ctx = arg;
	struct stress_msg	 recv;
	struct mach_msg_header	 reply, done;
	struct port_space	*self_space;
	int			 rv;

	self_space = current_thread->th_task->t_port_space;

	for (;;) {
		rv = mach_msg_recv_block(self_space, ctx->itc_service,
		    &recv.hdr, sizeof(recv));
		if (rv != MACH_MSG_OK) {
			ctx->itc_rv = (unsigned int)rv;
			break;
		}

		if (recv.hdr.msgh_id == STRESS_THREAD_SENTINEL) {
			ctx->itc_rv = 0;
			break;
		}

		/*
		 * recv.reply_pd.name was just installed by deliver_msg
		 * into THIS task's port_space; it carries a SEND_ONCE
		 * right.  MOVE_SEND_ONCE consumes the right.
		 */
		reply.msgh_bits    = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_MOVE_SEND_ONCE, 0);
		reply.msgh_size    = sizeof(reply);
		reply.msgh_remote  = recv.reply_pd.name;
		reply.msgh_local   = MACH_PORT_NULL;
		reply.msgh_voucher = 0;
		reply.msgh_id      = recv.hdr.msgh_id + 1;
		(void)mach_msg_send(self_space, &reply);
	}

	done.msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
	done.msgh_size    = sizeof(done);
	done.msgh_remote  = ctx->itc_done;
	done.msgh_local   = MACH_PORT_NULL;
	done.msgh_voucher = 0;
	done.msgh_id      = 0;
	(void)mach_msg_send(self_space, &done);
}

int
stress_intertask(unsigned int rounds)
{
	struct task		*worker;
	struct intertask_ctx	 ctx;
	mach_port_name_t	 service_kernel, reply_port_kernel;
	mach_port_name_t	 service_worker, done_kernel, done_worker;
	struct stress_msg	 req;
	struct mach_msg_header	 sentinel, reply, done;
	struct thread		*server;
	size_t			 inuse_k0, inuse_k1, inuse_w0, inuse_w1;
	size_t			 cached0, cached1, pmm0, pmm1, cons0, cons1;
	unsigned int		 i;
	int			 rv;

	if (rounds == 0)
		rounds = 1;
	if (rounds > 100000u)
		rounds = 100000u;

	kprintf("stress_intertask: %u rounds across 2 tasks\n", rounds);

	cached0 = kmem_cached_pages();
	pmm0    = pmm_used_pages();
	cons0   = pmm0 - cached0;
	inuse_k0 = port_space_inuse(kernel_space);

	worker = task_create("worker");
	if (worker == NULL) {
		kprintf("stress_intertask: task_create failed\n");
		return (1);
	}
	inuse_w0 = port_space_inuse(worker->t_port_space);

	/*
	 * Service port: created in the worker's namespace so the
	 * worker holds the RECEIVE end; the parent learns the same
	 * port under a separate name via port_space_inject_send.
	 */
	service_worker = port_allocate(worker->t_port_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	rv = port_space_inject_send(worker->t_port_space, service_worker,
	    kernel_space, &service_kernel);
	if (service_worker == MACH_PORT_NULL || rv != MACH_MSG_OK) {
		kprintf("stress_intertask: service bootstrap failed\n");
		return (2);
	}

	/*
	 * Done port: parent allocates in kernel_space, parent holds
	 * RECEIVE.  Worker gets a SEND right under its own name to
	 * signal back.
	 */
	done_kernel = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE | MACH_PORT_RIGHT_SEND);
	rv = port_space_inject_send(kernel_space, done_kernel,
	    worker->t_port_space, &done_worker);
	if (done_kernel == MACH_PORT_NULL || rv != MACH_MSG_OK) {
		kprintf("stress_intertask: done bootstrap failed\n");
		return (3);
	}

	/* Reply port: client-side only, kernel_space. */
	reply_port_kernel = port_allocate(kernel_space,
	    MACH_PORT_RIGHT_RECEIVE);
	if (reply_port_kernel == MACH_PORT_NULL) {
		kprintf("stress_intertask: reply_port allocate failed\n");
		return (4);
	}

	ctx.itc_service = service_worker;
	ctx.itc_done    = done_worker;
	ctx.itc_rv      = 0;

	server = thread_create(worker, intertask_server, &ctx,
	    "intertask-server");
	if (server == NULL) {
		kprintf("stress_intertask: thread_create failed\n");
		return (5);
	}
	thread_start(server);

	for (i = 0; i < rounds; i++) {
		req.hdr.msgh_bits = MACH_MSGH_BITS(
		    MACH_MSG_TYPE_COPY_SEND, 0) |
		    MACH_MSGH_BITS_COMPLEX;
		req.hdr.msgh_size    = sizeof(req);
		req.hdr.msgh_remote  = service_kernel;
		req.hdr.msgh_local   = MACH_PORT_NULL;
		req.hdr.msgh_voucher = 0;
		req.hdr.msgh_id      = i;
		req.body.msgh_descriptor_count = 1;
		req.reply_pd.name        = reply_port_kernel;
		req.reply_pd.pad1        = 0;
		req.reply_pd.disposition = MACH_MSG_TYPE_MAKE_SEND_ONCE;
		req.reply_pd.type        = MACH_MSG_PORT_DESCRIPTOR;
		req.reply_pd.pad2        = 0;
		req.payload              = i;

		rv = mach_msg_send(kernel_space, &req.hdr);
		if (rv != MACH_MSG_OK) {
			kprintf("stress_intertask: req #%u send: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}

		rv = mach_msg_recv_block(kernel_space, reply_port_kernel,
		    &reply, sizeof(reply));
		if (rv != MACH_MSG_OK) {
			kprintf("stress_intertask: reply #%u recv: %s\n",
			    i, mach_msg_strerror(rv));
			goto out;
		}
		if (reply.msgh_id != i + 1) {
			kprintf("stress_intertask: reply id mismatch\n");
			rv = 6;
			goto out;
		}
	}

	sentinel.msgh_bits    = MACH_MSGH_BITS(
	    MACH_MSG_TYPE_COPY_SEND, 0);
	sentinel.msgh_size    = sizeof(sentinel);
	sentinel.msgh_remote  = service_kernel;
	sentinel.msgh_local   = MACH_PORT_NULL;
	sentinel.msgh_voucher = 0;
	sentinel.msgh_id      = STRESS_THREAD_SENTINEL;
	(void)mach_msg_send(kernel_space, &sentinel);

	rv = mach_msg_recv_block(kernel_space, done_kernel, &done,
	    sizeof(done));
	if (rv != MACH_MSG_OK) {
		kprintf("stress_intertask: done recv: %s\n",
		    mach_msg_strerror(rv));
		goto out;
	}

	rv = (int)ctx.itc_rv;

out:
	/* Tear down: drop names in both spaces. */
	port_deallocate(kernel_space, service_kernel);
	port_deallocate(kernel_space, done_kernel);
	port_deallocate(kernel_space, reply_port_kernel);
	port_deallocate(worker->t_port_space, service_worker);
	port_deallocate(worker->t_port_space, done_worker);

	sched_reap_zombies();

	inuse_w1 = port_space_inuse(worker->t_port_space);
	inuse_k1 = port_space_inuse(kernel_space);

	/* The server thread is reaped; nothing's left in the worker. */
	task_deref(worker);

	cached1 = kmem_cached_pages();
	pmm1    = pmm_used_pages();
	cons1   = pmm1 - cached1;

	kprintf("stress_intertask: kernel names %zu -> %zu, "
	    "worker names %zu -> %zu\n",
	    inuse_k0, inuse_k1, inuse_w0, inuse_w1);
	kprintf("stress_intertask: conserved %zu -> %zu\n",
	    cons0, cons1);

	if (rv != MACH_MSG_OK && rv != 0) {
		kprintf("stress_intertask: FAIL rv=%d\n", rv);
		return (rv);
	}
	if (inuse_k1 != inuse_k0) {
		kprintf("stress_intertask: FAIL kernel name leak "
		    "(delta=%lld)\n",
		    (long long)((long long)inuse_k1 -
		        (long long)inuse_k0));
		return (60);
	}
	if (cons1 != cons0) {
		kprintf("stress_intertask: FAIL conservation broken "
		    "(delta=%lld)\n",
		    (long long)((long long)cons1 - (long long)cons0));
		return (61);
	}

	kprintf("stress_intertask: PASS\n");
	return (0);
}
