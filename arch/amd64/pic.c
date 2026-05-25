/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdint.h>

#include "io.h"
#include "pic.h"

#define	PIC1_CMD	0x20
#define	PIC1_DATA	0x21
#define	PIC2_CMD	0xA0
#define	PIC2_DATA	0xA1

#define	ICW1_INIT	0x10
#define	ICW1_ICW4	0x01
#define	ICW4_8086	0x01

#define	PIC_EOI		0x20

#define	PIC1_VEC_OFF	(PIC_VEC_BASE)		/* IRQ  0- 7 -> 32-39 */
#define	PIC2_VEC_OFF	(PIC_VEC_BASE + 8)	/* IRQ  8-15 -> 40-47 */

void
pic_init(void)
{

	/* ICW1: cascade mode, ICW4 to follow. */
	outb(PIC1_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();
	outb(PIC2_CMD, ICW1_INIT | ICW1_ICW4);
	io_wait();

	/* ICW2: vector offsets. */
	outb(PIC1_DATA, PIC1_VEC_OFF);
	io_wait();
	outb(PIC2_DATA, PIC2_VEC_OFF);
	io_wait();

	/* ICW3: cascade wiring -- master has slave on IRQ2; slave is id 2. */
	outb(PIC1_DATA, 0x04);
	io_wait();
	outb(PIC2_DATA, 0x02);
	io_wait();

	/* ICW4: 8086 mode (vs. legacy 8080). */
	outb(PIC1_DATA, ICW4_8086);
	io_wait();
	outb(PIC2_DATA, ICW4_8086);
	io_wait();

	/* Mask every line; callers enable individually. */
	outb(PIC1_DATA, 0xFF);
	outb(PIC2_DATA, 0xFF);
}

void
pic_mask(unsigned int irq)
{
	uint16_t	port;
	uint8_t		mask;

	if (irq >= 16)
		return;

	port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	mask = inb(port);
	mask |= (uint8_t)(1u << (irq & 7));
	outb(port, mask);
}

void
pic_unmask(unsigned int irq)
{
	uint16_t	port;
	uint8_t		mask;

	if (irq >= 16)
		return;

	port = (irq < 8) ? PIC1_DATA : PIC2_DATA;
	mask = inb(port);
	mask &= (uint8_t)~(1u << (irq & 7));
	outb(port, mask);
}

void
pic_eoi(unsigned int irq)
{

	if (irq >= 16)
		return;

	if (irq >= 8)
		outb(PIC2_CMD, PIC_EOI);
	outb(PIC1_CMD, PIC_EOI);
}
