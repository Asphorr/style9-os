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
 * Power-of-two bucket sizes from 16 to 2048 bytes; requests above
 * that fall through to direct pmm_alloc_pages().  Every chunk carries
 * a small header before the caller-visible pointer that records the
 * bucket index (or page-multiple count) plus a redzone magic, so
 * kfree() can route the freed chunk back without help from the caller
 * and double-frees / wild pointers are caught with a panic instead of
 * silent corruption.
 *
 * Not callable from interrupt context: the underlying spinlock is not
 * IRQ-safe.  BSD shape: an IRQ handler that wants memory enqueues a
 * task for a thread to run later.
 */

void	 kmem_init(void);
void	*kmalloc(size_t size);
void	*kcalloc(size_t n, size_t size);
void	 kfree(void *p);
void	 kmem_stats(void);

/*
 * Number of pmm pages currently parked in bucket freelists.  A
 * regression that leaks chunks (kfree path bug, dropped pointer) is
 * not visible from pmm alone because kmem caches its refills; this
 * number plus pmm_used gives the conserved quantity stress tests
 * need to check against the baseline.
 */
size_t	 kmem_cached_pages(void);

#endif /* !_SYS_KMEM_H_ */
