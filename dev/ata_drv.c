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
#include "dev_proto.h"
#include "dev_subsystem.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "port.h"
#include "spinlock.h"

extern struct port	*port_create_kernel_owned(uint8_t kind, void *arg);

/*
 * ATA register offsets from the channel's I/O base (0x1F0 / 0x170).
 *	data    16-bit data port; insw / outsw a sector here
 *	error   read-only on read, write-only feature on write
 *	count   sector count (LBA28); for LBA48 the high byte is latched
 *	        in by writing twice (HOB write order)
 *	lbalo,
 *	lbamid,
 *	lbahi   the three LBA bytes (LBA28).  For LBA48 each one gets
 *	        written twice (high byte first, then low byte).
 *	drive   drive-select / head: 0xA0 | (slave?0x10:0) | (LBA28?0x40:0)
 *	command read-only status, write-only command.
 *
 * The control register (alt-status / device-control) lives on a
 * separate I/O range -- 0x3F6 for the primary channel, 0x376 for the
 * secondary.  Reading it does not clear the pending IRQ, which is why
 * status polls hit alt-status instead of the main status register.
 */
#define	ATA_REG_DATA		0
#define	ATA_REG_ERROR		1
#define	ATA_REG_FEATURE		1
#define	ATA_REG_COUNT		2
#define	ATA_REG_LBALO		3
#define	ATA_REG_LBAMID		4
#define	ATA_REG_LBAHI		5
#define	ATA_REG_DRIVE		6
#define	ATA_REG_STATUS		7
#define	ATA_REG_COMMAND		7

/* Control register offsets (from ctrl_base). */
#define	ATA_CTL_ALT_STATUS	0
#define	ATA_CTL_DEV_CONTROL	0

/* Status register bits. */
#define	ATA_SR_ERR		0x01
#define	ATA_SR_IDX		0x02
#define	ATA_SR_CORR		0x04
#define	ATA_SR_DRQ		0x08
#define	ATA_SR_DSC		0x10
#define	ATA_SR_DF		0x20
#define	ATA_SR_DRDY		0x40
#define	ATA_SR_BSY		0x80

/* Error register bits. */
#define	ATA_ER_AMNF		0x01	/* address mark not found     */
#define	ATA_ER_TK0NF		0x02	/* track 0 not found          */
#define	ATA_ER_ABRT		0x04	/* command aborted            */
#define	ATA_ER_MCR		0x08	/* media change requested     */
#define	ATA_ER_IDNF		0x10	/* ID not found               */
#define	ATA_ER_MC		0x20	/* media changed              */
#define	ATA_ER_UNC		0x40	/* uncorrectable data error   */
#define	ATA_ER_BBK		0x80	/* bad block                  */

/* ATA commands. */
#define	ATA_CMD_READ_SECTORS		0x20
#define	ATA_CMD_READ_SECTORS_EXT	0x24
#define	ATA_CMD_WRITE_SECTORS		0x30
#define	ATA_CMD_WRITE_SECTORS_EXT	0x34
#define	ATA_CMD_FLUSH_CACHE		0xE7
#define	ATA_CMD_FLUSH_CACHE_EXT		0xEA
#define	ATA_CMD_IDENTIFY		0xEC

/* Device-control bits. */
#define	ATA_DCR_NIEN		0x02	/* mask channel IRQ           */
#define	ATA_DCR_SRST		0x04	/* software reset             */

/* Drive/head register: 0xA0 base | LBA-mode bit | slave bit. */
#define	ATA_DRV_BASE		0xA0
#define	ATA_DRV_LBA		0x40
#define	ATA_DRV_SLAVE		0x10

#define	ATA_SECTOR_BYTES	512u

/*
 * Up to two channels, each with master + slave.  Probed at init; only
 * drives that respond to IDENTIFY get an entry in `drives`.
 */
#define	ATA_NDRIVES_MAX		4

/* Lock key:
 *	(c) const after probe
 *	(l) protected by ch_lock of the owning channel
 */
struct ata_channel {
	struct spinlock	 ch_lock;
	uint16_t	 ch_io_base;	/* (c) e.g. 0x1F0           */
	uint16_t	 ch_ctrl_base;	/* (c) e.g. 0x3F6           */
};

struct ata_drive {
	struct ata_channel	*d_ch;		/* (c) channel              */
	bool			 d_slave;	/* (c) master vs slave      */
	bool			 d_present;	/* (c)                       */
	bool			 d_lba48;	/* (c) supports LBA48 cmds  */
	uint32_t		 d_lba28_sectors;	/* (c) IDENTIFY w60-w61 */
	uint64_t		 d_lba48_sectors;	/* (c) IDENTIFY w100-w103 */
	char			 d_model[40];	/* (c) NUL-padded            */
	char			 d_devname[8];	/* (c) "disk0" .. "disk3"   */
};

static struct ata_channel	channels[2] = {
	{ SPINLOCK_INIT("ata-ch0"), 0x1F0, 0x3F6 },
	{ SPINLOCK_INIT("ata-ch1"), 0x170, 0x376 },
};

static struct ata_drive		drives[ATA_NDRIVES_MAX];

static int	ata_dispatch_for_drive(struct ata_drive *d,
		    const struct mach_msg_header *req,
		    struct port_space *from);

/*
 * Per-drive dispatcher trampolines.  Each binds one drive index and
 * forwards to ata_dispatch_for_drive.  Non-static so PORT_SPECIAL_SERVICE
 * can hold the function pointer in p_special_arg.
 */
int	ata_dispatch_disk0(const struct mach_msg_header *, struct port_space *);
int	ata_dispatch_disk1(const struct mach_msg_header *, struct port_space *);
int	ata_dispatch_disk2(const struct mach_msg_header *, struct port_space *);
int	ata_dispatch_disk3(const struct mach_msg_header *, struct port_space *);

static bool	ata_identify(struct ata_drive *d);
static int	ata_read(struct ata_drive *d, uint64_t lba, uint32_t count,
		    void *buf);
static int	ata_write(struct ata_drive *d, uint64_t lba, uint32_t count,
		    const void *buf);
static int	ata_sync(struct ata_drive *d);

static void	ata_select_drive(struct ata_drive *d, uint8_t extra);
static int	ata_wait_ready(struct ata_channel *ch, uint8_t want, uint8_t *sr_out);
static void	ata_400ns(struct ata_channel *ch);
static const char *ata_decode_err(uint8_t er);

/* ---- init ---------------------------------------------------------- */

void
ata_drv_init(void)
{
	struct ata_drive	*d;
	struct port		*ctl;
	size_t			 ci;
	size_t			 ndrives;
	int			 rv;

	ndrives = 0;

	for (ci = 0; ci < 2; ci++) {
		/*
		 * Software reset on the channel.  Pulse SRST in the
		 * device-control register, wait, release.  This puts both
		 * master and slave into a known state before IDENTIFY.
		 * nIEN is set so the channel does not assert IRQs while
		 * we poll.
		 */
		outb(channels[ci].ch_ctrl_base + ATA_CTL_DEV_CONTROL,
		    ATA_DCR_SRST | ATA_DCR_NIEN);
		ata_400ns(&channels[ci]);
		outb(channels[ci].ch_ctrl_base + ATA_CTL_DEV_CONTROL,
		    ATA_DCR_NIEN);
		ata_400ns(&channels[ci]);

		for (size_t slv = 0; slv < 2 && ndrives < ATA_NDRIVES_MAX;
		    slv++) {
			d = &drives[ndrives];
			d->d_ch     = &channels[ci];
			d->d_slave  = (slv != 0);
			d->d_present = false;

			if (!ata_identify(d))
				continue;

			d->d_present = true;
			d->d_devname[0] = 'd';
			d->d_devname[1] = 'i';
			d->d_devname[2] = 's';
			d->d_devname[3] = 'k';
			d->d_devname[4] = (char)('0' + ndrives);
			d->d_devname[5] = '\0';

			kprintf("ata: %s = %s (LBA28=%u, LBA48=%llu, "
			    "ext=%s)\n",
			    d->d_devname, d->d_model,
			    (unsigned)d->d_lba28_sectors,
			    (unsigned long long)d->d_lba48_sectors,
			    d->d_lba48 ? "yes" : "no");

			/*
			 * Stand up the control port.  We need both the
			 * dispatcher function AND the per-drive identity at
			 * dispatch time, but PORT_SPECIAL_SERVICE only lets
			 * us stash one pointer in p_special_arg (the fn).
			 * Four thin trampolines bind one drive index each;
			 * the right trampoline is picked here based on
			 * enumeration order.
			 */
			{
				void *fn = NULL;
				switch (ndrives) {
				case 0: fn = (void *)(uintptr_t)ata_dispatch_disk0; break;
				case 1: fn = (void *)(uintptr_t)ata_dispatch_disk1; break;
				case 2: fn = (void *)(uintptr_t)ata_dispatch_disk2; break;
				case 3: fn = (void *)(uintptr_t)ata_dispatch_disk3; break;
				}
				ctl = port_create_kernel_owned(
				    PORT_SPECIAL_SERVICE, fn);
				if (ctl == NULL)
					panic("ata_drv_init: port_create");
			}

			rv = dev_register(d->d_devname, ctl);
			if (rv != MACH_MSG_OK)
				panic("ata_drv_init: dev_register %s (rv=%d)",
				    d->d_devname, rv);

			ndrives++;
		}
	}

	if (ndrives == 0)
		kprintf("ata: no drives detected\n");
}

/*
 * The dispatcher pattern we use everywhere else stores a single
 * function pointer in p_special_arg, but we need to know WHICH drive
 * to act on.  Four thin trampolines bind one drive index each; init
 * picks the right one based on enumeration order.
 *
 * Tried packing both into one pointer (drive index in low bits) but
 * the special-port intercept dereferences as a function pointer
 * directly, and we'd lose type safety for a small win.
 */
int
ata_dispatch_disk0(const struct mach_msg_header *req, struct port_space *from)
{
	return (ata_dispatch_for_drive(&drives[0], req, from));
}
int
ata_dispatch_disk1(const struct mach_msg_header *req, struct port_space *from)
{
	return (ata_dispatch_for_drive(&drives[1], req, from));
}
int
ata_dispatch_disk2(const struct mach_msg_header *req, struct port_space *from)
{
	return (ata_dispatch_for_drive(&drives[2], req, from));
}
int
ata_dispatch_disk3(const struct mach_msg_header *req, struct port_space *from)
{
	return (ata_dispatch_for_drive(&drives[3], req, from));
}

static int
ata_dispatch_for_drive(struct ata_drive *d, const struct mach_msg_header *req,
    struct port_space *from)
{
	uint64_t	 lba;
	uint32_t	 count;
	int		 rv;

	if (d == NULL || !d->d_present)
		return (MACH_E_DEAD);

	switch (req->msgh_id) {
	case DEV_OP_INFO:
		return (dev_reply_info(req, from,
		    d->d_devname, DEV_KIND_BLOCK,
		    DEV_F_READABLE | DEV_F_WRITABLE));

	case DEV_OP_GEOM: {
		uint8_t			 buf[sizeof(struct mach_msg_header) +
					     sizeof(struct dev_geom_reply)];
		struct mach_msg_header	*rhdr;
		struct dev_geom_reply	*body;
		size_t			 i;

		rhdr = (struct mach_msg_header *)buf;
		body = (struct dev_geom_reply *)
		    (buf + sizeof(struct mach_msg_header));

		body->dgr_rv            = MACH_MSG_OK;
		body->dgr_sector_bytes  = ATA_SECTOR_BYTES;
		body->dgr_total_sectors = d->d_lba48
		    ? d->d_lba48_sectors
		    : (uint64_t)d->d_lba28_sectors;
		body->dgr_flags         = d->d_lba48 ? 1u : 0u;
		body->dgr_pad           = 0;
		for (i = 0; i < sizeof(body->dgr_model); i++)
			body->dgr_model[i] = d->d_model[i];

		rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		rhdr->msgh_size    = sizeof(buf);
		rhdr->msgh_remote  = req->msgh_local;
		rhdr->msgh_local   = MACH_PORT_NULL;
		rhdr->msgh_voucher = 0;
		rhdr->msgh_id      = req->msgh_id;
		return (mach_msg_send(from, rhdr));
	}

	case DEV_OP_READ_BLOCK: {
		const struct dev_block_io_req	*irq;
		uint8_t				 buf[sizeof(struct mach_msg_header) +
						     sizeof(struct dev_block_read_reply)];
		struct mach_msg_header		*rhdr;
		struct dev_block_read_reply	*body;
		const uint8_t			*payload;
		size_t				 i;

		if (req->msgh_size < sizeof(struct mach_msg_header) +
		    sizeof(struct dev_block_io_req))
			return (MACH_E_INVAL);
		payload = (const uint8_t *)req +
		    sizeof(struct mach_msg_header);
		irq = (const struct dev_block_io_req *)payload;

		lba   = irq->dbr_lba;
		count = irq->dbr_count;
		if (count == 0 || count > DEV_BLOCK_MAX_SECTORS)
			return (MACH_E_INVAL);

		rhdr = (struct mach_msg_header *)buf;
		body = (struct dev_block_read_reply *)
		    (buf + sizeof(struct mach_msg_header));

		for (i = 0; i < sizeof(body->dbr_data); i++)
			body->dbr_data[i] = 0;

		rv = ata_read(d, lba, count, body->dbr_data);
		body->dbr_rv    = rv;
		body->dbr_count = (rv == MACH_MSG_OK) ? count : 0;

		rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		rhdr->msgh_size    = sizeof(buf);
		rhdr->msgh_remote  = req->msgh_local;
		rhdr->msgh_local   = MACH_PORT_NULL;
		rhdr->msgh_voucher = 0;
		rhdr->msgh_id      = req->msgh_id;
		return (mach_msg_send(from, rhdr));
	}

	case DEV_OP_WRITE_BLOCK: {
		const struct dev_block_write_req	*wrq;
		uint8_t					 buf[sizeof(struct mach_msg_header) +
							     sizeof(struct dev_block_io_reply)];
		struct mach_msg_header			*rhdr;
		struct dev_block_io_reply		*body;
		const uint8_t				*payload;

		if (req->msgh_size < sizeof(struct mach_msg_header) +
		    sizeof(struct dev_block_write_req))
			return (MACH_E_INVAL);
		payload = (const uint8_t *)req +
		    sizeof(struct mach_msg_header);
		wrq = (const struct dev_block_write_req *)payload;

		lba   = wrq->dbw_lba;
		count = wrq->dbw_count;
		if (count == 0 || count > DEV_BLOCK_MAX_SECTORS)
			return (MACH_E_INVAL);

		rv = ata_write(d, lba, count, wrq->dbw_data);

		rhdr = (struct mach_msg_header *)buf;
		body = (struct dev_block_io_reply *)
		    (buf + sizeof(struct mach_msg_header));
		body->dbr_rv      = rv;
		body->dbr_sectors = (rv == MACH_MSG_OK) ? count : 0;

		rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		rhdr->msgh_size    = sizeof(buf);
		rhdr->msgh_remote  = req->msgh_local;
		rhdr->msgh_local   = MACH_PORT_NULL;
		rhdr->msgh_voucher = 0;
		rhdr->msgh_id      = req->msgh_id;
		return (mach_msg_send(from, rhdr));
	}

	case DEV_OP_SYNC: {
		uint8_t				 buf[sizeof(struct mach_msg_header) +
						     sizeof(struct dev_block_io_reply)];
		struct mach_msg_header		*rhdr;
		struct dev_block_io_reply	*body;

		rv = ata_sync(d);

		rhdr = (struct mach_msg_header *)buf;
		body = (struct dev_block_io_reply *)
		    (buf + sizeof(struct mach_msg_header));
		body->dbr_rv      = rv;
		body->dbr_sectors = 0;

		rhdr->msgh_bits    = MACH_MSGH_BITS(MACH_MSG_TYPE_COPY_SEND, 0);
		rhdr->msgh_size    = sizeof(buf);
		rhdr->msgh_remote  = req->msgh_local;
		rhdr->msgh_local   = MACH_PORT_NULL;
		rhdr->msgh_voucher = 0;
		rhdr->msgh_id      = req->msgh_id;
		return (mach_msg_send(from, rhdr));
	}

	default:
		return (MACH_E_INVAL);
	}
}

/* ---- IDENTIFY ------------------------------------------------------ */

static bool
ata_identify(struct ata_drive *d)
{
	struct ata_channel	*ch;
	uint16_t		 id[256];
	uint8_t			 sr;
	uint8_t			 lo, hi;
	size_t			 i;
	bool			 patapi;

	ch = d->d_ch;

	spin_lock(&ch->ch_lock);

	ata_select_drive(d, 0);
	ata_400ns(ch);

	/* Zero count + LBA registers so we don't confuse the drive. */
	outb(ch->ch_io_base + ATA_REG_COUNT,  0);
	outb(ch->ch_io_base + ATA_REG_LBALO,  0);
	outb(ch->ch_io_base + ATA_REG_LBAMID, 0);
	outb(ch->ch_io_base + ATA_REG_LBAHI,  0);

	outb(ch->ch_io_base + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
	ata_400ns(ch);

	sr = inb(ch->ch_io_base + ATA_REG_STATUS);
	if (sr == 0) {
		spin_unlock(&ch->ch_lock);
		return (false);	/* no device on this slot */
	}

	/*
	 * Poll BSY=0.  If during the wait LBAMID or LBAHI become non-zero
	 * we're talking to an ATAPI device (or SATA in legacy mode) and
	 * IDENTIFY won't return what we expect -- bail and let the caller
	 * mark the slot empty.  A future ATAPI driver can revisit.
	 */
	for (;;) {
		sr = inb(ch->ch_io_base + ATA_REG_STATUS);
		if ((sr & ATA_SR_BSY) == 0)
			break;
		if (sr & ATA_SR_ERR) {
			spin_unlock(&ch->ch_lock);
			return (false);
		}
	}

	lo = inb(ch->ch_io_base + ATA_REG_LBAMID);
	hi = inb(ch->ch_io_base + ATA_REG_LBAHI);
	patapi = (lo != 0 || hi != 0);
	if (patapi) {
		spin_unlock(&ch->ch_lock);
		return (false);
	}

	/* Wait for DRQ or ERR. */
	for (;;) {
		sr = inb(ch->ch_io_base + ATA_REG_STATUS);
		if (sr & ATA_SR_ERR) {
			spin_unlock(&ch->ch_lock);
			return (false);
		}
		if (sr & ATA_SR_DRQ)
			break;
	}

	insw(ch->ch_io_base + ATA_REG_DATA, id, 256);

	spin_unlock(&ch->ch_lock);

	/*
	 * IDENTIFY fields:
	 *	word 27..46	model number (40 ASCII chars, byte-swapped)
	 *	word 60..61	total user-addressable LBA28 sectors (u32 LE)
	 *	word 83 bit 10	1 == supports LBA48
	 *	word 100..103	total LBA48 sectors (u64 LE)
	 */
	d->d_lba28_sectors = (uint32_t)id[60] | ((uint32_t)id[61] << 16);
	d->d_lba48 = ((id[83] & (1u << 10)) != 0);
	d->d_lba48_sectors =
	      (uint64_t)id[100]
	    | ((uint64_t)id[101] << 16)
	    | ((uint64_t)id[102] << 32)
	    | ((uint64_t)id[103] << 48);

	/*
	 * Model words come ASCII-encoded but with each pair byte-swapped
	 * (e.g. "QE" in word 27 is stored as 0x4551).  Unscramble into
	 * d_model -- 40 chars total -- then trim trailing spaces.
	 */
	for (i = 0; i < 20; i++) {
		uint16_t w = id[27 + i];
		d->d_model[i * 2]     = (char)((w >> 8) & 0xFF);
		d->d_model[i * 2 + 1] = (char)(w & 0xFF);
	}
	d->d_model[39] = '\0';
	for (i = 39; i > 0 && d->d_model[i - 1] == ' '; i--)
		d->d_model[i - 1] = '\0';

	return (true);
}

/* ---- READ / WRITE -------------------------------------------------- */

/*
 * Issue a single PIO transfer.  All command + register setup, sector
 * loop, and FLUSH on write are done under ch_lock so a sibling drive
 * on the same channel can't trample the LBA registers in the middle
 * of our transfer.  Returns MACH_MSG_OK or MACH_E_*.
 */
static int
ata_read(struct ata_drive *d, uint64_t lba, uint32_t count, void *buf)
{
	struct ata_channel	*ch;
	uint8_t			*p;
	uint8_t			 sr;
	uint8_t			 er;
	bool			 use_lba48;
	uint32_t		 s;
	uint8_t			 cmd;

	if (count == 0)
		return (MACH_E_INVAL);

	use_lba48 = d->d_lba48 &&
	    (lba >= (1ull << 28) || (lba + count) > (1ull << 28) || count > 256);

	if (!use_lba48) {
		if (lba + count > (uint64_t)d->d_lba28_sectors)
			return (MACH_E_INVAL);
	} else {
		if (lba + count > d->d_lba48_sectors)
			return (MACH_E_INVAL);
	}

	ch = d->d_ch;
	p  = (uint8_t *)buf;

	spin_lock(&ch->ch_lock);

	if (use_lba48) {
		ata_select_drive(d, ATA_DRV_LBA);
		ata_400ns(ch);

		/*
		 * LBA48 register layout requires the high byte of each
		 * field to be written FIRST, then the low byte.  The drive
		 * latches the previous write into the HOB-shadow register
		 * pair on each port access.
		 */
		outb(ch->ch_io_base + ATA_REG_COUNT,
		    (uint8_t)((count >> 8) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBALO,
		    (uint8_t)((lba >> 24) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAMID,
		    (uint8_t)((lba >> 32) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAHI,
		    (uint8_t)((lba >> 40) & 0xFF));

		outb(ch->ch_io_base + ATA_REG_COUNT,
		    (uint8_t)(count & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBALO,
		    (uint8_t)(lba & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAMID,
		    (uint8_t)((lba >> 8) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAHI,
		    (uint8_t)((lba >> 16) & 0xFF));

		cmd = ATA_CMD_READ_SECTORS_EXT;
	} else {
		ata_select_drive(d, ATA_DRV_LBA |
		    (uint8_t)((lba >> 24) & 0x0F));
		ata_400ns(ch);

		outb(ch->ch_io_base + ATA_REG_COUNT,
		    (uint8_t)(count == 256 ? 0 : count));
		outb(ch->ch_io_base + ATA_REG_LBALO,
		    (uint8_t)(lba & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAMID,
		    (uint8_t)((lba >> 8) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAHI,
		    (uint8_t)((lba >> 16) & 0xFF));

		cmd = ATA_CMD_READ_SECTORS;
	}

	outb(ch->ch_io_base + ATA_REG_COMMAND, cmd);

	for (s = 0; s < count; s++) {
		if (ata_wait_ready(ch, ATA_SR_DRQ, &sr) != 0) {
			er = inb(ch->ch_io_base + ATA_REG_ERROR);
			kprintf("ata: %s read sector %llu failed: %s "
			    "(sr=0x%02x er=0x%02x)\n",
			    d->d_devname, (unsigned long long)(lba + s),
			    ata_decode_err(er),
			    (unsigned)sr, (unsigned)er);
			spin_unlock(&ch->ch_lock);
			return (MACH_E_INVAL);
		}

		insw(ch->ch_io_base + ATA_REG_DATA,
		    p + s * ATA_SECTOR_BYTES, ATA_SECTOR_BYTES / 2);
	}

	spin_unlock(&ch->ch_lock);
	return (MACH_MSG_OK);
}

static int
ata_write(struct ata_drive *d, uint64_t lba, uint32_t count, const void *buf)
{
	struct ata_channel	*ch;
	const uint8_t		*p;
	uint8_t			 sr;
	uint8_t			 er;
	bool			 use_lba48;
	uint32_t		 s;
	uint8_t			 cmd;

	if (count == 0)
		return (MACH_E_INVAL);

	use_lba48 = d->d_lba48 &&
	    (lba >= (1ull << 28) || (lba + count) > (1ull << 28) || count > 256);

	if (!use_lba48) {
		if (lba + count > (uint64_t)d->d_lba28_sectors)
			return (MACH_E_INVAL);
	} else {
		if (lba + count > d->d_lba48_sectors)
			return (MACH_E_INVAL);
	}

	ch = d->d_ch;
	p  = (const uint8_t *)buf;

	spin_lock(&ch->ch_lock);

	if (use_lba48) {
		ata_select_drive(d, ATA_DRV_LBA);
		ata_400ns(ch);

		outb(ch->ch_io_base + ATA_REG_COUNT,
		    (uint8_t)((count >> 8) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBALO,
		    (uint8_t)((lba >> 24) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAMID,
		    (uint8_t)((lba >> 32) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAHI,
		    (uint8_t)((lba >> 40) & 0xFF));

		outb(ch->ch_io_base + ATA_REG_COUNT,
		    (uint8_t)(count & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBALO,
		    (uint8_t)(lba & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAMID,
		    (uint8_t)((lba >> 8) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAHI,
		    (uint8_t)((lba >> 16) & 0xFF));

		cmd = ATA_CMD_WRITE_SECTORS_EXT;
	} else {
		ata_select_drive(d, ATA_DRV_LBA |
		    (uint8_t)((lba >> 24) & 0x0F));
		ata_400ns(ch);

		outb(ch->ch_io_base + ATA_REG_COUNT,
		    (uint8_t)(count == 256 ? 0 : count));
		outb(ch->ch_io_base + ATA_REG_LBALO,
		    (uint8_t)(lba & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAMID,
		    (uint8_t)((lba >> 8) & 0xFF));
		outb(ch->ch_io_base + ATA_REG_LBAHI,
		    (uint8_t)((lba >> 16) & 0xFF));

		cmd = ATA_CMD_WRITE_SECTORS;
	}

	outb(ch->ch_io_base + ATA_REG_COMMAND, cmd);

	for (s = 0; s < count; s++) {
		if (ata_wait_ready(ch, ATA_SR_DRQ, &sr) != 0) {
			er = inb(ch->ch_io_base + ATA_REG_ERROR);
			kprintf("ata: %s write sector %llu failed: %s "
			    "(sr=0x%02x er=0x%02x)\n",
			    d->d_devname, (unsigned long long)(lba + s),
			    ata_decode_err(er),
			    (unsigned)sr, (unsigned)er);
			spin_unlock(&ch->ch_lock);
			return (MACH_E_INVAL);
		}

		outsw(ch->ch_io_base + ATA_REG_DATA,
		    p + s * ATA_SECTOR_BYTES, ATA_SECTOR_BYTES / 2);
	}

	/*
	 * After the last sector, flush the drive's write cache.  Without
	 * this a power loss could leave previously-written bytes only in
	 * the drive's RAM.  The flush command is its own command + status
	 * cycle, so we wait BSY=0 once more.
	 */
	outb(ch->ch_io_base + ATA_REG_COMMAND,
	    use_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);

	if (ata_wait_ready(ch, 0, &sr) != 0) {
		er = inb(ch->ch_io_base + ATA_REG_ERROR);
		kprintf("ata: %s flush after write failed: %s\n",
		    d->d_devname, ata_decode_err(er));
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

	spin_unlock(&ch->ch_lock);
	return (MACH_MSG_OK);
}

static int
ata_sync(struct ata_drive *d)
{
	struct ata_channel	*ch;
	uint8_t			 sr;
	uint8_t			 er;

	ch = d->d_ch;

	spin_lock(&ch->ch_lock);

	ata_select_drive(d, d->d_lba48 ? ATA_DRV_LBA : 0);
	ata_400ns(ch);

	outb(ch->ch_io_base + ATA_REG_COMMAND,
	    d->d_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);

	if (ata_wait_ready(ch, 0, &sr) != 0) {
		er = inb(ch->ch_io_base + ATA_REG_ERROR);
		kprintf("ata: %s sync failed: %s\n",
		    d->d_devname, ata_decode_err(er));
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

	spin_unlock(&ch->ch_lock);
	return (MACH_MSG_OK);
}

/* ---- helpers ------------------------------------------------------- */

static void
ata_select_drive(struct ata_drive *d, uint8_t extra)
{

	outb(d->d_ch->ch_io_base + ATA_REG_DRIVE,
	    (uint8_t)(ATA_DRV_BASE | (d->d_slave ? ATA_DRV_SLAVE : 0) |
	    (extra & 0x5F)));
}

/*
 * Reading the alt-status register has the same content as the regular
 * status register but does NOT clear pending interrupts.  We hit it
 * four times to burn ~400 ns -- the minimum settling time the ATA
 * spec requires after a drive-select or command write before the
 * status register is allowed to be trusted.
 */
static void
ata_400ns(struct ata_channel *ch)
{

	(void)inb(ch->ch_ctrl_base + ATA_CTL_ALT_STATUS);
	(void)inb(ch->ch_ctrl_base + ATA_CTL_ALT_STATUS);
	(void)inb(ch->ch_ctrl_base + ATA_CTL_ALT_STATUS);
	(void)inb(ch->ch_ctrl_base + ATA_CTL_ALT_STATUS);
}

/*
 * Spin until BSY=0 and the requested status bits are set (or ERR/DF
 * fires).  Returns 0 on success with *sr_out holding the final status,
 * non-zero on error so the caller can read the error register and
 * report.
 *
 * `want` bits we're looking for (typically ATA_SR_DRQ on a transfer
 * step, 0 for "just wait for not-busy" after a flush).
 */
static int
ata_wait_ready(struct ata_channel *ch, uint8_t want, uint8_t *sr_out)
{
	uint8_t	sr;

	/* Burn the 400 ns settle first; first status read is unreliable. */
	ata_400ns(ch);

	for (;;) {
		sr = inb(ch->ch_io_base + ATA_REG_STATUS);
		if (sr & (ATA_SR_ERR | ATA_SR_DF)) {
			*sr_out = sr;
			return (-1);
		}
		if (sr & ATA_SR_BSY)
			continue;
		if ((sr & want) == want) {
			*sr_out = sr;
			return (0);
		}
		/*
		 * BSY=0, DRQ=0, no error: command finished without DRQ.
		 * If the caller wanted DRQ, that's a failure; if they
		 * wanted "just done" (want=0), succeed.
		 */
		if (want == 0) {
			*sr_out = sr;
			return (0);
		}
		/* Spin until DRQ asserts; some drives delay it briefly. */
	}
}

static const char *
ata_decode_err(uint8_t er)
{

	if (er & ATA_ER_UNC)   return ("uncorrectable data");
	if (er & ATA_ER_IDNF)  return ("ID not found");
	if (er & ATA_ER_ABRT)  return ("command aborted");
	if (er & ATA_ER_TK0NF) return ("track 0 not found");
	if (er & ATA_ER_AMNF)  return ("address mark not found");
	if (er & ATA_ER_MC)    return ("media changed");
	if (er & ATA_ER_MCR)   return ("media change request");
	if (er & ATA_ER_BBK)   return ("bad block");
	return ("unknown");
}
