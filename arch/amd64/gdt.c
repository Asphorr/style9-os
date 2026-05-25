/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "gdt.h"

/*
 * Five 8-byte descriptors.  Long-mode code segments set the L bit (bit
 * 53); data segments are kept compatible with both 32- and 64-bit by
 * leaving D/B and G set.
 *
 *	0x00AF9A000000FFFF	kernel code64
 *	0x00CF92000000FFFF	kernel data
 *	0x00AFFA000000FFFF	user code64
 *	0x00CFF2000000FFFF	user data
 */
static uint64_t		gdt[5] __attribute__((aligned(16))) = {
	0,
	0x00AF9A000000FFFFULL,
	0x00CF92000000FFFFULL,
	0x00AFFA000000FFFFULL,
	0x00CFF2000000FFFFULL,
};

struct gdt_ptr {
	uint16_t	gp_limit;
	uint64_t	gp_base;
} __attribute__((packed));

static struct gdt_ptr	gdtr;

void
gdt_init(void)
{

	gdtr.gp_limit = (uint16_t)(sizeof(gdt) - 1);
	gdtr.gp_base  = (uint64_t)(uintptr_t)&gdt;

	/*
	 * lgdt installs the new descriptor table.  We then reload data
	 * segments with the new data selector and reload CS with a far
	 * return (no ljmp in 64-bit GAS for arbitrary immediates -- we
	 * push the selector and a return-target onto the stack, lretq
	 * loads CS and pops the target).
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
	    :
	    : "m"(gdtr)
	    : "rax", "memory");
}
