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

#endif /* !_SYS_ATA_DRV_H_ */
