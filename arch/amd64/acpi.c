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
 * Where the Root System Description Pointer is allowed to be.  Two windows,
 * and both must be looked in: the first kilobyte of the Extended BIOS Data
 * Area, whose segment address is a 16-bit word the BIOS leaves at physical
 * 0x40E, and the BIOS read-only region at the top of the first megabyte.  The
 * structure is 16-byte aligned in both.
 */
#define	ACPI_EBDA_PTR		0x40E
#define	ACPI_BIOS_LOW		0xE0000
#define	ACPI_BIOS_HIGH		0x100000
#define	ACPI_RSDP_ALIGN		16

#define	ACPI_RSDP_SIG		"RSD PTR "
#define	ACPI_RSDP_SIG_LEN	8

/*
 * The checksummed prefix of the RSDP.  Revision 0 checksums 20 bytes and stops
 * there; revision 2 and up append a length and a 64-bit XSDT pointer and
 * checksum the whole thing again.  The first 20 bytes are checked either way,
 * so a revision-2 table with a broken extended checksum still yields a usable
 * RSDT rather than nothing.
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
 * Every ACPI table starts with this, and the length in it is the length of the
 * whole table including the header.  The walk trusts that length for exactly
 * one thing -- how much to checksum -- and re-derives everything else, because
 * a length is the one field a corrupt table uses to make a reader run off the
 * end.
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
 * MADT entry types.  Only the four that describe processors and interrupt
 * plumbing are named; the rest are skipped by length, which is why the length
 * is validated before it is used to advance.
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
 * A processor entry is usable if it is ENABLED, or if it is ONLINE_CAPABLE,
 * which is ACPI's way of saying "not running but could be started".  Neither
 * bit set means the firmware is describing a socket with nothing in it.
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
 * Sum of bytes must be zero mod 256.  This is the only integrity check ACPI
 * offers, and it is worth applying to every table rather than to the first
 * one: the tables are built by firmware that also has bugs, and a table that
 * fails here is one this kernel would otherwise believe.
 */
/*
 * Read a table entry a byte at a time.
 *
 * Not fussiness: the XSDT's array of 64-bit pointers begins at offset 36, so
 * every entry in it is four-byte aligned and none of them is eight -- a
 * uint64_t load through a plain pointer would be reading an object at an
 * address that cannot hold one.  x86 tolerates that in hardware and the
 * language does not, which is the combination that produces code working until
 * the compiler picks a different instruction.  Spelling out the byte order also
 * says what ACPI's is: little-endian, always, whatever the machine's.
 */
/*
 * One byte of physical memory, through the identity map.
 *
 * The address goes through a volatile local before it becomes a pointer, and
 * that indirection is the entire content of this function.  Handed a LITERAL
 * address, the compiler concludes the pointer refers to no declared object and
 * warns that every subscript of it is out of the bounds of nothing
 * (-Warray-bounds over `void[0]').  It is not wrong: C has no way to say "this
 * integer is an address the firmware chose".  Forcing the value through memory
 * it must actually load is how one says it anyway.
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
 * The tables are read through the boot identity map, which reaches one
 * gigabyte.  Every physical address taken out of a table is checked against
 * that before it is dereferenced -- firmware puts these tables in low memory
 * on every machine anyone has met, but "on every machine anyone has met" is
 * not a bound, and the fault would be a page fault in the middle of a probe.
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
		 * Signature without checksum is not a find.  The eight bytes
		 * "RSD PTR " appear in the middle of other things -- a BIOS
		 * that copied a table, a string in an option ROM -- and
		 * following one of those leads to a table walk over nonsense.
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
	 * EBDA first, because that is where it is on a machine that has one,
	 * and the BIOS region below is where it is on a machine that does not.
	 * The word at 0x40E is a real-mode SEGMENT, so it is shifted, not
	 * used: a value of 0x9FC0 means physical 0x9FC00.
	 *
	 * Read a byte at a time through acpi_peek8 -- see there for why a
	 * literal address needs help getting past the compiler.
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
 * Validate one table at a physical address and, optionally, insist on a
 * signature.  Two-step on purpose: the header has to be readable before its
 * length can be trusted, and the length has to be sane before the checksum
 * can be taken over it.
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
 * Walk the root table's array of pointers looking for one signature.  The XSDT
 * is preferred where the RSDP offers it, because on a machine whose tables sit
 * above four gigabytes the RSDT physically cannot describe them -- its entries
 * are 32 bits wide.  If the XSDT does not check out, the RSDT is still tried:
 * two roots describing the same tables is ACPI's own redundancy and there is no
 * reason to refuse the working one.
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
		 * A zero or short length would make this loop stand still or
		 * step backwards, so it ends the walk rather than being
		 * skipped: after a length nobody can trust, the position of
		 * every following entry is a guess.
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
			 * Counted and named, not registered.  An x2APIC-only
			 * processor is one this kernel cannot address: the
			 * driver speaks the MMIO interface, and ids above 254
			 * do not fit in the register that sends a startup
			 * message.  Saying so is better than a table that
			 * quietly describes more CPUs than ever start.
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
			/*
			 * The 64-bit address wins over the 32-bit one in the
			 * table header, which is the entire point of the
			 * entry existing.
			 */
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
	 * THE CROSS-CHECK, and the reason this probe is more than a printout:
	 * the address above came from a table the firmware wrote, and the one
	 * below came from a register on this processor.  Two independent
	 * sources agreeing is evidence; one source is a claim.  They can
	 * legitimately differ only if this CPU's APIC has been relocated,
	 * which nothing here does.
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
