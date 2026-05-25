/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_PMM_H_
#define	_SYS_PMM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/*
 * Physical Memory Manager: bitmap-backed first-fit allocator.
 *
 * One bit per 4 KiB frame, bit-set == allocated.  The bitmap covers
 * physical addresses 0 .. min(memmap_max_pa, PMM_HARD_CAP_BYTES).  The
 * upper cap exists because the boot identity map only reaches 1 GiB;
 * pages beyond that exist in the firmware map but are not yet
 * VA-accessible.  Lift the cap once pmap publishes a direct map.
 *
 * Lock-key on global state is "pmm_lock"; all single-page and multi-
 * page operations take it.  IRQ context must not allocate (allocation
 * would re-enter the lock if an IRQ handler tries to fault in a page);
 * BSD discipline.
 */

#define	PAGE_SHIFT		12
#define	PAGE_SIZE		((uint64_t)1 << PAGE_SHIFT)
#define	PAGE_MASK		(PAGE_SIZE - 1)

#define	PA_INVALID		((uint64_t)0)
#define	PA_ROUND_DOWN(x)	((uint64_t)(x) & ~PAGE_MASK)
#define	PA_ROUND_UP(x)		(((uint64_t)(x) + PAGE_MASK) & ~PAGE_MASK)

/*
 * Limit imposed by the boot identity map.  Sized 1 GiB to match the
 * range that boot.S maps with 2 MiB huge pages.  Memory above this is
 * still parsed and reported, just not allocated.
 */
#define	PMM_HARD_CAP_BYTES	((uint64_t)1 << 30)

void		 pmm_init(void);
uint64_t	 pmm_alloc_page(void);
uint64_t	 pmm_alloc_pages(size_t npages);
void		 pmm_free_page(uint64_t pa);
void		 pmm_free_pages(uint64_t pa, size_t npages);
void		 pmm_reserve(uint64_t base, uint64_t length);

size_t		 pmm_total_pages(void);
size_t		 pmm_free_pages_count(void);
size_t		 pmm_used_pages(void);
void		 pmm_stats(void);

/*
 * pmm_kva_from_pa: convert a managed physical address to a kernel
 * virtual address using the boot identity map.  Single chokepoint
 * for the day pmap publishes a higher-half DMAP -- everyone funnels
 * through here so the cutover is one function.
 */
static inline void *
pmm_kva_from_pa(uint64_t pa)
{

	return ((void *)(uintptr_t)pa);
}

#endif /* !_SYS_PMM_H_ */
