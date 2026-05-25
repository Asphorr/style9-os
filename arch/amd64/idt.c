/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "gdt.h"
#include "idt.h"

struct idt_entry {
	uint16_t	ie_offset_lo;
	uint16_t	ie_selector;
	uint8_t		ie_ist;			/* IST index (3 bits) + zero */
	uint8_t		ie_attr;
	uint16_t	ie_offset_mid;
	uint32_t	ie_offset_hi;
	uint32_t	ie_reserved;
} __attribute__((packed));

struct idt_ptr {
	uint16_t	ip_limit;
	uint64_t	ip_base;
} __attribute__((packed));

static struct idt_entry	idt[IDT_NENTRIES] __attribute__((aligned(16)));
static struct idt_ptr	idtr;

/* Generated in isr.S; one entry per CPU exception + IRQ stub (0..47). */
extern uintptr_t	isr_table[];

#define	IDT_NSTUBS	48

void
idt_set_gate(unsigned int vec, uintptr_t handler, uint16_t selector,
    uint8_t ist, uint8_t attr)
{

	if (vec >= IDT_NENTRIES)
		return;

	idt[vec].ie_offset_lo  = (uint16_t)(handler & 0xFFFF);
	idt[vec].ie_selector   = selector;
	idt[vec].ie_ist        = (uint8_t)(ist & 0x7);
	idt[vec].ie_attr       = attr;
	idt[vec].ie_offset_mid = (uint16_t)((handler >> 16) & 0xFFFF);
	idt[vec].ie_offset_hi  = (uint32_t)((handler >> 32) & 0xFFFFFFFFU);
	idt[vec].ie_reserved   = 0;
}

void
idt_init(void)
{
	unsigned int	i;
	uint8_t		attr;

	attr = IDT_ATTR(1, 0, IDT_TYPE_INTR_GATE);

	for (i = 0; i < IDT_NSTUBS; i++)
		idt_set_gate(i, isr_table[i], GDT_KCODE, 0, attr);

	idtr.ip_limit = (uint16_t)(sizeof(idt) - 1);
	idtr.ip_base  = (uint64_t)(uintptr_t)&idt;

	__asm__ __volatile__ ("lidt %0" : : "m"(idtr));
}
