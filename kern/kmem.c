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
 * (bucket_size - KMEM_HDR_SIZE).  Bucket 0 (16 bytes) is the smallest
 * bucket; with a 16-byte header it has zero payload room and is
 * unreachable from bucket_for().  Kept in the array so existing
 * stats / printf indexing stays stable.
 *
 * KMEM_HDR_MAGIC sits in the header and is checked on every kfree;
 * mismatch panics (double-free or wild pointer).
 *
 * KMEM_RZ_PATTERN fills the trailing slack between the caller's
 * payload end and the bucket's chunk end on every alloc -- on free we
 * verify it's intact, which catches buffer-overrun writes.
 *
 * KMEM_POISON_PATTERN fills the entire payload AND slack on free
 * (after the red-zone check).  When the chunk gets pulled from the
 * freelist for the next alloc, the new kmalloc verifies it's still
 * intact, catching use-after-free writes during the freed lifetime.
 */
#define	KMEM_MIN_LOG2		4		/* 16 bytes      */
#define	KMEM_MAX_LOG2		11		/* 2048 bytes    */
#define	KMEM_NBUCKETS		(KMEM_MAX_LOG2 - KMEM_MIN_LOG2 + 1)

#define	KMEM_HDR_MAGIC		0x4B4D4348u	/* 'K' 'M' 'C' 'H' */
#define	KMEM_BUCKET_LARGE	0xFFFFu		/* outside bucket table */
#define	KMEM_RZ_PATTERN		0xFEu		/* trailing red zone   */
#define	KMEM_POISON_PATTERN	0xDEu		/* freed-payload fill  */

struct kmem_hdr {
	uint16_t	kh_bucket;	/* bucket index or KMEM_BUCKET_LARGE */
	uint16_t	kh_npages;	/* page count when kh_bucket == LARGE */
	uint32_t	kh_magic;	/* KMEM_HDR_MAGIC                     */
	uint64_t	kh_size;	/* caller-requested bytes (red-zone)  */
};

#define	KMEM_HDR_SIZE	((size_t)sizeof(struct kmem_hdr))
_Static_assert(KMEM_HDR_SIZE == 16, "kmem header expected to be 16 bytes");

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
static void		 paint_redzone(struct kmem_hdr *hdr, size_t bucket_size);
static void		 verify_redzone(struct kmem_hdr *hdr, size_t bucket_size);
static void		 paint_poison(struct kmem_hdr *hdr, size_t bucket_size);
static void		 verify_poison(struct kmem_chunk *c, size_t bucket_size);

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
	size_t			 bucket_size;
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
	bucket_size = bk->kb_size;

	spin_unlock(&kmem_lock);

	/*
	 * Verify the freed-chunk poison is still intact: if anyone
	 * scribbled into this chunk during its dead lifetime (classic
	 * use-after-free), the byte-by-byte poison check trips here and
	 * we panic before handing the corrupted pointer to a new caller.
	 * refill_bucket paints fresh chunks with poison too, so this
	 * check is uniform across first-life and recycled paths.
	 */
	verify_poison(c, bucket_size);

	hdr = (struct kmem_hdr *)c;

	hdr->kh_bucket = (uint16_t)(bk - kmem_buckets);
	hdr->kh_npages = 0;
	hdr->kh_magic  = KMEM_HDR_MAGIC;
	hdr->kh_size   = (uint64_t)size;

	paint_redzone(hdr, bucket_size);

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
	size_t			 bucket_size;

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
	bucket_size = bk->kb_size;
	c  = (struct kmem_chunk *)hdr;

	/*
	 * Red-zone check: panic if the caller wrote past the end of
	 * their allocation.  Done before any header mutation so the
	 * autopsy sees the pristine kh_size + bucket.
	 */
	verify_redzone(hdr, bucket_size);

	/*
	 * Poison the entire chunk payload so a use-after-free write
	 * during the freed lifetime is caught at the next kmalloc that
	 * pulls this chunk.  Then poison the magic so a double-free
	 * trips the magic check above instead.  Order matters --
	 * paint_poison stomps the magic if we did it first.
	 */
	paint_poison(hdr, bucket_size);
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
 *
 * Paint the freed-state poison pattern onto every chunk before
 * linking so verify_poison in kmalloc works uniformly for first-life
 * and recycled paths.  Only the bytes past the kc_next slot are
 * painted; the link itself is set by the assignment that follows.
 */
static void
refill_bucket(struct kmem_bucket *bk)
{
	uint64_t		 pa;
	uint8_t			*p;
	size_t			 chunks, i, j;
	struct kmem_chunk	*c;
	uint8_t			*body;

	pa = pmm_alloc_page();
	if (pa == PA_INVALID)
		return;

	p = (uint8_t *)pmm_kva_from_pa(pa);
	chunks = (size_t)PAGE_SIZE / bk->kb_size;
	for (i = 0; i < chunks; i++) {
		c = (struct kmem_chunk *)(p + i * bk->kb_size);
		body = (uint8_t *)c + sizeof(struct kmem_chunk);
		for (j = sizeof(struct kmem_chunk); j < bk->kb_size; j++)
			*body++ = KMEM_POISON_PATTERN;
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

/* ---- red-zone + freelist poison ---------------------------------- */

/*
 * Fill the slack between the end of the caller's payload and the end
 * of the bucket chunk with KMEM_RZ_PATTERN.  On free we check that
 * pattern is intact; any byte that's been overwritten is a buffer
 * overflow we'd otherwise never see.
 */
static void
paint_redzone(struct kmem_hdr *hdr, size_t bucket_size)
{
	uint8_t	*payload;
	uint8_t	*slack;
	uint8_t	*end;

	payload = (uint8_t *)hdr + KMEM_HDR_SIZE;
	slack   = payload + hdr->kh_size;
	end     = (uint8_t *)hdr + bucket_size;
	while (slack < end)
		*slack++ = KMEM_RZ_PATTERN;
}

static void
verify_redzone(struct kmem_hdr *hdr, size_t bucket_size)
{
	uint8_t	*payload;
	uint8_t	*slack;
	uint8_t	*end;
	size_t	 off;

	payload = (uint8_t *)hdr + KMEM_HDR_SIZE;
	slack   = payload + hdr->kh_size;
	end     = (uint8_t *)hdr + bucket_size;
	off = 0;
	while (slack + off < end) {
		if (slack[off] != KMEM_RZ_PATTERN) {
			panic("kfree(%p): red-zone violation at +%zu "
			    "(byte 0x%02x, expected 0x%02x); "
			    "%llu-byte alloc overran its %zu-byte bucket",
			    (void *)payload, off,
			    (unsigned)slack[off],
			    (unsigned)KMEM_RZ_PATTERN,
			    (unsigned long long)hdr->kh_size,
			    bucket_size);
		}
		off++;
	}
}

/*
 * Fill the entire chunk past the kc_next link slot with the freed-
 * state poison pattern.  Symmetric with verify_poison (which reads
 * the same range) and with refill_bucket (which paints fresh chunks
 * the same way); without that symmetry the kh_size slot at offset 8
 * leaks through verify_poison's check on the next alloc.
 *
 * Caller then overwrites bytes [0, 8) when it assigns kc_next on its
 * way to the freelist, so the freelist link survives.
 */
static void
paint_poison(struct kmem_hdr *hdr, size_t bucket_size)
{
	uint8_t	*p;
	uint8_t	*end;

	p   = (uint8_t *)hdr + sizeof(struct kmem_chunk);
	end = (uint8_t *)hdr + bucket_size;
	while (p < end)
		*p++ = KMEM_POISON_PATTERN;
}

/*
 * Walk the chunk that just came off the freelist and ensure every byte
 * past the kc_next slot still matches KMEM_POISON_PATTERN.  If any
 * byte differs, somebody wrote through a freed pointer between kfree
 * and this kmalloc -- panic with the offset so the corrupting writer
 * has a fingerprint to chase.
 */
static void
verify_poison(struct kmem_chunk *c, size_t bucket_size)
{
	uint8_t	*p;
	uint8_t	*end;
	size_t	 off;

	/* Skip kc_next slot (first sizeof(void *) bytes). */
	p   = (uint8_t *)c + sizeof(struct kmem_chunk);
	end = (uint8_t *)c + bucket_size;
	off = sizeof(struct kmem_chunk);
	while (p < end) {
		if (*p != KMEM_POISON_PATTERN) {
			panic("kmalloc: freed-poison violation at chunk=%p "
			    "+%zu (byte 0x%02x, expected 0x%02x); "
			    "use-after-free into a %zu-byte bucket chunk",
			    (void *)c, off,
			    (unsigned)*p,
			    (unsigned)KMEM_POISON_PATTERN,
			    bucket_size);
		}
		p++;
		off++;
	}
}
