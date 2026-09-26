/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "memmap.h"
#include "panic.h"
#include "pmm.h"
#include "spinlock.h"

extern char	__kernel_start[];
extern char	__kernel_end[];

/*
 * Lock key:
 *	(p) protected by pmm_lock
 *	(c) const after pmm_init
 */
static uint64_t		*pmm_bitmap;		/* (p) bit set => used    */
static uint64_t		 pmm_bitmap_words;	/* (c) length in u64s     */
static uint64_t		 pmm_managed_pages;	/* (c) total managed      */
static uint64_t		 pmm_managed_pa_end;	/* (c) exclusive end PA   */
static uint64_t		 pmm_alloc_hint;	/* (p) next-fit cursor    */
static uint64_t		 pmm_used_count;	/* (p) currently allocated*/

/*
 * (p) owners per managed frame, by page number.  Sixteen bits: a program
 * forking in a loop can keep far more than 255 sharers of one frame
 * alive.  At the 1 GiB cap the array is 512 KiB.
 */
static uint16_t		*pmm_refs;
static uint64_t		 pmm_shared_count;	/* (p) frames with refs > 1 */
static uint64_t		 pmm_n_ref;		/* (p) pmm_page_ref calls   */
static uint64_t		 pmm_n_lastref;		/* (p) frees that freed     */

static struct spinlock	pmm_lock = SPINLOCK_INIT("pmm");

static void		 mark_used(uint64_t idx);
static void		 mark_free(uint64_t idx);
static bool		 is_used(uint64_t idx);
static void		 mark_range_used(uint64_t base, uint64_t length);
static uint64_t		 carve_bitmap_storage(size_t bytes_needed);
static void		 ref_set(uint64_t idx, uint32_t v);

static inline uint64_t
page_index(uint64_t pa)
{

	return (pa >> PAGE_SHIFT);
}

static inline uint64_t
pa_from_index(uint64_t idx)
{

	return (idx << PAGE_SHIFT);
}

/*
 * Two phases.  Phase 1 finds a home for the bitmap and owner counts in a
 * MEMMAP_FREE region, clear of the low 1 MiB and the kernel image.
 * Phase 2 marks every managed page used, frees the pages of MEMMAP_FREE
 * regions, then reserves the low 1 MiB, the kernel image and the
 * bookkeeping again -- correct whatever the firmware reports.
 */
void
pmm_init(void)
{
	uint64_t	bm_pa, bm_bytes, kstart, kend, hard_cap, max_free_end;
	uint64_t	rc_bytes, carve_bytes;
	size_t		i;

	if (memmap_max_pa == 0)
		panic("pmm_init: memmap_init was not called");

	/*
	 * Manage 0 .. min(highest free end, hard cap).  Pages above the
	 * highest FREE region are reserved or absent, and managing them
	 * would only inflate the used count.
	 */
	hard_cap = PMM_HARD_CAP_BYTES;
	max_free_end = 0;
	for (i = 0; i < memmap_nentries; i++) {
		uint64_t end;
		if (memmap_entries[i].me_type != MEMMAP_FREE)
			continue;
		end = memmap_entries[i].me_base + memmap_entries[i].me_length;
		if (end > max_free_end)
			max_free_end = end;
	}
	if (max_free_end == 0)
		panic("pmm_init: no FREE regions in memmap");

	pmm_managed_pa_end = max_free_end;
	if (pmm_managed_pa_end > hard_cap)
		pmm_managed_pa_end = hard_cap;
	pmm_managed_pa_end = PA_ROUND_UP(pmm_managed_pa_end);

	pmm_managed_pages = pmm_managed_pa_end >> PAGE_SHIFT;
	pmm_bitmap_words  = (pmm_managed_pages + 63) / 64;
	bm_bytes          = pmm_bitmap_words * sizeof(uint64_t);

	/*
	 * The bitmap and owner counts are carved and reserved as one block;
	 * the counts start on the next page boundary, so the two are easy
	 * to tell apart in a memory dump.
	 */
	rc_bytes    = pmm_managed_pages * sizeof(uint16_t);
	carve_bytes = PA_ROUND_UP(bm_bytes) + rc_bytes;

	bm_pa = carve_bitmap_storage(carve_bytes);
	if (bm_pa == PA_INVALID)
		panic("pmm_init: no usable region big enough for "
		    "%llu bytes of frame bookkeeping",
		    (unsigned long long)carve_bytes);

	pmm_bitmap = (uint64_t *)pmm_kva_from_pa(bm_pa);
	pmm_refs   = (uint16_t *)pmm_kva_from_pa(bm_pa + PA_ROUND_UP(bm_bytes));

	/* Phase 2: start every page used, then carve out the free ones. */
	for (i = 0; i < pmm_bitmap_words; i++)
		pmm_bitmap[i] = ~(uint64_t)0;
	pmm_used_count = pmm_managed_pages;

	/*
	 * Zero owners everywhere.  Pages that stay used were never
	 * allocated (firmware holes, the kernel image, this array), and a
	 * zero count lets pmm_free_page assert as much instead of wrapping.
	 */
	for (i = 0; i < pmm_managed_pages; i++)
		pmm_refs[i] = 0;

	for (i = 0; i < memmap_nentries; i++) {
		if (memmap_entries[i].me_type != MEMMAP_FREE)
			continue;

		uint64_t base = PA_ROUND_UP(memmap_entries[i].me_base);
		uint64_t end  = PA_ROUND_DOWN(memmap_entries[i].me_base +
		    memmap_entries[i].me_length);

		if (end > pmm_managed_pa_end)
			end = pmm_managed_pa_end;
		if (base >= end)
			continue;

		for (uint64_t pa = base; pa < end; pa += PAGE_SIZE) {
			uint64_t idx = page_index(pa);
			if (is_used(idx)) {
				mark_free(idx);
				pmm_used_count--;
			}
		}
	}

	/*
	 * The low 1 MiB (BIOS data area, EBDA, video memory) is never handed
	 * out, even when the firmware calls it RAM: 256 pages.
	 */
	mark_range_used(0, 0x100000);

	/* Kernel image. */
	kstart = PA_ROUND_DOWN((uintptr_t)__kernel_start);
	kend   = PA_ROUND_UP((uintptr_t)__kernel_end);
	mark_range_used(kstart, kend - kstart);

	/* Bitmap and owner counts. */
	mark_range_used(bm_pa, carve_bytes);

	pmm_alloc_hint = 0;

	kprintf("pmm: bitmap at 0x%llx (%llu bytes) + %llu bytes of owner "
	    "counts, %llu managed pages, %llu free\n",
	    (unsigned long long)bm_pa,
	    (unsigned long long)bm_bytes,
	    (unsigned long long)rc_bytes,
	    (unsigned long long)pmm_managed_pages,
	    (unsigned long long)(pmm_managed_pages - pmm_used_count));
}

uint64_t
pmm_alloc_page(void)
{
	uint64_t	idx, start;

	spin_lock(&pmm_lock);

	start = pmm_alloc_hint;
	for (uint64_t i = 0; i < pmm_managed_pages; i++) {
		idx = start + i;
		if (idx >= pmm_managed_pages)
			idx -= pmm_managed_pages;

		if (!is_used(idx)) {
			mark_used(idx);
			ref_set(idx, 1);
			pmm_used_count++;
			pmm_alloc_hint = idx + 1;
			if (pmm_alloc_hint >= pmm_managed_pages)
				pmm_alloc_hint = 0;
			spin_unlock(&pmm_lock);
			return (pa_from_index(idx));
		}
	}

	spin_unlock(&pmm_lock);
	return (PA_INVALID);
}

uint64_t
pmm_alloc_pages(size_t npages)
{
	uint64_t	idx, run, start;
	size_t		j;

	if (npages == 0)
		return (PA_INVALID);
	if (npages == 1)
		return (pmm_alloc_page());

	spin_lock(&pmm_lock);

	for (start = pmm_alloc_hint;
	    start + npages <= pmm_managed_pages;
	    start++) {
		run = 0;
		while (run < npages && !is_used(start + run))
			run++;

		if (run == npages) {
			for (j = 0; j < npages; j++) {
				mark_used(start + j);
				ref_set(start + j, 1);
			}
			pmm_used_count += npages;
			pmm_alloc_hint = start + npages;
			idx = start;
			spin_unlock(&pmm_lock);
			return (pa_from_index(idx));
		}

		/* Skip past the occupied page we just hit. */
		start += run;
	}

	/* Restart from the bottom on wrap-around. */
	for (start = 0;
	    start + npages <= pmm_alloc_hint;
	    start++) {
		run = 0;
		while (run < npages && !is_used(start + run))
			run++;

		if (run == npages) {
			for (j = 0; j < npages; j++) {
				mark_used(start + j);
				ref_set(start + j, 1);
			}
			pmm_used_count += npages;
			pmm_alloc_hint = start + npages;
			idx = start;
			spin_unlock(&pmm_lock);
			return (pa_from_index(idx));
		}

		start += run;
	}

	spin_unlock(&pmm_lock);
	return (PA_INVALID);
}

/*
 * "I am done with this frame": drop one owner.  Only the last owner out
 * returns the frame to the bitmap.
 */
void
pmm_free_page(uint64_t pa)
{
	uint64_t	idx;

	KASSERT((pa & PAGE_MASK) == 0, "pmm_free_page: unaligned PA");
	KASSERT(pa < pmm_managed_pa_end, "pmm_free_page: PA out of range");

	idx = page_index(pa);

	spin_lock(&pmm_lock);
	KASSERT(is_used(idx), "pmm_free_page: double-free");
	/*
	 * Used but never allocated (kernel image, low memory, this
	 * bookkeeping): a caller freeing one has mistaken a reserved frame
	 * for an allocated one.  Say so rather than wrap the count.
	 */
	KASSERT(pmm_refs[idx] != 0,
	    "pmm_free_page: frame was reserved, not allocated");

	ref_set(idx, pmm_refs[idx] - 1);
	if (pmm_refs[idx] != 0) {
		spin_unlock(&pmm_lock);
		return;
	}

	pmm_n_lastref++;
	mark_free(idx);
	pmm_used_count--;
	if (idx < pmm_alloc_hint)
		pmm_alloc_hint = idx;
	spin_unlock(&pmm_lock);
}

void
pmm_free_pages(uint64_t pa, size_t npages)
{
	size_t	j;

	KASSERT((pa & PAGE_MASK) == 0, "pmm_free_pages: unaligned PA");

	spin_lock(&pmm_lock);
	for (j = 0; j < npages; j++) {
		uint64_t idx = page_index(pa) + j;
		KASSERT(idx < pmm_managed_pages,
		    "pmm_free_pages: range overruns managed area");
		KASSERT(is_used(idx), "pmm_free_pages: double-free");
		/*
		 * A run is handed out as one thing and never shared page by
		 * page (pmm.h); a shared frame here is a bug to catch, not a
		 * frame to leak.
		 */
		KASSERT(pmm_refs[idx] == 1,
		    "pmm_free_pages: frame inside a run has other owners");
		ref_set(idx, 0);
		mark_free(idx);
	}
	pmm_used_count -= npages;
	pmm_n_lastref += npages;
	if (page_index(pa) < pmm_alloc_hint)
		pmm_alloc_hint = page_index(pa);
	spin_unlock(&pmm_lock);
}

void
pmm_page_ref(uint64_t pa)
{
	uint64_t	idx;

	KASSERT((pa & PAGE_MASK) == 0, "pmm_page_ref: unaligned PA");
	KASSERT(pa < pmm_managed_pa_end, "pmm_page_ref: PA out of range");

	idx = page_index(pa);

	spin_lock(&pmm_lock);
	KASSERT(is_used(idx) && pmm_refs[idx] != 0,
	    "pmm_page_ref: frame is not allocated");
	KASSERT(pmm_refs[idx] != 0xFFFFu, "pmm_page_ref: owner count would wrap");
	ref_set(idx, pmm_refs[idx] + 1);
	pmm_n_ref++;
	spin_unlock(&pmm_lock);
}

uint32_t
pmm_page_refs(uint64_t pa)
{
	uint64_t	idx;
	uint32_t	v;

	if ((pa & PAGE_MASK) != 0 || pa >= pmm_managed_pa_end)
		return (0);

	idx = page_index(pa);

	spin_lock(&pmm_lock);
	v = pmm_refs[idx];
	spin_unlock(&pmm_lock);

	return (v);
}

size_t
pmm_shared_pages(void)
{
	size_t	v;

	spin_lock(&pmm_lock);
	v = (size_t)pmm_shared_count;
	spin_unlock(&pmm_lock);

	return (v);
}

void
pmm_reserve(uint64_t base, uint64_t length)
{

	spin_lock(&pmm_lock);
	mark_range_used(base, length);
	spin_unlock(&pmm_lock);
}

size_t
pmm_total_pages(void)
{

	return ((size_t)pmm_managed_pages);
}

size_t
pmm_free_pages_count(void)
{
	size_t	v;

	spin_lock(&pmm_lock);
	v = (size_t)(pmm_managed_pages - pmm_used_count);
	spin_unlock(&pmm_lock);

	return (v);
}

size_t
pmm_used_pages(void)
{
	size_t	v;

	spin_lock(&pmm_lock);
	v = (size_t)pmm_used_count;
	spin_unlock(&pmm_lock);

	return (v);
}

void
pmm_stats(void)
{
	uint64_t	used, freec, shared, nref, lastref;

	spin_lock(&pmm_lock);
	used    = pmm_used_count;
	freec   = pmm_managed_pages - pmm_used_count;
	shared  = pmm_shared_count;
	nref    = pmm_n_ref;
	lastref = pmm_n_lastref;
	spin_unlock(&pmm_lock);

	kprintf("pmm: %llu pages managed, %llu used (%llu KiB), "
	    "%llu free (%llu KiB)\n",
	    (unsigned long long)pmm_managed_pages,
	    (unsigned long long)used,
	    (unsigned long long)(used << (PAGE_SHIFT - 10)),
	    (unsigned long long)freec,
	    (unsigned long long)(freec << (PAGE_SHIFT - 10)));
	/*
	 * `shared` says frames are shared right now, `nref` that sharing
	 * ever happened.
	 */
	kprintf("pmm: %llu frames shared now, %llu extra owners taken, "
	    "%llu frames released by their last owner\n",
	    (unsigned long long)shared,
	    (unsigned long long)nref,
	    (unsigned long long)lastref);
}

/* ---- internals ---------------------------------------------------------- */

/*
 * Set a frame's owner count and keep pmm_shared_count in step.  Caller
 * holds pmm_lock.  Every write to pmm_refs goes through here.
 */
static void
ref_set(uint64_t idx, uint32_t v)
{

	if (pmm_refs[idx] > 1 && v <= 1)
		pmm_shared_count--;
	else if (pmm_refs[idx] <= 1 && v > 1)
		pmm_shared_count++;
	pmm_refs[idx] = (uint16_t)v;
}

static void
mark_used(uint64_t idx)
{
	uint64_t	w, b;

	w = idx / 64;
	b = idx % 64;
	pmm_bitmap[w] |= ((uint64_t)1 << b);
}

static void
mark_free(uint64_t idx)
{
	uint64_t	w, b;

	w = idx / 64;
	b = idx % 64;
	pmm_bitmap[w] &= ~((uint64_t)1 << b);
}

static bool
is_used(uint64_t idx)
{
	uint64_t	w, b;

	w = idx / 64;
	b = idx % 64;
	return ((pmm_bitmap[w] & ((uint64_t)1 << b)) != 0);
}

static void
mark_range_used(uint64_t base, uint64_t length)
{
	uint64_t	start, end, pa;

	if (length == 0)
		return;

	start = PA_ROUND_DOWN(base);
	end   = PA_ROUND_UP(base + length);

	if (start >= pmm_managed_pa_end)
		return;
	if (end > pmm_managed_pa_end)
		end = pmm_managed_pa_end;

	for (pa = start; pa < end; pa += PAGE_SIZE) {
		uint64_t idx = page_index(pa);
		if (!is_used(idx)) {
			mark_used(idx);
			pmm_used_count++;
		}
	}
}

/*
 * Find a home for `bytes_needed` of bookkeeping: the start of the first
 * MEMMAP_FREE region, clipped above the low 1 MiB and past the kernel
 * image, with that much room.  PA_INVALID if none.
 */
static uint64_t
carve_bitmap_storage(size_t bytes_needed)
{
	uint64_t	kstart, kend, base, end, candidate;
	size_t		i;

	kstart = PA_ROUND_DOWN((uintptr_t)__kernel_start);
	kend   = PA_ROUND_UP((uintptr_t)__kernel_end);

	for (i = 0; i < memmap_nentries; i++) {
		if (memmap_entries[i].me_type != MEMMAP_FREE)
			continue;

		base = PA_ROUND_UP(memmap_entries[i].me_base);
		end  = PA_ROUND_DOWN(memmap_entries[i].me_base +
		    memmap_entries[i].me_length);

		if (base < 0x100000)
			base = 0x100000;

		/* Place after the kernel if the region straddles it. */
		if (base < kend && end > kstart)
			base = kend;

		if (end > pmm_managed_pa_end)
			end = pmm_managed_pa_end;

		if (base >= end)
			continue;
		if (end - base < bytes_needed)
			continue;

		candidate = base;
		return (candidate);
	}

	return (PA_INVALID);
}
