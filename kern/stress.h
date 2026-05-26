/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_STRESS_H_
#define	_SYS_STRESS_H_

#include <stdint.h>

/*
 * In-kernel stress tests.
 *
 * Each routine returns 0 on success, non-zero on the first detected
 * regression.  They are intended to be invoked from the shell (see
 * the 'stress' command in cmds.c) but are equally usable from kmain
 * for boot-time smoke tests.
 *
 *	stress_mem(N)		mixed-size alloc/free churn for N
 *				iterations; verifies allocation count
 *				equals free count and the pmm used-page
 *				delta is within the cached-bucket budget.
 *
 *	stress_mem_boundary()	allocates every interesting size around
 *				bucket boundaries, scribbles a known
 *				pattern across the full extent, verifies
 *				it survived, then frees.
 *
 *	stress_timer(seconds)	measures observed PIT ticks-per-second
 *				against the configured rate, with
 *				kmalloc/kfree load applied; reports the
 *				ppm error.
 */

int	stress_mem(unsigned int iterations);
int	stress_mem_boundary(void);
int	stress_timer(unsigned int seconds);
int	stress_port(unsigned int rounds);
int	stress_thread(unsigned int rounds);
int	stress_preempt(unsigned int n_workers, unsigned int sleep_ms);
int	stress_sendonce(unsigned int rounds);
int	stress_portset(unsigned int n_members, unsigned int per_member);
int	stress_intertask(unsigned int rounds);
int	stress_moverecv(unsigned int rounds);
int	stress_nosenders(unsigned int rounds);
int	stress_sendblock(unsigned int rounds);
int	stress_rpc(unsigned int rounds);

#endif /* !_SYS_STRESS_H_ */
