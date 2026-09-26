/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "acpi.h"
#include "cpu.h"
#include "kprintf.h"
#include "lapic.h"
#include "pmm.h"

/*
 * Where the RSDP may be, 16-byte aligned: the first KiB of the EBDA (its
 * real-mode segment is the word at physical 0x40E), and the BIOS region
 * 0xE0000-0xFFFFF.
 */
#define	ACPI_EBDA_PTR		0x40E
#define	ACPI_BIOS_LOW		0xE0000
#define	ACPI_BIOS_HIGH		0x100000
#define	ACPI_RSDP_ALIGN		16

#define	ACPI_RSDP_SIG		"RSD PTR "
#define	ACPI_RSDP_SIG_LEN	8

/*
 * The RSDP prefix covered by the revision-0 checksum.  Revision 2+ adds a
 * length, the XSDT pointer and an extended checksum; only these 20 bytes
 * are checked, so a bad extended checksum still leaves a usable RSDT.
 */
#define	ACPI_RSDP_V1_LEN	20

struct acpi_rsdp {
	char		rp_signature[8];
	uint8_t		rp_checksum;
	char		rp_oemid[6];
	uint8_t		rp_revision;
	uint32_t	rp_rsdt;
	uint32_t	rp_length;
	uint64_t	rp_xsdt;
	uint8_t		rp_ext_checksum;
	uint8_t		rp_reserved[3];
} __attribute__((packed));

/*
 * Common table header; sh_length covers the whole table.  It is bounded
 * against the identity map before it is used to checksum or walk.
 */
struct acpi_sdt {
	char		sh_signature[4];
	uint32_t	sh_length;
	uint8_t		sh_revision;
	uint8_t		sh_checksum;
	char		sh_oemid[6];
	char		sh_oem_table_id[8];
	uint32_t	sh_oem_revision;
	uint32_t	sh_creator_id;
	uint32_t	sh_creator_revision;
} __attribute__((packed));

struct acpi_madt {
	struct acpi_sdt	ma_hdr;
	uint32_t	ma_lapic_pa;
	uint32_t	ma_flags;
} __attribute__((packed));

#define	ACPI_MADT_PCAT_COMPAT	(1u << 0)

/*
 * MADT entry types.  Any the walk does not handle is skipped by its
 * length, which is validated before it is used to advance.
 */
#define	ACPI_MADT_LAPIC		0
#define	ACPI_MADT_IOAPIC	1
#define	ACPI_MADT_ISO		2
#define	ACPI_MADT_LAPIC_OVR	5
#define	ACPI_MADT_X2APIC	9

struct acpi_madt_entry {
	uint8_t		me_type;
	uint8_t		me_len;
} __attribute__((packed));

struct acpi_madt_lapic {
	struct acpi_madt_entry	ml_hdr;
	uint8_t			ml_acpi_id;
	uint8_t			ml_apic_id;
	uint32_t		ml_flags;
} __attribute__((packed));

struct acpi_madt_ioapic {
	struct acpi_madt_entry	mi_hdr;
	uint8_t			mi_id;
	uint8_t			mi_reserved;
	uint32_t		mi_pa;
	uint32_t		mi_gsi_base;
} __attribute__((packed));

struct acpi_madt_lapic_ovr {
	struct acpi_madt_entry	mo_hdr;
	uint16_t		mo_reserved;
	uint64_t		mo_pa;
} __attribute__((packed));

struct acpi_madt_x2apic {
	struct acpi_madt_entry	mx_hdr;
	uint16_t		mx_reserved;
	uint32_t		mx_apic_id;
	uint32_t		mx_flags;
	uint32_t		mx_acpi_id;
} __attribute__((packed));

/*
 * A processor entry is usable if ENABLED or ONLINE_CAPABLE ("not running,
 * but could be started"); neither bit means an empty socket.
 */
#define	ACPI_LAPIC_ENABLED	(1u << 0)
#define	ACPI_LAPIC_ONLINE_CAP	(1u << 1)

static uint64_t	acpi_lapic_base;	/* (c) as the MADT describes it */
static uint64_t	acpi_ioapic_base;	/* (c)                          */
static uint32_t	acpi_ioapic_gsi;	/* (c)                          */
static bool	acpi_has_8259;		/* (c)                          */

static bool			 acpi_sum_ok(const void *p, size_t len);
static bool			 acpi_in_identity_map(uint64_t pa, size_t len);
static const struct acpi_rsdp	*acpi_find_rsdp(void);
static const struct acpi_rsdp	*acpi_scan_window(uint64_t base, uint64_t end);
static const struct acpi_sdt	*acpi_table_at(uint64_t pa, const char *sig);
static const struct acpi_sdt	*acpi_find_table(const struct acpi_rsdp *rp,
				    const char *sig);
static void			 acpi_madt_walk(const struct acpi_madt *ma);

/*
 * One byte of physical memory, through the identity map.  The address
 * passes through a volatile local: given a literal address, the compiler
 * decides the pointer names no object and warns (-Warray-bounds over
 * `void[0]').
 */
static uint8_t
acpi_peek8(uint64_t pa)
{
	volatile uint64_t	 addr;
	const volatile uint8_t	*p;

	addr = pa;
	p = (const volatile uint8_t *)pmm_kva_from_pa(addr);
	return (*p);
}

/*
 * Read a table entry a byte at a time, little-endian as ACPI always is.
 * The XSDT's 64-bit pointers start at offset 36, so they are only 4-byte
 * aligned, and a plain uint64_t load from them is undefined behaviour.
 */
static uint32_t
acpi_read32(const uint8_t *p)
{
	uint32_t	v;
	unsigned int	i;

	v = 0;
	for (i = 0; i < 4; i++)
		v |= (uint32_t)p[i] << (i * 8);
	return (v);
}

static uint64_t
acpi_read64(const uint8_t *p)
{
	uint64_t	v;
	unsigned int	i;

	v = 0;
	for (i = 0; i < 8; i++)
		v |= (uint64_t)p[i] << (i * 8);
	return (v);
}

/*
 * Bytes must sum to zero mod 256: ACPI's only integrity check, applied to
 * every table.
 */
static bool
acpi_sum_ok(const void *p, size_t len)
{
	const uint8_t	*b;
	uint8_t		 sum;
	size_t		 i;

	b   = (const uint8_t *)p;
	sum = 0;
	for (i = 0; i < len; i++)
		sum = (uint8_t)(sum + b[i]);
	return (sum == 0);
}

/*
 * The tables are read through the boot identity map (PMM_HARD_CAP_BYTES,
 * 1 GiB).  Every physical address taken from a table is checked against it
 * before use; firmware keeps them low in practice, but nothing promises it.
 */
static bool
acpi_in_identity_map(uint64_t pa, size_t len)
{

	if (pa == 0 || len == 0)
		return (false);
	if (pa >= PMM_HARD_CAP_BYTES)
		return (false);
	return (pa + len <= PMM_HARD_CAP_BYTES);
}

static const struct acpi_rsdp *
acpi_scan_window(uint64_t base, uint64_t end)
{
	const struct acpi_rsdp	*rp;
	uint64_t		 pa;
	unsigned int		 i;
	bool			 match;

	for (pa = base; pa + sizeof(*rp) <= end; pa += ACPI_RSDP_ALIGN) {
		if (!acpi_in_identity_map(pa, sizeof(*rp)))
			continue;
		rp = (const struct acpi_rsdp *)pmm_kva_from_pa(pa);

		match = true;
		for (i = 0; i < ACPI_RSDP_SIG_LEN; i++) {
			if (rp->rp_signature[i] != ACPI_RSDP_SIG[i]) {
				match = false;
				break;
			}
		}
		if (!match)
			continue;

		/*
		 * A signature without a good checksum is not a find: "RSD PTR "
		 * also turns up in stale copies and option-ROM strings.
		 */
		if (!acpi_sum_ok(rp, ACPI_RSDP_V1_LEN)) {
			kprintf("acpi: rsdp signature at 0x%llx fails its "
			    "checksum -- ignored\n", (unsigned long long)pa);
			continue;
		}
		return (rp);
	}
	return (NULL);
}

static const struct acpi_rsdp *
acpi_find_rsdp(void)
{
	const struct acpi_rsdp		*rp;
	uint64_t			 ebda;

	/*
	 * EBDA first, then the BIOS region.  The word at 0x40E is a real-mode
	 * segment (0x9FC0 means physical 0x9FC00), read through acpi_peek8.
	 */
	if (acpi_in_identity_map(ACPI_EBDA_PTR, 2)) {
		ebda = (uint64_t)(acpi_peek8(ACPI_EBDA_PTR) |
		    ((uint32_t)acpi_peek8(ACPI_EBDA_PTR + 1) << 8)) << 4;
		if (ebda >= 0x400 && ebda < ACPI_BIOS_LOW) {
			rp = acpi_scan_window(ebda, ebda + 1024);
			if (rp != NULL)
				return (rp);
		}
	}

	return (acpi_scan_window(ACPI_BIOS_LOW, ACPI_BIOS_HIGH));
}

/*
 * Validate one table at a physical address, optionally insisting on a
 * signature.  The header must be mapped before its length is read, and
 * the length sane before the checksum is taken over it.
 */
static const struct acpi_sdt *
acpi_table_at(uint64_t pa, const char *sig)
{
	const struct acpi_sdt	*sdt;
	unsigned int		 i;

	if (!acpi_in_identity_map(pa, sizeof(*sdt)))
		return (NULL);
	sdt = (const struct acpi_sdt *)pmm_kva_from_pa(pa);

	if (sdt->sh_length < sizeof(*sdt))
		return (NULL);
	if (!acpi_in_identity_map(pa, sdt->sh_length))
		return (NULL);
	if (!acpi_sum_ok(sdt, sdt->sh_length))
		return (NULL);

	if (sig != NULL) {
		for (i = 0; i < 4; i++) {
			if (sdt->sh_signature[i] != sig[i])
				return (NULL);
		}
	}
	return (sdt);
}

/*
 * Find a table by signature through the root table.  The XSDT is preferred
 * when offered (the RSDT's 32-bit entries cannot reach tables above 4 GiB);
 * if it does not check out or lacks the table, the RSDT is tried.
 */
static const struct acpi_sdt *
acpi_find_table(const struct acpi_rsdp *rp, const char *sig)
{
	const struct acpi_sdt	*root;
	const struct acpi_sdt	*sdt;
	const uint8_t		*ent;
	size_t			 n;
	size_t			 i;

	if (rp->rp_revision >= 2 && rp->rp_xsdt != 0) {
		root = acpi_table_at(rp->rp_xsdt, "XSDT");
		if (root != NULL) {
			ent = (const uint8_t *)root + sizeof(*root);
			n   = (root->sh_length - sizeof(*root)) / 8;
			for (i = 0; i < n; i++) {
				sdt = acpi_table_at(acpi_read64(ent + i * 8),
				    sig);
				if (sdt != NULL)
					return (sdt);
			}
		}
	}

	root = acpi_table_at(rp->rp_rsdt, "RSDT");
	if (root == NULL)
		return (NULL);
	ent = (const uint8_t *)root + sizeof(*root);
	n   = (root->sh_length - sizeof(*root)) / 4;
	for (i = 0; i < n; i++) {
		sdt = acpi_table_at(acpi_read32(ent + i * 4), sig);
		if (sdt != NULL)
			return (sdt);
	}
	return (NULL);
}

static void
acpi_madt_walk(const struct acpi_madt *ma)
{
	const struct acpi_madt_entry	*me;
	const struct acpi_madt_lapic	*ml;
	const struct acpi_madt_ioapic	*mi;
	const struct acpi_madt_lapic_ovr *mo;
	const struct acpi_madt_x2apic	*mx;
	const uint8_t			*p;
	const uint8_t			*end;
	unsigned int			 x2apic_seen;
	unsigned int			 dropped;
	unsigned int			 unusable;
	int				 id;

	p   = (const uint8_t *)ma + sizeof(*ma);
	end = (const uint8_t *)ma + ma->ma_hdr.sh_length;

	x2apic_seen = 0;
	dropped     = 0;
	unusable    = 0;

	while (p + sizeof(*me) <= end) {
		me = (const struct acpi_madt_entry *)(const void *)p;

		/*
		 * A short or overlong length ends the walk: after it, every
		 * following entry's position is a guess.
		 */
		if (me->me_len < sizeof(*me) || p + me->me_len > end) {
			kprintf("acpi: madt entry at +%u has length %u -- "
			    "stopping the walk here\n",
			    (unsigned int)(p - (const uint8_t *)ma),
			    (unsigned int)me->me_len);
			break;
		}

		switch (me->me_type) {
		case ACPI_MADT_LAPIC:
			if (me->me_len < sizeof(*ml))
				break;
			ml = (const struct acpi_madt_lapic *)(const void *)p;
			if ((ml->ml_flags & (ACPI_LAPIC_ENABLED |
			    ACPI_LAPIC_ONLINE_CAP)) == 0) {
				unusable++;
				break;
			}
			id = cpu_register(ml->ml_apic_id, ml->ml_acpi_id);
			if (id < 0 && cpu_present_count() >= MAXCPU) {
				dropped++;
				break;
			}
			kprintf("acpi:   lapic id %u (acpi id %u)%s%s\n",
			    (unsigned int)ml->ml_apic_id,
			    (unsigned int)ml->ml_acpi_id,
			    (ml->ml_flags & ACPI_LAPIC_ENABLED) != 0 ?
			    "" : " online-capable",
			    id < 0 ? " -- this processor" : "");
			break;

		case ACPI_MADT_X2APIC:
			if (me->me_len < sizeof(*mx))
				break;
			mx = (const struct acpi_madt_x2apic *)(const void *)p;
			if ((mx->mx_flags & (ACPI_LAPIC_ENABLED |
			    ACPI_LAPIC_ONLINE_CAP)) == 0)
				break;
			/*
			 * Counted and reported, not registered: the driver
			 * speaks xAPIC MMIO, whose ICR cannot address an id
			 * above 254.
			 */
			x2apic_seen++;
			break;

		case ACPI_MADT_IOAPIC:
			if (me->me_len < sizeof(*mi))
				break;
			mi = (const struct acpi_madt_ioapic *)(const void *)p;
			if (acpi_ioapic_base == 0) {
				acpi_ioapic_base = mi->mi_pa;
				acpi_ioapic_gsi  = mi->mi_gsi_base;
			}
			kprintf("acpi:   io apic id %u at 0x%llx, gsi base "
			    "%u\n", (unsigned int)mi->mi_id,
			    (unsigned long long)mi->mi_pa,
			    (unsigned int)mi->mi_gsi_base);
			break;

		case ACPI_MADT_LAPIC_OVR:
			if (me->me_len < sizeof(*mo))
				break;
			mo = (const struct acpi_madt_lapic_ovr *)
			    (const void *)p;
			/* Overrides the header's 32-bit address. */
			acpi_lapic_base = mo->mo_pa;
			kprintf("acpi:   lapic address overridden to "
			    "0x%llx\n", (unsigned long long)mo->mo_pa);
			break;

		default:
			break;
		}

		p += me->me_len;
	}

	if (x2apic_seen != 0)
		kprintf("acpi: %u processor(s) described only as x2APIC -- "
		    "this driver speaks xAPIC, so they stay down\n",
		    x2apic_seen);
	if (unusable != 0)
		kprintf("acpi: %u processor entr(ies) neither enabled nor "
		    "online-capable -- empty sockets\n", unusable);
	if (dropped != 0)
		kprintf("acpi: %u processor(s) beyond MAXCPU=%u DROPPED -- "
		    "raise MAXCPU to use them\n", dropped,
		    (unsigned int)MAXCPU);
}

bool
acpi_madt_probe(void)
{
	const struct acpi_rsdp	*rp;
	const struct acpi_sdt	*sdt;
	const struct acpi_madt	*ma;
	uint64_t		 hw;

	rp = acpi_find_rsdp();
	if (rp == NULL) {
		kprintf("acpi: no rsdp in the ebda or the bios region -- "
		    "one processor is all this kernel can know about\n");
		return (false);
	}

	kprintf("acpi: rsdp at %p, revision %u, oem '%c%c%c%c%c%c'\n",
	    (const void *)rp, (unsigned int)rp->rp_revision,
	    rp->rp_oemid[0], rp->rp_oemid[1], rp->rp_oemid[2],
	    rp->rp_oemid[3], rp->rp_oemid[4], rp->rp_oemid[5]);

	sdt = acpi_find_table(rp, "APIC");
	if (sdt == NULL) {
		kprintf("acpi: no MADT among the tables -- staying on one "
		    "processor\n");
		return (false);
	}

	ma = (const struct acpi_madt *)(const void *)sdt;
	if (sdt->sh_length < sizeof(*ma)) {
		kprintf("acpi: MADT is %u bytes, shorter than its own "
		    "header\n", (unsigned int)sdt->sh_length);
		return (false);
	}

	acpi_lapic_base = ma->ma_lapic_pa;
	acpi_has_8259   = (ma->ma_flags & ACPI_MADT_PCAT_COMPAT) != 0;

	kprintf("acpi: madt at %p, %u bytes, lapic regs 0x%llx, "
	    "8259 %s\n", (const void *)ma, (unsigned int)sdt->sh_length,
	    (unsigned long long)acpi_lapic_base,
	    acpi_has_8259 ? "present" : "absent");

	acpi_madt_walk(ma);

	/*
	 * Cross-check the firmware's APIC address against this CPU's
	 * IA32_APIC_BASE MSR.  They may differ only if the APIC was
	 * relocated, which nothing here does.
	 */
	hw = lapic_base_pa();
	if (hw != 0 && acpi_lapic_base != 0 && hw != acpi_lapic_base)
		kprintf("acpi: *** the MADT says the local APIC is at 0x%llx "
		    "and the MSR says 0x%llx ***\n",
		    (unsigned long long)acpi_lapic_base,
		    (unsigned long long)hw);

	kprintf("acpi: %u processor(s) present, %u running\n",
	    cpu_present_count(), cpu_online_count());

	return (true);
}

uint64_t
acpi_lapic_pa(void)
{

	return (acpi_lapic_base);
}

uint64_t
acpi_ioapic_pa(void)
{

	return (acpi_ioapic_base);
}

uint32_t
acpi_ioapic_gsi_base(void)
{

	return (acpi_ioapic_gsi);
}

bool
acpi_pcat_compat(void)
{

	return (acpi_has_8259);
}
