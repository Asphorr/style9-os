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
 *
 * A FRAME CAN HAVE MORE THAN ONE OWNER
 *
 * Beside the bitmap there is a second array, one 16-bit count per managed
 * frame, and it answers a question the bitmap cannot: how many owners does
 * this page have.  Everything above the allocator -- copy-on-write after
 * fork, a file's page mapped into several tasks, memory sent out-of-line in
 * a message -- is some arrangement of "the same frame, reachable from more
 * than one place", and every one of those arrangements needs the same thing
 * from here: nobody may return the frame while somebody else still has it.
 *
 * The count is deliberately NOT a new pair of alloc/free entry points.
 * pmm_free_page has always meant "I am done with this frame", and that
 * sentence is still exactly right when the frame has other owners -- it just
 * no longer implies the frame becomes free.  So pmm_free_page drops one
 * reference and returns the frame to the bitmap only when the last one goes,
 * and every existing caller keeps its meaning without being touched.  The
 * only new verb is pmm_page_ref, for the moment a second owner appears.
 *
 * pmm_alloc_page hands back a frame with a count of one, so a system that
 * never shares anything behaves precisely as it did before this existed.
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

/*
 * Drop one reference to `pa`.  The frame goes back to the allocator only
 * when the last reference goes; see the header comment above for why this
 * is spelled as the old free() rather than as a new unref().
 */
void		 pmm_free_page(uint64_t pa);

/*
 * Multi-page free.  Runs are allocated by callers that want physical
 * contiguity (kmem's slabs, DMA buffers) and are never shared page by page,
 * so this asserts each frame has exactly one owner rather than counting
 * down -- a shared frame inside a run would mean somebody handed out part
 * of a contiguous allocation, which is a bug worth catching here.
 */
void		 pmm_free_pages(uint64_t pa, size_t npages);
void		 pmm_reserve(uint64_t base, uint64_t length);

/*
 * Take one more reference to an already-allocated frame.  The caller must
 * hold a reference already (this is not a way to resurrect a free page):
 * the count going from 0 to 1 is pmm_alloc_page's job alone.
 */
void		 pmm_page_ref(uint64_t pa);

/*
 * How many owners `pa` has.  Zero means the frame is not allocated.  The
 * copy-on-write fault asks this to tell "the last writer, who may simply
 * take the page" from "one of several, who must copy it".
 */
uint32_t	 pmm_page_refs(uint64_t pa);

/* Frames with more than one owner right now. */
size_t		 pmm_shared_pages(void);

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
