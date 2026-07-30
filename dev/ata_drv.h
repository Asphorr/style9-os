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
 * Mature first cut:
 *	- LBA28 + LBA48 addressing, auto-selected per command
 *	- multi-sector transfers up to DEV_BLOCK_MAX_SECTORS per call
 *	- READ SECTORS (EXT) + WRITE SECTORS (EXT) commands
 *	- FLUSH CACHE (EXT) after writes; explicit DEV_OP_SYNC support
 *	- IDENTIFY parse: model, total LBA28/LBA48 sectors, LBA48 bit
 *	- per-channel spinlock so master/slave serialise correctly
 *	- error-register decoding on ERR; status discipline with
 *	  400 ns alt-status reads.
 *
 * Interrupt-driven where the channel proved at probe that it actually
 * raises INTRQ, and polled where it did not -- decided from observed
 * behaviour rather than from a table.  Both waits are bounded, and both
 * settle what the drive is doing against the LIVE status register: an
 * interrupt says a boundary was reached, not which one, and a command
 * that was never accepted raises nothing at all.
 */

void	ata_drv_init(void);

/*
 * Direct kernel-side block read, bypassing the Mach device protocol -- the
 * in-kernel filesystem (fs/fat/fat.c) reads sectors with this rather than
 * messaging its own disk service from inside the kernel.  Reads `count`
 * sectors at `lba` from drive `drive_idx` (0 = first ATA drive detected) into
 * `buf`.  Returns 0 on success or a positive MACH_E_* (no such drive, I/O
 * error, LBA out of range).
 */
int	ata_kread(unsigned drive_idx, uint64_t lba, uint32_t count, void *buf);

/*
 * The other direction, same contract.  WRITE SECTORS (EXT) followed by FLUSH
 * CACHE, so a return of 0 means the drive has the bytes rather than merely
 * having accepted them -- which is the difference that matters to a
 * filesystem deciding whether its metadata is safe to point at.
 *
 * Callers go through bio_write (fs/bio.h) rather than here, because the block
 * cache cannot see a write that goes around it and would keep serving what
 * the disk no longer holds.
 */
int	ata_kwrite(unsigned drive_idx, uint64_t lba, uint32_t count,
	    const void *buf);

/* Interrupt accounting, for the boot banner. */
void	ata_irq_stats(void);

/*
 * How many commands have been started on a channel that already had one in
 * flight, across both channels.  A transfer sleeps for the drive's interrupt
 * and gives ch_lock back to do it, so the channel is genuinely unowned in the
 * middle of a command; this counts whether anybody has ever walked into that
 * window.  Readable at any time, because the interesting moment is usually
 * long after the boot banner.
 */
uint32_t	ata_overlaps(void);

/*
 * How many waits for a drive interrupt ended at their deadline instead, across
 * both channels -- each one a command that would have waited for ever under
 * the previous shape of ata_wait_intr.
 */
uint32_t	ata_lost_intrs(void);

#endif /* !_SYS_ATA_DRV_H_ */
