/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_RANDOM_H_
#define	_SYS_RANDOM_H_

#include <stddef.h>
#include <stdint.h>

/*
 * The kernel's random numbers: a ChaCha20 generator keyed from a BLAKE2s
 * pool, in the shape of Linux's crng.  The pool takes what random_init
 * gathers at boot -- the cycle counter's jitter, the clocks, RDRAND where
 * the processor has it -- and then every interrupt's timing.  The
 * generator reseeds from the pool at most once a second and rekeys after
 * every request, so its state never tells what it gave out before.  It
 * never blocks: it is seeded before anything can ask.
 */

/* Gather the boot seed and check the primitives; before any consumer. */
void	random_init(void);

/* Fill `buf` with `n` random bytes.  May spin briefly; never sleeps. */
void	random_bytes(void *buf, size_t n);

/* Stir caller-supplied bytes (a write to /dev/random) into the pool. */
void	random_add(const void *buf, size_t n);

/*
 * At every interrupt: stir the cycle counter, the vector and where it
 * landed into this CPU's word.  No lock; folded in at the next reseed.
 */
void	random_intr(uint64_t vector, uint64_t rip);

#endif /* !_SYS_RANDOM_H_ */
