/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "cpu.h"
#include "gdt.h"

/*
 * The eight 8-byte descriptors that make up a GDT.  Indices 6 and 7
 * together hold the 16-byte TSS descriptor (filled in at runtime once
 * the TSS base is known).
 *
 *	0x00AF9A000000FFFF	kernel code64
 *	0x00CF92000000FFFF	kernel data
 *	0x00AFFA000000FFFF	user code64 (placeholder at 0x18 too;
 *				SYSRET-compat would target it but we
 *				only ever SYSRETQ)
 *	0x00CFF2000000FFFF	user data
 */
#define	GDT_ENTRIES		8

#define	GDT_DESC_KCODE		0x00AF9A000000FFFFULL
#define	GDT_DESC_KDATA		0x00CF92000000FFFFULL
#define	GDT_DESC_UCODE		0x00AFFA000000FFFFULL
#define	GDT_DESC_UDATA		0x00CFF2000000FFFFULL

/*
 * One table, one pointer and one TSS per CPU (see gdt.h), indexed by the
 * dense CPU id, so tss_set_rsp0 cannot address another CPU's TSS.
 */
static uint64_t		gdt[MAXCPU][GDT_ENTRIES] __attribute__((aligned(16)));

struct gdt_ptr {
	uint16_t	gp_limit;
	uint64_t	gp_base;
} __attribute__((packed));

static struct gdt_ptr	gdtr[MAXCPU];

/*
 * 64-bit TSS, wire format imposed by Intel SDM Vol 3.  Only RSP0 is used
 * (loaded on an interrupt or exception from ring 3); IST stays zero.
 */
struct tss {
	uint32_t	reserved0;
	uint64_t	rsp0;
	uint64_t	rsp1;
	uint64_t	rsp2;
	uint64_t	reserved1;
	uint64_t	ist[7];
	uint64_t	reserved2;
	uint16_t	reserved3;
	uint16_t	io_map_base;
} __attribute__((packed));

_Static_assert(sizeof(struct tss) == 104, "tss must be 104 bytes");

static struct tss	tss[MAXCPU] __attribute__((aligned(16)));

/*
 * Fill the flat descriptors.  Written out, not copied from a template: the
 * kernel has no memcpy, and -O2 may turn a copy loop into a call to one.
 */
static void
gdt_fill_flat(uint64_t *g)
{

	g[0] = 0;
	g[GDT_KCODE / 8]   = GDT_DESC_KCODE;
	g[GDT_KDATA / 8]   = GDT_DESC_KDATA;
	g[GDT_UCODE32 / 8] = GDT_DESC_UCODE;
	g[GDT_UDATA / 8]   = GDT_DESC_UDATA;
	g[GDT_UCODE / 8]   = GDT_DESC_UCODE;
}

static void
gdt_set_tss(uint64_t *g, uint64_t base, uint32_t limit)
{
	uint64_t	lo;
	uint64_t	hi;

	lo  = (uint64_t)limit & 0xFFFFULL;
	lo |= (base & 0xFFFFFFULL) << 16;
	lo |= 0x89ULL << 40;			/* type=9 (avail TSS), P=1 */
	lo |= ((uint64_t)limit & 0xF0000ULL) << 32;	/* limit high  */
	lo |= ((base >> 24) & 0xFFULL) << 56;

	hi  = (base >> 32) & 0xFFFFFFFFULL;

	g[GDT_TSS / 8]       = lo;
	g[GDT_TSS / 8 + 1]   = hi;
}

void
gdt_init_cpu(void)
{
	unsigned int	me;

	me = cpu_id();

	tss[me].rsp0 = 0;
	tss[me].io_map_base = sizeof(struct tss);

	gdt_fill_flat(gdt[me]);
	gdt_set_tss(gdt[me], (uint64_t)(uintptr_t)&tss[me],
	    sizeof(struct tss) - 1);

	gdtr[me].gp_limit = (uint16_t)(sizeof(gdt[me]) - 1);
	gdtr[me].gp_base  = (uint64_t)(uintptr_t)&gdt[me];

	/*
	 * lgdt, reload the data segments, reload CS with a far return
	 * (push selector and target, lretq -- 64-bit GAS has no ljmp with
	 * an arbitrary immediate), then ltr so ring transitions take RSP0
	 * from our TSS.
	 *
	 * %gs and %fs are deliberately not reloaded.  In long mode their
	 * bases live in MSRs, and writing the selector loads the
	 * descriptor's base instead -- zero here -- which would point this
	 * CPU's per-CPU block at physical zero.  The GS base is set by
	 * cpu_bsp_init or ap_entry.
	 */
	__asm__ __volatile__ (
	    "lgdt %0			\n"
	    "movw $0x10, %%ax		\n"
	    "movw %%ax, %%ds		\n"
	    "movw %%ax, %%es		\n"
	    "movw %%ax, %%ss		\n"
	    "pushq $0x08		\n"
	    "leaq 1f(%%rip), %%rax	\n"
	    "pushq %%rax		\n"
	    "lretq			\n"
	    "1:				\n"
	    "movw $0x30, %%ax		\n"
	    "ltr %%ax			\n"
	    :
	    : "m" (gdtr[me])
	    : "rax", "memory");
}

void
tss_set_rsp0(uint64_t rsp)
{

	tss[cpu_id()].rsp0 = rsp;
}
