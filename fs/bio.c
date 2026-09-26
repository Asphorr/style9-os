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
#include "sched.h"
#include "spinlock.h"

/* See bio.h for what this is and why. */

struct bio_buf {
	uint64_t	bb_page;	/* device page number            */
	uint64_t	bb_stamp;	/* LRU: last access              */
	uint8_t		*bb_data;	/* BIO_PAGE_BYTES of it          */
	unsigned	bb_drive;
	bool		bb_valid;
	bool		bb_busy;	/* claimed, fetch in progress    */
	/*
	 * Set when a write lands on this page while a fetch of it is in
	 * flight: the fetch may have read the pre-write bytes, so the fetcher
	 * discards its result instead of caching it.
	 */
	bool		bb_stale;
};

/* (b) protected by bio_lock; (i) set once in bio_init. */
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
static uint64_t		bio_n_write;	/* sector runs written           */
static uint64_t		bio_n_patch;	/* resident pages a write fixed  */

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
	 * One slab, sliced: separate 4 KiB allocations would each round up
	 * to two pages in the large-allocation path.
	 */
	bio_arena = kmalloc((size_t)BIO_NBUFS * BIO_PAGE_BYTES);
	if (bio_arena == NULL) {
		kprintf("bio: no memory for %u buffers -- cache disabled\n",
		    (unsigned)BIO_NBUFS);
		return;
	}
	/*
	 * An identity no disk has.  A zeroed buffer would claim page 0 of
	 * drive 0 -- the APFS container superblock, rewritten at every
	 * checkpoint -- and a lookup by (drive, page) would match the empty
	 * buffer instead of the one holding that page.
	 */
	for (i = 0; i < BIO_NBUFS; i++) {
		bio_bufs[i].bb_data  = bio_arena + i * BIO_PAGE_BYTES;
		bio_bufs[i].bb_page  = (uint64_t)-1;
		bio_bufs[i].bb_drive = (unsigned)-1;
		bio_bufs[i].bb_valid = false;
	}
	bio_ready = true;
	kprintf("bio: %u x %u B block cache (%u KiB)\n", (unsigned)BIO_NBUFS,
	    (unsigned)BIO_PAGE_BYTES,
	    (unsigned)((BIO_NBUFS * BIO_PAGE_BYTES) / 1024));
}

/*
 * Look up (drive, page), fetching it if absent.  Called with bio_lock held;
 * returns with it held, but may drop it in between.
 *
 * It must be dropped for the fetch: the driver sleeps for the disk
 * interrupt, and a spinlock (which holds off interrupts and preemption on
 * its CPU) must never be held across a sleep.  So a miss claims its buffer
 * with bb_busy, drops the lock, fetches, and retakes it.  bb_busy keeps
 * another thread from claiming the buffer or reading it half-filled; a
 * thread that finds it busy waits with the lock dropped and retries
 * (*retry set, NULL returned).
 */
static struct bio_buf *
page_get(unsigned drive, uint64_t page, bool *retry)
{
	struct bio_buf	*victim;
	size_t		 i;
	int		 rv;

	*retry = false;
	for (i = 0; i < BIO_NBUFS; i++) {
		if (bio_bufs[i].bb_page != page || bio_bufs[i].bb_drive != drive)
			continue;
		if (bio_bufs[i].bb_busy) {
			/*
			 * Someone else is fetching it; wait outside the lock.
			 * The fetcher sleeps on the disk, so this CPU's queue
			 * may be empty and the yield return at once; then nap,
			 * rather than spin on the lock the fetcher needs.
			 */
			spin_unlock(&bio_lock);
			if (!thread_yield())
				sched_nap_ms(1);
			spin_lock(&bio_lock);
			*retry = true;
			return (NULL);
		}
		if (bio_bufs[i].bb_valid) {
			bio_bufs[i].bb_stamp = ++bio_clock;
			bio_n_hit++;
			return (&bio_bufs[i]);
		}
	}

	/* Miss.  Take a free buffer, else the least recently used one. */
	victim = NULL;
	for (i = 0; i < BIO_NBUFS; i++) {
		if (bio_bufs[i].bb_busy)
			continue;
		if (!bio_bufs[i].bb_valid) {
			victim = &bio_bufs[i];
			break;
		}
		if (victim == NULL || bio_bufs[i].bb_stamp < victim->bb_stamp)
			victim = &bio_bufs[i];
	}
	if (victim == NULL) {			/* every buffer is in flight */
		spin_unlock(&bio_lock);
		if (!thread_yield())
			sched_nap_ms(1);
		spin_lock(&bio_lock);
		*retry = true;
		return (NULL);
	}
	if (victim->bb_valid)
		bio_n_evict++;

	/*
	 * Claim it: invalid (no one reads it), busy (no one takes it), and
	 * already named, so a concurrent lookup of the page waits instead of
	 * starting a second fetch.
	 */
	victim->bb_valid = false;
	victim->bb_busy  = true;
	victim->bb_stale = false;
	victim->bb_page  = page;
	victim->bb_drive = drive;

	spin_unlock(&bio_lock);
	rv = ata_kread(drive, page * BIO_SECTORS_PER_PAGE,
	    BIO_SECTORS_PER_PAGE, victim->bb_data);
	spin_lock(&bio_lock);

	victim->bb_busy = false;
	if (rv != 0) {
		/* Leave nothing behind claiming to hold this page. */
		victim->bb_page = (uint64_t)-1;
		return (NULL);
	}
	if (victim->bb_stale) {
		/*
		 * Written while the read was in flight, so the result may
		 * predate the write.  Drop it; the retry re-reads the disk.
		 */
		victim->bb_stale = false;
		victim->bb_page  = (uint64_t)-1;
		*retry = true;
		return (NULL);
	}
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
	bool		 retry;

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

		bb = page_get(drive, page, &retry);
		if (retry) {
			run = 0;		/* nothing consumed; go round again */
			continue;
		}
		if (bb == NULL) {
			spin_unlock(&bio_lock);
			return (1);		/* the driver's error, flattened */
		}
		/*
		 * Copy under the lock; page_get returned a valid, non-busy
		 * buffer.
		 */
		mem_copy(out + (size_t)done * BIO_SECTOR_BYTES,
		    bb->bb_data + (size_t)within * BIO_SECTOR_BYTES,
		    (size_t)run * BIO_SECTOR_BYTES);
	}
	spin_unlock(&bio_lock);
	return (0);
}

int
bio_write(unsigned drive, uint64_t lba, uint32_t nsec, const void *buf)
{
	const uint8_t	*in;
	uint64_t	 page;
	uint32_t	 done;
	uint32_t	 within;
	uint32_t	 run;
	size_t		 i;
	int		 rv;

	if (nsec == 0)
		return (0);

	/*
	 * The device first, with no lock held: ata_kwrite sleeps (see
	 * page_get).
	 */
	rv = ata_kwrite(drive, lba, nsec, buf);
	if (rv != 0)
		return (rv);
	if (!bio_ready)
		return (0);

	/*
	 * Then make the cache agree.  Done second so a failed write never
	 * leaves the cache holding bytes the disk did not take.
	 */
	in = buf;
	spin_lock(&bio_lock);
	bio_n_write++;
	for (done = 0; done < nsec; done += run) {
		page   = (lba + done) / BIO_SECTORS_PER_PAGE;
		within = (uint32_t)((lba + done) % BIO_SECTORS_PER_PAGE);
		run    = BIO_SECTORS_PER_PAGE - within;
		if (run > nsec - done)
			run = nsec - done;

		for (i = 0; i < BIO_NBUFS; i++) {
			if (bio_bufs[i].bb_page != page ||
			    bio_bufs[i].bb_drive != drive)
				continue;
			if (bio_bufs[i].bb_busy) {
				bio_bufs[i].bb_stale = true;
				break;
			}
			/*
			 * Nothing to patch here, but keep scanning: an invalid
			 * buffer naming the page does not mean no other buffer
			 * holds it.
			 */
			if (!bio_bufs[i].bb_valid)
				continue;
			mem_copy(bio_bufs[i].bb_data +
			    (size_t)within * BIO_SECTOR_BYTES,
			    in + (size_t)done * BIO_SECTOR_BYTES,
			    (size_t)run * BIO_SECTOR_BYTES);
			bio_n_patch++;
			break;
		}
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
	uint64_t	req, hit, miss, evict, wr, patch, total, pct;
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
	wr    = bio_n_write;
	patch = bio_n_patch;
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
	if (wr != 0)
		kprintf("bio: %llu writes -- %llu resident pages patched\n",
		    (unsigned long long)wr, (unsigned long long)patch);
}
