/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_ATA_DRV_H_
#define	_SYS_ATA_DRV_H_

/*
 * Legacy ATA / IDE driver.
 *
 * Probes the two ISA-legacy channels (I/O 0x1F0+0x3F6, IRQ14 and
 * 0x170+0x376, IRQ15) for up to four PATA drives (master/slave on
 * each channel) via the ATA IDENTIFY DEVICE command.  Each drive
 * found is registered with the bootstrap port under "dev/disk<N>"
 * and answers the BLOCK device protocol (DEV_OP_GEOM / READ_BLOCK /
 * WRITE_BLOCK / SYNC) defined in dev_proto.h.
 *
 *	- LBA28 + LBA48 addressing, auto-selected per command
 *	- multi-sector transfers up to DEV_BLOCK_MAX_SECTORS per call
 *	- FLUSH CACHE (EXT) after writes; explicit DEV_OP_SYNC
 *	- per-channel spinlock so master and slave serialise
 *
 * Interrupt-driven where the channel raised INTRQ at probe, polled where
 * it did not.  Every wait is bounded and settles against the live status
 * register: an interrupt says a boundary was reached, not which one.
 */

void	ata_drv_init(void);

/*
 * Kernel-side block read, bypassing the Mach device protocol; the in-kernel
 * filesystems reach it through the block cache (bio_read, fs/bio.h).  Reads
 * `count' sectors at `lba' from drive `drive_idx' (0 = first drive found)
 * into `buf'.  Returns 0 or a positive MACH_E_* (no such drive, I/O error,
 * LBA out of range).
 */
int	ata_kread(unsigned drive_idx, uint64_t lba, uint32_t count, void *buf);

/*
 * The other direction, same contract, and no FLUSH CACHE: 0 means the
 * drive accepted the bytes, which may sit in its write cache, in any
 * order, until ata_ksync.  Callers use bio_write and bio_sync (fs/bio.h),
 * since the block cache cannot see a write that goes around it.
 */
int	ata_kwrite(unsigned drive_idx, uint64_t lba, uint32_t count,
	    const void *buf);

/* FLUSH CACHE: every write ata_kwrite returned 0 for is on the medium. */
int	ata_ksync(unsigned drive_idx);

/* Print per-channel interrupt and wait counters. */
void	ata_irq_stats(void);

/*
 * Commands started on a channel that already had one in flight, both
 * channels.  A transfer releases ch_lock while it sleeps for the interrupt,
 * so the channel is unowned mid-command; this counts entries into that
 * window.  Readable at any time.
 */
uint32_t	ata_overlaps(void);

/* Interrupt waits, both channels, that ended at a deadline instead. */
uint32_t	ata_lost_intrs(void);

#endif /* !_SYS_ATA_DRV_H_ */
