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
 * x86_64 Interrupt Descriptor Table.
 *
 * 256 16-byte gates.  We populate gates 0-47 from the stub addresses
 * exported by isr.S; the rest stay zeroed (P=0) so an unexpected vector
 * raises #NP which we route through the panic path.
 */

#define	IDT_NENTRIES		256

#define	IDT_TYPE_INTR_GATE	0xE	/* IF cleared on entry  */
#define	IDT_TYPE_TRAP_GATE	0xF	/* IF unchanged         */

#define	IDT_ATTR(present, dpl, type)					\
	(uint8_t)(((present) << 7) | (((dpl) & 3) << 5) | ((type) & 0xF))

void	idt_init(void);
void	idt_set_gate(unsigned int vec, uintptr_t handler,
	    uint16_t selector, uint8_t ist, uint8_t attr);

#endif /* !_MACHINE_IDT_H_ */
