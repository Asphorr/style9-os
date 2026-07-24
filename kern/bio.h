/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_BIO_H_
#define	_SYS_BIO_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Block cache.
 *
 * Every filesystem here reads the disk the same way and none of them
 * remembers anything: walking an APFS B-tree re-reads the root node on every
 * lookup, and a directory listing re-walks the whole tree once per entry, so
 * the same handful of blocks come off the platter over and over.  The driver
 * underneath is PIO -- each transfer is a loop of `insw` with the channel
 * spinlock held -- so a re-read is not merely wasteful, it stalls the machine.
 *
 * This sits between the two.  It caches fixed 4 KiB pages of a device, keyed
 * by (drive, page), and serves arbitrary sector runs out of them: a filesystem
 * that reads 4 KiB blocks and one that reads 512-byte sectors both hit the
 * same pages.  Replacement is LRU by access stamp over a small fixed set of
 * buffers, all allocated at init -- nothing here calls the heap on the I/O
 * path, and nothing grows without bound.
 *
 * It is a READ cache and says so: bio_invalidate_drive drops everything for a
 * device, and the write path calls it, because a cache that survives a write
 * it did not see is a cache that hands back a lie.
 */

/* Cached unit.  Matches the APFS block size, and 8 ATA sectors exactly. */
#define	BIO_PAGE_BYTES		4096
#define	BIO_SECTOR_BYTES	512
#define	BIO_SECTORS_PER_PAGE	(BIO_PAGE_BYTES / BIO_SECTOR_BYTES)

/*
 * How many pages to keep: 256 KiB of a 127 MiB machine.  Measured, not
 * guessed.  Mounting both filesystems and walking the APFS volume touches 82
 * distinct pages and asks for 225; at this size the surplus asks all hit and
 * the 18 evictions cost nothing, because what gets evicted is the streamed
 * file data nobody reads twice.  Raising it to 256 buffers was tried: zero
 * evictions, and the identical 143 hits and 82 disk reads.  Four times the
 * memory bought nothing, so this stays small.
 */
#define	BIO_NBUFS		64

/* Allocate the buffers.  Call once, after kmem_init and before any read. */
void	bio_init(void);

/*
 * Read `nsec` 512-byte sectors starting at `lba` from `drive` into `buf`,
 * through the cache.  Returns 0 on success, or the driver's positive error.
 * Drop-in for ata_kread with the same argument order.
 */
int	bio_read(unsigned drive, uint64_t lba, uint32_t nsec, void *buf);

/*
 * Forget everything cached for a device.  The write path must call this: this
 * layer never sees writes, so it cannot know which pages they invalidated.
 */
void	bio_invalidate_drive(unsigned drive);

/* Hit/miss counters, for the shell and the boot banner. */
void	bio_stats(void);

#endif /* !_SYS_BIO_H_ */
