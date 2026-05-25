/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_PMAP_H_
#define	_MACHINE_PMAP_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * Machine-dependent kernel VM layer.
 *
 * Only kernel-VA mappings exist today; user pmaps follow once we have
 * processes.  The functions below operate on the live CR3 root: in
 * effect they extend the page-table tree that boot.S installed, by
 * allocating fresh PDPT / PD / PT pages from pmm as needed.
 *
 * The boot identity map is 2 MiB huge pages covering 0 .. 1 GiB.  We
 * refuse to map a 4 KiB sub-page over a present huge-page entry; the
 * pmap layer is for addresses outside the boot range (in practice,
 * higher-half VAs).  Within the boot range you already have a valid
 * VA == PA mapping; just use pmm_kva_from_pa().
 */

#define	VM_PROT_READ		0x01	/* implicit on present mappings */
#define	VM_PROT_WRITE		0x02
#define	VM_PROT_EXEC		0x04

#define	PMAP_NOCACHE		0x100	/* PCD/PWT for MMIO regions     */
#define	PMAP_GLOBAL		0x200	/* set the G bit                */

void		pmap_bootstrap(void);
bool		pmap_kenter(uint64_t va, uint64_t pa, uint32_t flags);
bool		pmap_kremove(uint64_t va);
uint64_t	pmap_kextract(uint64_t va);

void		pmap_invlpg(uint64_t va);

void		pmap_stats(void);

#endif /* !_MACHINE_PMAP_H_ */
