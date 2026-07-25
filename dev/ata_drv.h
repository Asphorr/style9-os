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
 * Polled (no IRQ wiring yet); a follow-up commit can swap the wait
 * loops for IRQ14/15-driven completion ports.
 */

void	ata_drv_init(void);

/*
 * Direct kernel-side block read, bypassing the Mach device protocol -- the
 * in-kernel filesystem (kern/fs_fat.c) reads sectors with this rather than
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

#endif /* !_SYS_ATA_DRV_H_ */
