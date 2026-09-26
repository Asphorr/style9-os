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
 * The filesystems re-read the same few blocks constantly (an APFS lookup
 * starts at the root node every time), and every read is PIO.  This layer
 * caches fixed 4 KiB pages of a device, keyed by (drive, page), and serves
 * arbitrary sector runs from them, so 4 KiB-block and 512-byte-sector
 * readers share pages.  Replacement is LRU by access stamp over a fixed set
 * of buffers allocated at init: no heap use on the I/O path.
 *
 * Writes go through bio_write, which writes the device and then makes the
 * cache agree.  bio_invalidate_drive is for writers that bypass this layer
 * (the Mach block-device protocol).
 */

/* Cached unit.  Matches the APFS block size, and 8 ATA sectors exactly. */
#define	BIO_PAGE_BYTES		4096
#define	BIO_SECTOR_BYTES	512
#define	BIO_SECTORS_PER_PAGE	(BIO_PAGE_BYTES / BIO_SECTOR_BYTES)

/*
 * 256 KiB.  Measured: mounting both filesystems and walking the APFS
 * volume touches 82 distinct pages, and 256 buffers gave exactly the same
 * hits and disk reads as 64.
 */
#define	BIO_NBUFS		64

/* Allocate the buffers.  Call once, after kmem_init and before any read. */
void	bio_init(void);

/*
 * Read `nsec' 512-byte sectors at `lba' from `drive' into `buf', through
 * the cache; same arguments as ata_kread.  Returns 0, or non-zero on a
 * device error (flattened to 1 when it came through the cache).
 */
int	bio_read(unsigned drive, uint64_t lba, uint32_t nsec, void *buf);

/*
 * Write `nsec' sectors at `lba'.  Returns 0, or the driver's positive
 * error.  Write-through: the disk first, then resident pages are patched
 * with the new bytes rather than dropped, so the metadata a write just
 * walked stays cached.  Nothing is dirty: on 0 the bytes are on the
 * platter, since ata_kwrite ends with FLUSH CACHE.
 */
int	bio_write(unsigned drive, uint64_t lba, uint32_t nsec, const void *buf);

/*
 * Forget everything cached for a device, for writers that bypass this
 * layer (the Mach block-device protocol).
 */
void	bio_invalidate_drive(unsigned drive);

/* Hit/miss counters, for the shell and the boot banner. */
void	bio_stats(void);

#endif /* !_SYS_BIO_H_ */
