/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_IDT_H_
#define	_MACHINE_IDT_H_

#include <stdint.h>

/*
 * x86_64 Interrupt Descriptor Table: 256 16-byte gates, every one an
 * interrupt gate on the matching isr.S stub.
 */

#define	IDT_NENTRIES		256

#define	IDT_TYPE_INTR_GATE	0xE	/* IF cleared on entry  */
#define	IDT_TYPE_TRAP_GATE	0xF	/* IF unchanged         */

#define	IDT_ATTR(present, dpl, type)					\
	(uint8_t)(((present) << 7) | (((dpl) & 3) << 5) | ((type) & 0xF))

void	idt_init(void);

/*
 * Point this CPU's IDTR at the shared table idt_init built.  A CPU that
 * skips it triple-faults on its first exception.
 */
void	idt_load(void);
void	idt_set_gate(unsigned int vec, uintptr_t handler,
	    uint16_t selector, uint8_t ist, uint8_t attr);

#endif /* !_MACHINE_IDT_H_ */
