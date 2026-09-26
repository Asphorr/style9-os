/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_KMEM_H_
#define	_SYS_KMEM_H_

#include <stddef.h>

/*
 * Small-object kernel allocator.
 *
 * Power-of-two buckets from 16 to 2048 bytes; larger requests go straight
 * to pmm_alloc_pages().  A header before the caller's pointer records the
 * bucket index (or page count) and a magic, so kfree() needs no size and
 * a double free or wild pointer panics instead of corrupting.  Red zones
 * and free-chunk poison catch overruns and use-after-free (kmem.c).
 *
 * Not for interrupt context: BSD shape, an IRQ handler that wants memory
 * hands the work to a thread.
 */

void	 kmem_init(void);
void	*kmalloc(size_t size);
void	*kcalloc(size_t n, size_t size);
void	 kfree(void *p);
void	 kmem_stats(void);

/*
 * Pages the buckets have taken from pmm (in use or free; they are never
 * given back).  Because of that caching a chunk leak is invisible to pmm
 * alone: stress tests check that pmm_used_pages() minus this is unchanged.
 */
size_t	 kmem_cached_pages(void);

#endif /* !_SYS_KMEM_H_ */
