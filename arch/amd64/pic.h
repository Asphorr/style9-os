/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_PIC_H_
#define	_MACHINE_PIC_H_

#include <stdint.h>

/*
 * Intel 8259A programmable interrupt controller, master + slave pair.
 *
 * BIOS leaves master IRQs mapped to vectors 0x08-0x0F which collide with
 * the CPU's reserved exception vectors (#DF, #TS, #NP, etc.).  pic_init
 * remaps master to 0x20 (32) and slave to 0x28 (40), then masks every
 * line; callers re-enable individual lines via pic_unmask.
 *
 * EOI must be issued at the end of every IRQ handler.  For IRQ 8-15 the
 * slave is acknowledged first, then the master (cascade path).
 */

#define	PIC_VEC_BASE	32

void	pic_init(void);
void	pic_mask(unsigned int irq);
void	pic_unmask(unsigned int irq);
void	pic_eoi(unsigned int irq);

#endif /* !_MACHINE_PIC_H_ */
