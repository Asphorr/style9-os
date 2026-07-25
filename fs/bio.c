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
	 * flight.  The fetch may already have pulled the pre-write bytes off
	 * the platter, so the buffer it is about to publish could be older
	 * than the disk; the fetcher checks this on the way out and throws its
	 * result away instead of caching a stale page.  Without it the window
	 * is small, silent, and produces a cache that disagrees with a disk
	 * nobody wrote to twice.
	 */
	bool		bb_stale;
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
 * Look up (drive, page), fetching it if absent.  Called with bio_lock held;
 * returns with it held, having possibly DROPPED it in the middle.
 *
 * Dropping it is not an optimisation, it is a correctness requirement.  The
 * driver underneath sleeps waiting for the disk interrupt, and in this kernel
 * the preempt counter is global: a thread that blocks while holding any
 * spinlock leaves the count above zero, and the deferred wake queue is only
 * drained when the count reaches zero.  Blocking with bio_lock held therefore
 * parks the thread and guarantees nothing will ever wake it.  Holding a
 * spinlock across device I/O is not slow here -- it is fatal.
 *
 * So a miss claims its buffer with a busy flag, drops the lock, fetches, and
 * takes the lock back.  The flag is what keeps a second thread from claiming
 * the same buffer or reading half-filled bytes out of it; a thread that finds
 * one busy yields and retries, which is legal precisely because it is holding
 * no lock at that point.
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
			/* Someone else is fetching it; wait outside the lock. */
			spin_unlock(&bio_lock);
			thread_yield();
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
		thread_yield();
		spin_lock(&bio_lock);
		*retry = true;
		return (NULL);
	}
	if (victim->bb_valid)
		bio_n_evict++;

	/*
	 * Claim it: invalid (so no one reads it) and busy (so no one takes
	 * it), with its identity already set so a concurrent lookup for the
	 * same page finds it busy rather than starting a second fetch.
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
		 * Someone wrote this page while the read was in flight, so what
		 * came back may predate their bytes.  Drop it and make the
		 * caller ask again -- the retry re-reads a disk that now holds
		 * the write, which is the only version worth caching.
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
		 * Copy under the lock, and only from a buffer that is valid
		 * and not busy -- page_get guarantees both, and the copy is
		 * short enough that holding the lock for it costs nothing.
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
	 * The device first, and with no lock held -- ata_kwrite sleeps waiting
	 * for the disk interrupt, and in this kernel the preempt counter is
	 * global, so blocking under bio_lock would park this thread where
	 * nothing can wake it.  Same rule as the fetch in page_get, same
	 * reason.
	 */
	rv = ata_kwrite(drive, lba, nsec, buf);
	if (rv != 0)
		return (rv);
	if (!bio_ready)
		return (0);

	/*
	 * Now make the cache agree.  Doing this second means a reader racing
	 * the write sees either the old bytes or the new ones, never a page
	 * this layer invented; doing it first would let a failed write leave
	 * the cache holding bytes the disk never took.
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
			if (!bio_bufs[i].bb_valid)
				break;
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
