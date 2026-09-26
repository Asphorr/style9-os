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
#include "clock.h"
#include "dev_proto.h"
#include "dev_subsystem.h"
#include "intr.h"
#include "io.h"
#include "kprintf.h"
#include "panic.h"
#include "pic.h"
#include "port.h"
#include "sched.h"
#include "spinlock.h"
#include "task.h"
#include "thread.h"
#include "tsc.h"

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
 * The control register (alt-status / device-control) is a separate range:
 * 0x3F6 primary, 0x376 secondary.  Reading alt-status does not clear a
 * pending IRQ, so it is the one to look at without acknowledging anything.
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
 * One park waits ATA_INTR_SLICE_MS for the drive's interrupt before looking
 * at the drive itself; a command is declared dead after ATA_INTR_LIMIT_MS,
 * which also bounds the polled waits.  The waiter holds fs_lock, so a wait
 * with no end would stall everything that touches the volume.
 */
#define	ATA_INTR_SLICE_MS	100
#define	ATA_INTR_LIMIT_MS	5000

/*
 * Up to two channels, each with master + slave.  Probed at init; only
 * drives that respond to IDENTIFY get an entry in `drives`.
 */
#define	ATA_NDRIVES_MAX		4

/* Lock key:
 *	(c) const after probe
 *	(l) protected by ch_lock of the owning channel
 */
/*
 * Interrupt-driven completion.  The IRQ fields are plain atomics, not under
 * ch_lock, which a transfer holds across its polled waits; the handler
 * takes no lock and touches nothing else.
 *
 * An interrupt that lands after the waiter checked ch_irq_seen but before
 * it is BLOCKED is not lost: thread_wake on a thread not yet blocked leaves
 * th_wake_pending, and the park declines to sleep.  A lost interrupt posts
 * no wake at all; the deadline in ata_wait_intr answers that.
 */
struct ata_channel {
	struct spinlock	 ch_lock;
	uint16_t	 ch_io_base;	/* (c) e.g. 0x1F0           */
	uint16_t	 ch_ctrl_base;	/* (c) e.g. 0x3F6           */
	unsigned	 ch_irq;	/* (c) 14 or 15             */
	bool		 ch_irq_ok;	/* (c) INTRQ proven at probe */
	struct thread	*volatile ch_waiter;	/* atomic */
	volatile uint32_t	  ch_irq_seen;	/* atomic */
	volatile uint8_t	  ch_irq_status;/* status latched by the ISR */
	volatile uint32_t	  ch_n_irq;	/* diagnostics */
	volatile uint32_t	  ch_n_block;	/* diagnostics */
	/*
	 * Who has a command in flight, and how often another thread started
	 * one anyway.  ch_lock does not exclude that: ata_wait_intr releases
	 * it to sleep, so the channel is unowned for most of a command.
	 */
	struct thread	*volatile ch_cmd_owner;		/* (l) */
	volatile uint32_t	  ch_n_overlap;		/* (l) */
	volatile uint32_t	  ch_n_lost;	/* waits the deadline ended */
	volatile uint32_t	  ch_n_drained;	/* sectors left in the port */
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
	{ SPINLOCK_INIT("ata-ch0"), 0x1F0, 0x3F6, 14, false, NULL, 0, 0, 0, 0,
	    NULL, 0, 0, 0 },
	{ SPINLOCK_INIT("ata-ch1"), 0x170, 0x376, 15, false, NULL, 0, 0, 0, 0,
	    NULL, 0, 0, 0 },
};

static struct ata_drive		drives[ATA_NDRIVES_MAX];

static int	ata_dispatch_for_drive(struct ata_drive *d,
		    const struct mach_msg_header *req,
		    struct port_space *from);

/*
 * Per-drive dispatcher trampolines, each bound to one drive index.
 * Non-static so PORT_SPECIAL_SERVICE can hold one in p_special_arg.
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
static int	ata_wait_intr(struct ata_channel *ch, uint8_t want,
		    uint8_t *sr_out);
static void	ata_irq14(struct trapframe *tf);
static void	ata_irq15(struct trapframe *tf);
static void	ata_400ns(struct ata_channel *ch);
static int	ata_wait_idle(struct ata_channel *ch);
static void	ata_cmd_claim(struct ata_channel *ch);
static void	ata_cmd_release(struct ata_channel *ch);
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
		 * Software reset: pulse SRST, with nIEN set so a half-reset
		 * channel raises nothing.  Puts master and slave into a known
		 * state before IDENTIFY.
		 */
		outb(channels[ci].ch_ctrl_base + ATA_CTL_DEV_CONTROL,
		    ATA_DCR_SRST | ATA_DCR_NIEN);
		ata_400ns(&channels[ci]);

		/*
		 * Release with nIEN clear, so the channel asserts INTRQ from
		 * here on.  IDENTIFY still polls: whether the interrupt
		 * arrives is what this probe finds out.
		 */
		irq_install(channels[ci].ch_irq,
		    ci == 0 ? ata_irq14 : ata_irq15);
		pic_unmask(channels[ci].ch_irq);
		channels[ci].ch_n_irq = 0;
		outb(channels[ci].ch_ctrl_base + ATA_CTL_DEV_CONTROL, 0);
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

			/* Control port, via this drive index's trampoline. */
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

		/*
		 * Did IDENTIFY's completion arrive as an interrupt?  Then
		 * transfers on this channel sleep.  If not (wiring, an
		 * unrouted PCI IRQ, a VM that does not raise the legacy
		 * line), they poll: waiting for an interrupt nobody sends
		 * would hang the first read.
		 */
		channels[ci].ch_irq_ok = (channels[ci].ch_n_irq != 0);
		kprintf("ata: channel %u IRQ%u -- %s (%u seen at probe)\n",
		    (unsigned)ci, channels[ci].ch_irq,
		    channels[ci].ch_irq_ok ? "interrupt-driven" :
		    "silent, staying polled",
		    (unsigned)channels[ci].ch_n_irq);
	}

	if (ndrives == 0)
		kprintf("ata: no drives detected\n");
}

/*
 * p_special_arg holds only the dispatcher function, with no room for the
 * drive, so four trampolines bind one drive index each; init picks by
 * enumeration order.  The drive index cannot ride in the pointer's low
 * bits: the special-port intercept calls the pointer directly.
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

		/*
		 * This write bypasses the block cache, which cannot tell which
		 * pages it made stale; have it forget the whole drive.
		 */
		if (rv == MACH_MSG_OK)
			bio_invalidate_drive((unsigned)(d - drives));

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
	 * Poll for BSY=0, then check LBAMID/LBAHI: non-zero means ATAPI (or
	 * SATA in legacy mode), which this IDENTIFY does not describe, so
	 * the slot is reported empty.
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
	 * The model is 40 ASCII chars, two per word with the first in the
	 * high byte ("QE" is word 0x5145).  Unpack into d_model, then trim
	 * trailing spaces.
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
 * One PIO transfer (ata_write likewise, ending in FLUSH CACHE), run under
 * ch_lock so a sibling drive on the channel cannot rewrite the task-file
 * registers mid-command -- except while a read parks in ata_wait_intr with
 * the lock released (see ch_cmd_owner).  Returns MACH_MSG_OK or MACH_E_*.
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
	ata_cmd_claim(ch);

	if (ata_wait_idle(ch) != 0) {
		ata_cmd_release(ch);
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

	if (use_lba48) {
		ata_select_drive(d, ATA_DRV_LBA);
		ata_400ns(ch);

		/*
		 * LBA48: the high byte of each field first, then the low;
		 * each write shifts the previous value into the HOB register.
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

	/*
	 * Arm the flag before issuing: the drive can raise INTRQ as soon as
	 * it accepts the command, and clearing afterwards would lose it.
	 */
	__atomic_store_n(&ch->ch_irq_seen, 0, __ATOMIC_RELAXED);
	outb(ch->ch_io_base + ATA_REG_COMMAND, cmd);

	for (s = 0; s < count; s++) {
		if (ata_wait_intr(ch, ATA_SR_DRQ, &sr) != 0) {
			er = inb(ch->ch_io_base + ATA_REG_ERROR);
			kprintf("ata: %s read sector %llu failed: %s "
			    "(sr=0x%02x er=0x%02x)\n",
			    d->d_devname, (unsigned long long)(lba + s),
			    ata_decode_err(er),
			    (unsigned)sr, (unsigned)er);
			ata_cmd_release(ch);
			spin_unlock(&ch->ch_lock);
			return (MACH_E_INVAL);
		}

		/*
		 * Re-arm before draining: the next sector's INTRQ comes as
		 * soon as this one's last word leaves the port, and a flag
		 * cleared after the copy would lose it.
		 */
		__atomic_store_n(&ch->ch_irq_seen, 0, __ATOMIC_RELAXED);
		insw(ch->ch_io_base + ATA_REG_DATA,
		    p + s * ATA_SECTOR_BYTES, ATA_SECTOR_BYTES / 2);
	}

	ata_cmd_release(ch);
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
	ata_cmd_claim(ch);

	if (ata_wait_idle(ch) != 0) {
		ata_cmd_release(ch);
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

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
			ata_cmd_release(ch);
			spin_unlock(&ch->ch_lock);
			return (MACH_E_INVAL);
		}

		outsw(ch->ch_io_base + ATA_REG_DATA,
		    p + s * ATA_SECTOR_BYTES, ATA_SECTOR_BYTES / 2);
	}

	/*
	 * Flush the drive's write cache, or a power cut can take bytes it
	 * holds only in RAM.  Wait for idle first: the drive is still
	 * committing the last sector, and a command written while BSY is set
	 * is dropped -- a flush that reports success and flushed nothing.
	 */
	if (ata_wait_idle(ch) != 0) {
		ata_cmd_release(ch);
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}
	outb(ch->ch_io_base + ATA_REG_COMMAND,
	    use_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);

	if (ata_wait_ready(ch, 0, &sr) != 0) {
		er = inb(ch->ch_io_base + ATA_REG_ERROR);
		kprintf("ata: %s flush after write failed: %s\n",
		    d->d_devname, ata_decode_err(er));
		ata_cmd_release(ch);
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

	ata_cmd_release(ch);
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
	ata_cmd_claim(ch);

	if (ata_wait_idle(ch) != 0) {
		ata_cmd_release(ch);
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

	ata_select_drive(d, d->d_lba48 ? ATA_DRV_LBA : 0);
	ata_400ns(ch);

	outb(ch->ch_io_base + ATA_REG_COMMAND,
	    d->d_lba48 ? ATA_CMD_FLUSH_CACHE_EXT : ATA_CMD_FLUSH_CACHE);

	if (ata_wait_ready(ch, 0, &sr) != 0) {
		er = inb(ch->ch_io_base + ATA_REG_ERROR);
		kprintf("ata: %s sync failed: %s\n",
		    d->d_devname, ata_decode_err(er));
		ata_cmd_release(ch);
		spin_unlock(&ch->ch_lock);
		return (MACH_E_INVAL);
	}

	ata_cmd_release(ch);
	spin_unlock(&ch->ch_lock);
	return (MACH_MSG_OK);
}

/*
 * Kernel-facing block I/O: the in-kernel filesystems reach the PIO path
 * here rather than by messaging their own disk service.  Checks the drive
 * index; ata_read/ata_write check the LBA range.
 */
int
ata_kread(unsigned drive_idx, uint64_t lba, uint32_t count, void *buf)
{

	if (drive_idx >= ATA_NDRIVES_MAX || !drives[drive_idx].d_present)
		return (MACH_E_DEAD);
	return (ata_read(&drives[drive_idx], lba, count, buf));
}

int
ata_kwrite(unsigned drive_idx, uint64_t lba, uint32_t count, const void *buf)
{

	if (drive_idx >= ATA_NDRIVES_MAX || !drives[drive_idx].d_present)
		return (MACH_E_DEAD);
	return (ata_write(&drives[drive_idx], lba, count, buf));
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
 * The ~400 ns the spec requires after a drive select or command write
 * before status can be trusted: four alt-status reads, which do not clear
 * a pending interrupt.
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
 * Wait for BSY=0 and DRQ=0 before touching the command-block registers, as
 * the spec requires.  A drive still finishing the previous command either
 * discards the writes (an emulator does, and the command is never issued)
 * or latches part of them into the running one; WRITE SECTORS followed by
 * FLUSH CACHE is the nearest case.  A stuck DRQ is drained, since only that
 * clears it; ch_n_drained counts commands abandoned with data in the port.
 */
static uint16_t	ata_sink[ATA_SECTOR_BYTES / 2];	/* thrown away by definition */

static int
ata_wait_idle(struct ata_channel *ch)
{
	uint64_t	give_up;
	uint64_t	hz;
	uint8_t		sr;

	/*
	 * Timed by the TSC, not the clock: this spins under ch_lock with
	 * interrupts off, and clock_uptime_ms advances from the PIT
	 * interrupt, which on the boot CPU this loop would itself hold off.
	 */
	hz = tsc_hz();
	if (hz == 0)				/* before calibration */
		hz = 1000000000ull;
	give_up = tsc_read() + hz * (ATA_INTR_LIMIT_MS / 1000);
	for (;;) {
		sr = inb(ch->ch_ctrl_base + ATA_CTL_ALT_STATUS);
		if ((sr & (ATA_SR_BSY | ATA_SR_DRQ)) == 0)
			return (0);
		if ((sr & ATA_SR_BSY) == 0 && (sr & ATA_SR_DRQ) != 0) {
			ch->ch_n_drained++;
			insw(ch->ch_io_base + ATA_REG_DATA, ata_sink,
			    ATA_SECTOR_BYTES / 2);
			continue;
		}
		if (tsc_read() >= give_up) {
			kprintf("ata: channel at 0x%x will not go idle -- "
			    "status 0x%02x\n", (unsigned)ch->ch_io_base,
			    (unsigned)sr);
			return (-1);
		}
	}
}

/*
 * The two ends of a command, called with ch_lock held.  Claiming excludes
 * nobody: it records who is mid-command and counts (ch_n_overlap) the times
 * another thread began one anyway, which ch_lock cannot prevent because
 * ata_wait_intr releases it to sleep.
 */
static void
ata_cmd_claim(struct ata_channel *ch)
{

	if (ch->ch_cmd_owner != NULL && ch->ch_cmd_owner != current_thread)
		ch->ch_n_overlap++;
	ch->ch_cmd_owner = current_thread;
}

static void
ata_cmd_release(struct ata_channel *ch)
{

	ch->ch_cmd_owner = NULL;
}

/*
 * Channel interrupt.  Reading the regular status register is what clears
 * INTRQ (alt-status does not), so the read is the acknowledgement; it is
 * latched for the waiter.
 */
static void
ata_irq(struct ata_channel *ch)
{
	struct thread	*w;

	ch->ch_irq_status = inb(ch->ch_io_base + ATA_REG_STATUS);
	ch->ch_n_irq++;
	__atomic_store_n(&ch->ch_irq_seen, 1, __ATOMIC_RELEASE);

	/*
	 * Empty the waiter slot before waking, so a second interrupt cannot
	 * post the same thread twice.  sched_post_irq_wake is the only
	 * scheduler call legal here: it queues the wake for a safe point.
	 */
	w = __atomic_exchange_n(&ch->ch_waiter, NULL, __ATOMIC_ACQ_REL);
	if (w != NULL)
		sched_post_irq_wake(w);
}

static void
ata_irq14(struct trapframe *tf)
{

	(void)tf;
	ata_irq(&channels[0]);
}

static void
ata_irq15(struct trapframe *tf)
{

	(void)tf;
	ata_irq(&channels[1]);
}

/*
 * Wait for the channel's interrupt, sleeping rather than spinning.  Called
 * and returns with ch_lock held.  A channel whose INTRQ did not show up at
 * probe is polled instead.
 *
 * A proven channel can still drop one: IRQ14/15 are edge-triggered at the
 * 8259, and a drive that deasserts and reasserts INTRQ while the previous
 * interrupt is in service presents an edge nobody sees.  So each park has
 * a deadline, after which the drive itself is asked.
 */
static int
ata_wait_intr(struct ata_channel *ch, uint8_t want, uint8_t *sr_out)
{
	struct thread	*self;
	uint64_t	 give_up;
	uint8_t		 sr;

	if (!ch->ch_irq_ok)
		return (ata_wait_ready(ch, want, sr_out));

	self    = current_thread;
	give_up = clock_uptime_ms() + ATA_INTR_LIMIT_MS;

	/*
	 * ch_waiter names this thread while it parks, and the ISR wakes that
	 * name without knowing whether the thread still exists.  It stays
	 * valid because a kernel_task thread cannot be killed and any other
	 * gets here holding a mutex (fs_lock), so the kill checks will not
	 * retire it mid-park (th_mutex_depth, kern/thread.h).  A killable
	 * thread holding nothing is refused.
	 */
	KASSERT(self->th_task == kernel_task || self->th_mutex_depth > 0,
	    "ata_wait_intr: a killable thread is waiting with nothing held");

	/*
	 * Noted as well: should a killable thread ever park here, its exit
	 * clears the slot (th_wait_slot, kern/thread.h) rather than leave its
	 * name for the command's interrupt to wake.
	 */
	thread_slot_note(self, &ch->ch_waiter);

	for (;;) {
		if (__atomic_load_n(&ch->ch_irq_seen, __ATOMIC_ACQUIRE) != 0)
			break;

		/*
		 * Install ourselves, then re-check: the interrupt may have
		 * landed in the gap, and then the slot is emptied again so an
		 * interrupt for the next sector finds no stale waiter.
		 *
		 * An exchange, not a store: a store still in this CPU's store
		 * buffer when the re-check reads ch_irq_seen lets an interrupt
		 * on another CPU find the slot empty and wake nobody, costing
		 * a whole ATA_INTR_SLICE_MS.  See kbd_getc_block.
		 */
		(void)__atomic_exchange_n(&ch->ch_waiter, self,
		    __ATOMIC_ACQ_REL);
		if (__atomic_load_n(&ch->ch_irq_seen, __ATOMIC_ACQUIRE) != 0) {
			__atomic_store_n(&ch->ch_waiter, NULL,
			    __ATOMIC_RELAXED);
			break;
		}

		ch->ch_n_block++;
		self->th_wake_deadline_ms = clock_uptime_ms() +
		    ATA_INTR_SLICE_MS;
		sched_add_timed_waiter(self);
		thread_block_release(THREAD_BLOCK_SLEEP, ch, &ch->ch_lock);
		sched_remove_timed_waiter(self);
		spin_lock(&ch->ch_lock);

		if (__atomic_load_n(&ch->ch_irq_seen, __ATOMIC_ACQUIRE) != 0)
			continue;

		/*
		 * The deadline woke us, so ask the drive.  Alt-status carries
		 * the same bits and reading it acknowledges nothing.  If the
		 * drive is at the boundary we wanted, its interrupt was lost,
		 * and the regular-status read that follows acknowledges it.
		 */
		sr = inb(ch->ch_ctrl_base + ATA_CTL_ALT_STATUS);
		if ((sr & ATA_SR_BSY) == 0 &&
		    ((sr & (ATA_SR_ERR | ATA_SR_DF)) != 0 || want == 0 ||
		    (sr & want) == want)) {
			__atomic_store_n(&ch->ch_waiter, NULL,
			    __ATOMIC_RELAXED);
			thread_slot_forget(self);
			ch->ch_n_lost++;
			sr = inb(ch->ch_io_base + ATA_REG_STATUS);
			*sr_out = sr;
			return ((sr & (ATA_SR_ERR | ATA_SR_DF)) != 0 ? -1 : 0);
		}

		/*
		 * Busy, or not yet at the boundary: a slow drive, not a lost
		 * interrupt.  It may be slow until ATA_INTR_LIMIT_MS; then the
		 * command is declared dead.
		 */
		if (clock_uptime_ms() >= give_up) {
			__atomic_store_n(&ch->ch_waiter, NULL,
			    __ATOMIC_RELAXED);
			ch->ch_n_lost++;
			kprintf("ata: channel at 0x%x gave up after %u ms -- "
			    "status 0x%02x, waiting for 0x%02x\n",
			    (unsigned)ch->ch_io_base,
			    (unsigned)ATA_INTR_LIMIT_MS, (unsigned)sr,
			    (unsigned)want);
			thread_slot_forget(self);
			*sr_out = sr;
			return (-1);
		}
	}
	__atomic_store_n(&ch->ch_waiter, NULL, __ATOMIC_RELAXED);
	thread_slot_forget(self);

	sr = ch->ch_irq_status;
	if ((sr & (ATA_SR_ERR | ATA_SR_DF)) != 0) {
		*sr_out = sr;
		return (-1);
	}
	/*
	 * The interrupt says the command reached a boundary, not necessarily
	 * the one asked about, and the latch may not even be this command's.
	 * So when bits are wanted the live register decides, always.  A
	 * latched DRQ from a late interrupt, or from the sector just
	 * finished, would drain the port while the drive is still fetching:
	 * the right block with the wrong body.  Five port reads per 256-word
	 * transfer cost nothing.
	 */
	if (want != 0)
		return (ata_wait_ready(ch, want, sr_out));
	*sr_out = sr;
	return (0);
}

/*
 * Spin until BSY=0 and all `want' bits are set (ATA_SR_DRQ for a transfer
 * step, 0 for "not busy" after a flush), or ERR/DF.  Returns 0 with the
 * final status in *sr_out, or -1 so the caller can read the error register.
 * Bounded on the TSC for ata_wait_idle's reason: it spins under ch_lock
 * with interrupts off, on every transfer step.
 */
static int
ata_wait_ready(struct ata_channel *ch, uint8_t want, uint8_t *sr_out)
{
	uint64_t	give_up;
	uint64_t	hz;
	uint8_t		sr;

	hz = tsc_hz();
	if (hz == 0)
		hz = 1000000000ull;
	give_up = tsc_read() + hz * (ATA_INTR_LIMIT_MS / 1000);

	/* Burn the 400 ns settle first; first status read is unreliable. */
	ata_400ns(ch);

	for (;;) {
		sr = inb(ch->ch_io_base + ATA_REG_STATUS);
		if (sr & (ATA_SR_ERR | ATA_SR_DF)) {
			*sr_out = sr;
			return (-1);
		}
		if ((sr & ATA_SR_BSY) == 0) {
			if ((sr & want) == want) {
				*sr_out = sr;
				return (0);
			}
			/*
			 * Not busy, no error, wanted bits missing: done if
			 * only not-busy was wanted, else keep looking (some
			 * drives raise DRQ a little late).
			 */
			if (want == 0) {
				*sr_out = sr;
				return (0);
			}
		}
		if (tsc_read() >= give_up) {
			kprintf("ata: channel at 0x%x stopped answering -- "
			    "status 0x%02x, waiting for 0x%02x\n",
			    (unsigned)ch->ch_io_base, (unsigned)sr,
			    (unsigned)want);
			*sr_out = sr;
			return (-1);
		}
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

/* Diagnostic counters, summed or printed per channel (dev/ata_drv.h). */
uint32_t
ata_overlaps(void)
{

	return (channels[0].ch_n_overlap + channels[1].ch_n_overlap);
}

uint32_t
ata_lost_intrs(void)
{

	return (channels[0].ch_n_lost + channels[1].ch_n_lost);
}

void
ata_irq_stats(void)
{
	size_t	ci;

	for (ci = 0; ci < 2; ci++)
		kprintf("ata: channel %u -- %u interrupts, %u blocking waits, "
		    "%u begun on a busy channel, %u interrupt(s) never came, "
		    "%u sector(s) drained from a port left full\n",
		    (unsigned)ci, (unsigned)channels[ci].ch_n_irq,
		    (unsigned)channels[ci].ch_n_block,
		    (unsigned)channels[ci].ch_n_overlap,
		    (unsigned)channels[ci].ch_n_lost,
		    (unsigned)channels[ci].ch_n_drained);
}
