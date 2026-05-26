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
 * Two flavours of operation:
 *
 *	- pmap_kenter / pmap_kremove / pmap_kextract install or query
 *	  mappings in the kernel's PML4 (the tree boot.S installed).  They
 *	  exist for kernel-VA work (MMIO, future high-half DMAP) and are
 *	  thin wrappers around the per-pmap APIs below with kernel_pmap.
 *
 *	- pmap_create / pmap_destroy / pmap_enter / pmap_remove /
 *	  pmap_extract / pmap_activate operate on a struct pmap *, the
 *	  per-task page-table tree.  A fresh pmap shares the kernel's
 *	  upper-PML4 entries (so the boot identity map and any future
 *	  kernel-VA mapping is visible from every task) and owns its own
 *	  PDPT under PML4[0] -- that's where user-VA installs land.
 *
 * The boot identity map is 2 MiB huge pages covering 0 .. 1 GiB.  Any
 * caller within that range should use pmm_kva_from_pa() instead of
 * pmap_kenter -- the mapping is already there from boot.S.
 */

#define	VM_PROT_READ		0x01	/* implicit on present mappings */
#define	VM_PROT_WRITE		0x02
#define	VM_PROT_EXEC		0x04
#define	VM_PROT_USER		0x08	/* ring 3 may read the page     */

#define	PMAP_NOCACHE		0x100	/* PCD/PWT for MMIO regions     */
#define	PMAP_GLOBAL		0x200	/* set the G bit                */

struct pmap;

extern struct pmap	*kernel_pmap;

void		pmap_bootstrap(void);

/* Per-pmap operations. */
struct pmap	*pmap_create(void);
void		 pmap_destroy(struct pmap *);
bool		 pmap_enter(struct pmap *, uint64_t va, uint64_t pa,
		    uint32_t flags);
bool		 pmap_remove(struct pmap *, uint64_t va);
uint64_t	 pmap_extract(struct pmap *, uint64_t va);

/*
 * Install `pm` as the active address space (mov %rax, %cr3).  Caller
 * is responsible for being on a stack reachable in both the old and
 * new tree -- in practice this means a kernel kstack that lives in the
 * shared boot identity map (PDPT[0] of PML4[0]).  Idempotent if the
 * requested CR3 already matches the current CR3.
 */
void		 pmap_activate(struct pmap *);

/*
 * Convenience wrappers for callers that care only about the kernel
 * pmap.  Equivalent to pmap_enter(kernel_pmap, ...) etc.
 */
bool		pmap_kenter(uint64_t va, uint64_t pa, uint32_t flags);
bool		pmap_kremove(uint64_t va);
uint64_t	pmap_kextract(uint64_t va);

void		pmap_invlpg(uint64_t va);

void		pmap_stats(void);

#endif /* !_MACHINE_PMAP_H_ */
