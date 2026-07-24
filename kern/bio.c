/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ata_drv.h"
#include "bio.h"
#include "kmem.h"
#include "kprintf.h"
#include "spinlock.h"

/* See bio.h for what this is and why. */

struct bio_buf {
	uint64_t	bb_page;	/* device page number            */
	uint64_t	bb_stamp;	/* LRU: last access              */
	uint8_t		*bb_data;	/* BIO_PAGE_BYTES of it          */
	unsigned	bb_drive;
	bool		bb_valid;
};

/* (b) protected by bio_lock. */
static struct spinlock	bio_lock;
static struct bio_buf	bio_bufs[BIO_NBUFS];		/* (b) */
static uint8_t		*bio_arena;			/* (i) one slab   */
static uint64_t		bio_clock;			/* (b) LRU stamp  */
static bool		bio_ready;			/* (i) */

/* Counters.  (b) -- read under the lock in bio_stats. */
static uint64_t		bio_n_req;	/* sector runs asked for         */
static uint64_t		bio_n_hit;	/* pages served from cache       */
static uint64_t		bio_n_miss;	/* pages fetched from the device */
static uint64_t		bio_n_evict;	/* live pages thrown out         */

static void
mem_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

void
bio_init(void)
{
	size_t	i;

	spin_init(&bio_lock, "bio");

	/*
	 * One slab, sliced.  Allocating BIO_NBUFS separate 4 KiB blocks would
	 * round each up to two pages in the large-allocation path and waste
	 * half the cache on headers.
	 */
	bio_arena = kmalloc((size_t)BIO_NBUFS * BIO_PAGE_BYTES);
	if (bio_arena == NULL) {
		kprintf("bio: no memory for %u buffers -- cache disabled\n",
		    (unsigned)BIO_NBUFS);
		return;
	}
	for (i = 0; i < BIO_NBUFS; i++) {
		bio_bufs[i].bb_data  = bio_arena + i * BIO_PAGE_BYTES;
		bio_bufs[i].bb_valid = false;
	}
	bio_ready = true;
	kprintf("bio: %u x %u B block cache (%u KiB)\n", (unsigned)BIO_NBUFS,
	    (unsigned)BIO_PAGE_BYTES,
	    (unsigned)((BIO_NBUFS * BIO_PAGE_BYTES) / 1024));
}

/*
 * Find the buffer holding (drive, page), or make one hold it.  Called with
 * bio_lock held; may perform device I/O, which is why the lock is held across
 * it -- two readers racing for the same page would otherwise both fetch it,
 * and the loser's copy would be the one left in the cache.
 */
static struct bio_buf *
page_get(unsigned drive, uint64_t page)
{
	struct bio_buf	*victim;
	size_t		 i;
	int		 rv;

	for (i = 0; i < BIO_NBUFS; i++) {
		if (bio_bufs[i].bb_valid && bio_bufs[i].bb_page == page &&
		    bio_bufs[i].bb_drive == drive) {
			bio_bufs[i].bb_stamp = ++bio_clock;
			bio_n_hit++;
			return (&bio_bufs[i]);
		}
	}

	/* Miss.  Take a free buffer, else the least recently used one. */
	victim = NULL;
	for (i = 0; i < BIO_NBUFS; i++) {
		if (!bio_bufs[i].bb_valid) {
			victim = &bio_bufs[i];
			break;
		}
		if (victim == NULL || bio_bufs[i].bb_stamp < victim->bb_stamp)
			victim = &bio_bufs[i];
	}
	if (victim->bb_valid)
		bio_n_evict++;

	/*
	 * Mark it invalid BEFORE the read: if the device fails we must not
	 * leave a buffer claiming to hold a page whose contents are stale or
	 * half-transferred.
	 */
	victim->bb_valid = false;
	rv = ata_kread(drive, page * BIO_SECTORS_PER_PAGE,
	    BIO_SECTORS_PER_PAGE, victim->bb_data);
	if (rv != 0)
		return (NULL);

	victim->bb_page  = page;
	victim->bb_drive = drive;
	victim->bb_stamp = ++bio_clock;
	victim->bb_valid = true;
	bio_n_miss++;
	return (victim);
}

int
bio_read(unsigned drive, uint64_t lba, uint32_t nsec, void *buf)
{
	struct bio_buf	*bb;
	uint8_t		*out;
	uint64_t	 page;
	uint32_t	 done;
	uint32_t	 within;
	uint32_t	 run;

	if (!bio_ready)				/* no cache: straight through */
		return (ata_kread(drive, lba, nsec, buf));
	if (nsec == 0)
		return (0);

	out = buf;
	spin_lock(&bio_lock);
	bio_n_req++;
	for (done = 0; done < nsec; done += run) {
		page   = (lba + done) / BIO_SECTORS_PER_PAGE;
		within = (uint32_t)((lba + done) % BIO_SECTORS_PER_PAGE);
		run    = BIO_SECTORS_PER_PAGE - within;
		if (run > nsec - done)
			run = nsec - done;

		bb = page_get(drive, page);
		if (bb == NULL) {
			spin_unlock(&bio_lock);
			return (1);		/* the driver's error, flattened */
		}
		mem_copy(out + (size_t)done * BIO_SECTOR_BYTES,
		    bb->bb_data + (size_t)within * BIO_SECTOR_BYTES,
		    (size_t)run * BIO_SECTOR_BYTES);
	}
	spin_unlock(&bio_lock);
	return (0);
}

void
bio_invalidate_drive(unsigned drive)
{
	size_t	i;

	if (!bio_ready)
		return;
	spin_lock(&bio_lock);
	for (i = 0; i < BIO_NBUFS; i++)
		if (bio_bufs[i].bb_drive == drive)
			bio_bufs[i].bb_valid = false;
	spin_unlock(&bio_lock);
}

void
bio_stats(void)
{
	uint64_t	req, hit, miss, evict, total, pct;
	size_t		i, live;

	if (!bio_ready) {
		kprintf("bio: cache disabled\n");
		return;
	}
	spin_lock(&bio_lock);
	req   = bio_n_req;
	hit   = bio_n_hit;
	miss  = bio_n_miss;
	evict = bio_n_evict;
	live  = 0;
	for (i = 0; i < BIO_NBUFS; i++)
		if (bio_bufs[i].bb_valid)
			live++;
	spin_unlock(&bio_lock);

	total = hit + miss;
	pct = (total != 0) ? (hit * 100) / total : 0;
	kprintf("bio: %llu requests, %llu page lookups -- %llu hit (%llu%%), "
	    "%llu read from disk, %llu evicted, %u/%u buffers live\n",
	    (unsigned long long)req, (unsigned long long)total,
	    (unsigned long long)hit, (unsigned long long)pct,
	    (unsigned long long)miss, (unsigned long long)evict,
	    (unsigned)live, (unsigned)BIO_NBUFS);
}
