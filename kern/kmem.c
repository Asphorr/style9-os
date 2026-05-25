/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "pmm.h"
#include "spinlock.h"

/*
 * Bucket sizes are 2^KMEM_MIN_LOG2 .. 2^KMEM_MAX_LOG2.  KMEM_HDR_SIZE
 * is consumed before the caller-visible pointer; bucket capacity is
 * (bucket_size - KMEM_HDR_SIZE).
 *
 * KMEM_HDR_MAGIC sits in the redzone slot and is checked on every
 * kfree(); a mismatch panics with a pointer + caller-RA.
 */
#define	KMEM_MIN_LOG2		4		/* 16 bytes      */
#define	KMEM_MAX_LOG2		11		/* 2048 bytes    */
#define	KMEM_NBUCKETS		(KMEM_MAX_LOG2 - KMEM_MIN_LOG2 + 1)

#define	KMEM_HDR_MAGIC		0x4B4D4348u	/* 'K' 'M' 'C' 'H' */
#define	KMEM_BUCKET_LARGE	0xFFFFu		/* outside bucket table */

struct kmem_hdr {
	uint16_t	kh_bucket;	/* bucket index or KMEM_BUCKET_LARGE */
	uint16_t	kh_npages;	/* page count when kh_bucket == LARGE */
	uint32_t	kh_magic;	/* KMEM_HDR_MAGIC                     */
};

#define	KMEM_HDR_SIZE	((size_t)sizeof(struct kmem_hdr))
_Static_assert(KMEM_HDR_SIZE == 8, "kmem header expected to be 8 bytes");

/*
 * Free chunk: when a bucket chunk is sitting on the freelist, we
 * overlay this struct on top of its payload area to thread the list.
 * The header stays valid; only the post-header bytes are reused.
 */
struct kmem_chunk {
	struct kmem_chunk	*kc_next;
};

struct kmem_bucket {
	size_t			 kb_size;	/* chunk size in bytes */
	struct kmem_chunk	*kb_free;	/* SLL of free chunks  */
	uint64_t		 kb_alloc;	/* total ever-allocated */
	uint64_t		 kb_freecnt;	/* on the free list now */
	uint64_t		 kb_inuse;	/* alloc - freecnt      */
};

/* Lock key: (p) protected by kmem_lock. */
static struct kmem_bucket	kmem_buckets[KMEM_NBUCKETS];	/* (p) */
static uint64_t			kmem_large_chunks;		/* (p) */
static uint64_t			kmem_large_pages;		/* (p) */
static struct spinlock		kmem_lock = SPINLOCK_INIT("kmem");

static void		 refill_bucket(struct kmem_bucket *bk);
static struct kmem_bucket *bucket_for(size_t size);
static void		*kmalloc_large(size_t size);
static void		 kfree_large(struct kmem_hdr *hdr);

void
kmem_init(void)
{
	size_t	i;

	for (i = 0; i < KMEM_NBUCKETS; i++) {
		kmem_buckets[i].kb_size    = (size_t)1 << (KMEM_MIN_LOG2 + i);
		kmem_buckets[i].kb_free    = NULL;
		kmem_buckets[i].kb_alloc   = 0;
		kmem_buckets[i].kb_freecnt = 0;
		kmem_buckets[i].kb_inuse   = 0;
	}
	kmem_large_chunks = 0;
	kmem_large_pages  = 0;

	kprintf("kmem: %zu buckets, sizes %zu..%zu bytes\n",
	    (size_t)KMEM_NBUCKETS,
	    kmem_buckets[0].kb_size,
	    kmem_buckets[KMEM_NBUCKETS - 1].kb_size);
}

void *
kmalloc(size_t size)
{
	struct kmem_bucket	*bk;
	struct kmem_chunk	*c;
	struct kmem_hdr		*hdr;
	size_t			 need;
	void			*payload;

	if (size == 0)
		return (NULL);

	need = size + KMEM_HDR_SIZE;
	bk = bucket_for(need);
	if (bk == NULL)
		return (kmalloc_large(size));

	spin_lock(&kmem_lock);

	if (bk->kb_free == NULL)
		refill_bucket(bk);

	c = bk->kb_free;
	if (c == NULL) {
		spin_unlock(&kmem_lock);
		return (NULL);
	}
	bk->kb_free = c->kc_next;
	bk->kb_freecnt--;
	bk->kb_inuse++;

	spin_unlock(&kmem_lock);

	hdr = (struct kmem_hdr *)c;
	hdr->kh_bucket = (uint16_t)(bk - kmem_buckets);
	hdr->kh_npages = 0;
	hdr->kh_magic  = KMEM_HDR_MAGIC;

	payload = (uint8_t *)hdr + KMEM_HDR_SIZE;
	return (payload);
}

void *
kcalloc(size_t n, size_t size)
{
	size_t		 total, i;
	uint8_t		*p;

	if (n != 0 && size > (size_t)-1 / n)
		return (NULL);
	total = n * size;

	p = (uint8_t *)kmalloc(total);
	if (p == NULL)
		return (NULL);

	for (i = 0; i < total; i++)
		p[i] = 0;
	return (p);
}

void
kfree(void *p)
{
	struct kmem_hdr		*hdr;
	struct kmem_chunk	*c;
	struct kmem_bucket	*bk;

	if (p == NULL)
		return;

	hdr = (struct kmem_hdr *)((uint8_t *)p - KMEM_HDR_SIZE);
	if (hdr->kh_magic != KMEM_HDR_MAGIC)
		panic("kfree(%p): bad magic 0x%08x (double-free or wild ptr)",
		    p, hdr->kh_magic);

	if (hdr->kh_bucket == KMEM_BUCKET_LARGE) {
		kfree_large(hdr);
		return;
	}

	if (hdr->kh_bucket >= KMEM_NBUCKETS)
		panic("kfree(%p): bogus bucket index %u",
		    p, hdr->kh_bucket);

	bk = &kmem_buckets[hdr->kh_bucket];
	c  = (struct kmem_chunk *)hdr;

	/* Poison the magic so a double-free trips the check above. */
	hdr->kh_magic = 0;

	spin_lock(&kmem_lock);
	c->kc_next = bk->kb_free;
	bk->kb_free = c;
	bk->kb_freecnt++;
	bk->kb_inuse--;
	spin_unlock(&kmem_lock);
}

size_t
kmem_cached_pages(void)
{
	size_t	total, chunks_per_page, i;

	spin_lock(&kmem_lock);
	total = 0;
	for (i = 0; i < KMEM_NBUCKETS; i++) {
		chunks_per_page = (size_t)PAGE_SIZE / kmem_buckets[i].kb_size;
		if (chunks_per_page == 0)
			continue;
		total += (size_t)((kmem_buckets[i].kb_alloc +
		    chunks_per_page - 1) / chunks_per_page);
	}
	spin_unlock(&kmem_lock);
	return (total);
}

void
kmem_stats(void)
{
	size_t			 i;
	uint64_t		 total_in, total_pg, large_pg, large_n;
	struct kmem_bucket	 snap[KMEM_NBUCKETS];

	spin_lock(&kmem_lock);
	for (i = 0; i < KMEM_NBUCKETS; i++)
		snap[i] = kmem_buckets[i];
	large_pg = kmem_large_pages;
	large_n  = kmem_large_chunks;
	spin_unlock(&kmem_lock);

	total_in = 0;
	total_pg = 0;

	kprintf("kmem stats:\n");
	for (i = 0; i < KMEM_NBUCKETS; i++) {
		kprintf("  bucket %2zu  %5zu B  inuse=%llu free=%llu "
		    "alloc=%llu\n",
		    i, snap[i].kb_size,
		    (unsigned long long)snap[i].kb_inuse,
		    (unsigned long long)snap[i].kb_freecnt,
		    (unsigned long long)snap[i].kb_alloc);
		total_in += snap[i].kb_inuse * snap[i].kb_size;
	}
	total_pg += large_pg;
	kprintf("  large allocs: %llu live, %llu pages\n",
	    (unsigned long long)large_n, (unsigned long long)large_pg);
	kprintf("  bucket inuse total: %llu bytes\n",
	    (unsigned long long)total_in);
}

/* ---- internals ---------------------------------------------------------- */

static struct kmem_bucket *
bucket_for(size_t size)
{
	size_t	i;

	for (i = 0; i < KMEM_NBUCKETS; i++) {
		if (size <= kmem_buckets[i].kb_size)
			return (&kmem_buckets[i]);
	}
	return (NULL);
}

/*
 * Refill: allocate a fresh page from pmm, slice it into chunks of the
 * bucket's size, thread them onto the freelist.  Must be called with
 * kmem_lock held.
 */
static void
refill_bucket(struct kmem_bucket *bk)
{
	uint64_t		 pa;
	uint8_t			*p;
	size_t			 chunks, i;
	struct kmem_chunk	*c;

	pa = pmm_alloc_page();
	if (pa == PA_INVALID)
		return;

	p = (uint8_t *)pmm_kva_from_pa(pa);
	chunks = (size_t)PAGE_SIZE / bk->kb_size;
	for (i = 0; i < chunks; i++) {
		c = (struct kmem_chunk *)(p + i * bk->kb_size);
		c->kc_next = bk->kb_free;
		bk->kb_free = c;
	}
	bk->kb_alloc   += chunks;
	bk->kb_freecnt += chunks;
}

/*
 * Large allocations: round up to a multiple of pages, ask pmm for a
 * contiguous run, drop our header into the first KMEM_HDR_SIZE bytes.
 * Single page is the most common path -- kmalloc(8 KiB) for stacks,
 * etc.
 */
static void *
kmalloc_large(size_t size)
{
	size_t		 need, npages;
	uint64_t	 pa;
	uint8_t		*base;
	struct kmem_hdr	*hdr;

	need = size + KMEM_HDR_SIZE;
	npages = (need + PAGE_SIZE - 1) / PAGE_SIZE;
	if (npages > 0xFFFFu)
		return (NULL);

	pa = pmm_alloc_pages(npages);
	if (pa == PA_INVALID)
		return (NULL);

	base = (uint8_t *)pmm_kva_from_pa(pa);
	hdr  = (struct kmem_hdr *)base;
	hdr->kh_bucket = KMEM_BUCKET_LARGE;
	hdr->kh_npages = (uint16_t)npages;
	hdr->kh_magic  = KMEM_HDR_MAGIC;

	spin_lock(&kmem_lock);
	kmem_large_chunks++;
	kmem_large_pages += npages;
	spin_unlock(&kmem_lock);

	return (base + KMEM_HDR_SIZE);
}

static void
kfree_large(struct kmem_hdr *hdr)
{
	size_t		npages;
	uint64_t	pa;

	npages = hdr->kh_npages;
	pa = (uint64_t)(uintptr_t)hdr;
	KASSERT((pa & PAGE_MASK) == 0,
	    "kfree_large: header not on page boundary");

	/* Poison before release. */
	hdr->kh_magic = 0;

	spin_lock(&kmem_lock);
	kmem_large_chunks--;
	kmem_large_pages -= npages;
	spin_unlock(&kmem_lock);

	pmm_free_pages(pa, npages);
}
