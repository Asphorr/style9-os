/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "gdt.h"

/*
 * The eight 8-byte descriptors that make up the GDT.  Indices 6 and 7
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
static uint64_t		gdt[8] __attribute__((aligned(16))) = {
	0,
	0x00AF9A000000FFFFULL,	/* 0x08 kernel code64           */
	0x00CF92000000FFFFULL,	/* 0x10 kernel data             */
	0x00AFFA000000FFFFULL,	/* 0x18 user code (compat slot) */
	0x00CFF2000000FFFFULL,	/* 0x20 user data               */
	0x00AFFA000000FFFFULL,	/* 0x28 user code64             */
	0,			/* 0x30 TSS low (filled in)     */
	0,			/* 0x38 TSS high                */
};

struct gdt_ptr {
	uint16_t	gp_limit;
	uint64_t	gp_base;
} __attribute__((packed));

static struct gdt_ptr	gdtr;

/*
 * 64-bit TSS.  Only RSP0 is meaningful today (used by the CPU when an
 * interrupt or exception promotes us from ring 3 to ring 0).  The IST
 * fields stay zero; when we want per-IRQ stacks they slot in here.
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

static struct tss	tss __attribute__((aligned(16)));

static void
gdt_set_tss(uint64_t base, uint32_t limit)
{
	uint64_t	lo;
	uint64_t	hi;

	lo  = (uint64_t)limit & 0xFFFFULL;
	lo |= (base & 0xFFFFFFULL) << 16;
	lo |= 0x89ULL << 40;			/* type=9 (avail TSS), P=1 */
	lo |= ((uint64_t)limit & 0xF0000ULL) << 32;	/* limit high  */
	lo |= ((base >> 24) & 0xFFULL) << 56;

	hi  = (base >> 32) & 0xFFFFFFFFULL;

	gdt[6] = lo;
	gdt[7] = hi;
}

void
gdt_init(void)
{

	tss.rsp0 = 0;
	tss.io_map_base = sizeof(struct tss);

	gdt_set_tss((uint64_t)(uintptr_t)&tss, sizeof(struct tss) - 1);

	gdtr.gp_limit = (uint16_t)(sizeof(gdt) - 1);
	gdtr.gp_base  = (uint64_t)(uintptr_t)&gdt;

	/*
	 * lgdt installs the new descriptor table.  We then reload data
	 * segments with the new data selector and reload CS with a far
	 * return (no ljmp in 64-bit GAS for arbitrary immediates -- we
	 * push the selector and a return-target onto the stack, lretq
	 * loads CS and pops the target).  Finally LTR makes the CPU
	 * use our TSS for ring-transition RSP loads.
	 */
	__asm__ __volatile__ (
	    "lgdt %0			\n"
	    "movw $0x10, %%ax		\n"
	    "movw %%ax, %%ds		\n"
	    "movw %%ax, %%es		\n"
	    "movw %%ax, %%fs		\n"
	    "movw %%ax, %%gs		\n"
	    "movw %%ax, %%ss		\n"
	    "pushq $0x08		\n"
	    "leaq 1f(%%rip), %%rax	\n"
	    "pushq %%rax		\n"
	    "lretq			\n"
	    "1:				\n"
	    "movw $0x30, %%ax		\n"
	    "ltr %%ax			\n"
	    :
	    : "m"(gdtr)
	    : "rax", "memory");
}

void
tss_set_rsp0(uint64_t rsp)
{

	tss.rsp0 = rsp;
}
