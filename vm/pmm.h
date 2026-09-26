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
 * Physical memory manager: a bitmap-backed first-fit allocator.
 *
 * One bit per 4 KiB frame, set when allocated, covering physical
 * addresses 0 .. min(memmap_max_pa, PMM_HARD_CAP_BYTES).  The cap is the
 * reach of the boot identity map; memory above it is not addressable
 * yet.  Lift it once pmap publishes a direct map.
 *
 * All state is under pmm_lock.  Interrupt handlers must not allocate.
 *
 * A frame can have more than one owner.  Beside the bitmap is a 16-bit
 * count per frame: copy-on-write after fork and OOL message pages both
 * make one frame reachable from several places, and it must not be
 * freed while anyone still has it.  pmm_alloc_page returns a count of
 * one; pmm_page_ref adds an owner; pmm_free_page means "I am done with
 * this frame" and drops one reference, freeing the frame with the last.
 */

#define	PAGE_SHIFT		12
#define	PAGE_SIZE		((uint64_t)1 << PAGE_SHIFT)
#define	PAGE_MASK		(PAGE_SIZE - 1)

#define	PA_INVALID		((uint64_t)0)
#define	PA_ROUND_DOWN(x)	((uint64_t)(x) & ~PAGE_MASK)
#define	PA_ROUND_UP(x)		(((uint64_t)(x) + PAGE_MASK) & ~PAGE_MASK)

/*
 * The boot identity map's reach: the 1 GiB boot.S maps with 2 MiB pages.
 * Memory above it is parsed and reported, not allocated.
 */
#define	PMM_HARD_CAP_BYTES	((uint64_t)1 << 30)

void		 pmm_init(void);
uint64_t	 pmm_alloc_page(void);
uint64_t	 pmm_alloc_pages(size_t npages);

/* Drop one reference to `pa`; the last one frees the frame. */
void		 pmm_free_page(uint64_t pa);

/*
 * Free a run from pmm_alloc_pages.  Runs (kmem slabs, DMA buffers) are
 * never shared page by page, so each frame must have exactly one owner;
 * this asserts it rather than counting down.
 */
void		 pmm_free_pages(uint64_t pa, size_t npages);
void		 pmm_reserve(uint64_t base, uint64_t length);

/*
 * One more reference to a frame the caller already holds one on; 0 -> 1
 * is pmm_alloc_page's alone.
 */
void		 pmm_page_ref(uint64_t pa);

/*
 * How many owners `pa` has; 0 if it is not allocated.  The copy-on-write
 * fault asks, to tell a last owner (who keeps the page) from one of
 * several (who copies it).
 */
uint32_t	 pmm_page_refs(uint64_t pa);

/* Frames with more than one owner right now. */
size_t		 pmm_shared_pages(void);

size_t		 pmm_total_pages(void);
size_t		 pmm_free_pages_count(void);
size_t		 pmm_used_pages(void);
void		 pmm_stats(void);

/*
 * A managed physical address as a kernel VA, through the boot identity
 * map.  Everyone goes through here, so a higher-half direct map is a
 * one-function change.
 */
static inline void *
pmm_kva_from_pa(uint64_t pa)
{

	return ((void *)(uintptr_t)pa);
}

/*
 * The same identity the other way, for vm_map_image, which maps a program
 * image's frames out of the kernel image instead of copying them.  Named
 * for the same reason as its mirror: under a direct map an open-coded cast
 * would be silently wrong.
 */
static inline uint64_t
pmm_pa_from_kva(const void *kva)
{

	return ((uint64_t)(uintptr_t)kva);
}

#endif /* !_SYS_PMM_H_ */
