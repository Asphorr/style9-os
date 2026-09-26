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
 *	  per-task page-table tree.  It shares the kernel's PML4 entries
 *	  1..511 and owns a copy of the kernel's PDPT under PML4[0],
 *	  where user VA lives.
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
 * Load `pm' into CR3, unless it is already there.  The caller's stack must
 * be mapped in both trees -- in practice a kstack in the boot identity map
 * (PDPT[0] of PML4[0]).
 */
void		 pmap_activate(struct pmap *);

/*
 * Convenience wrappers for callers that care only about the kernel
 * pmap.  Equivalent to pmap_enter(kernel_pmap, ...) etc.
 */
bool		pmap_kenter(uint64_t va, uint64_t pa, uint32_t flags);
bool		pmap_kremove(uint64_t va);
uint64_t	pmap_kextract(uint64_t va);

/*
 * Drop one page's translation here and on every other online CPU, and
 * return only once all have: a caller that removes a mapping and then frees
 * the page may assume it unreachable.  One load while only one CPU is
 * online.  See pmap.c for how the request reaches a CPU that is spinning
 * with interrupts off.
 */
void		pmap_invlpg(uint64_t va);

/*
 * Carry out an outstanding invalidation on this CPU, if any.  Called from
 * the IPI and from every loop that spins with interrupts off; the latter is
 * what makes the shootdown deadlock-free.  Interrupts must be off.
 */
void		pmap_tlb_poll(void);

/* Claim the vector.  Before any processor that could be asked exists. */
void		pmap_tlb_init(void);

/*
 * Send a thousand invalidations and check that every CPU answered each.
 * Only the far CPU can write an acknowledgement, so this tests the whole
 * path: APIC, IDT, handler, barriers.
 */
void		pmap_tlb_selftest(void);

/*
 * Physical address of the kernel's PML4 (the CR3 value), which mp.c hands
 * to a starting AP: it needs a page table before it can run at any kernel
 * address.
 */
uint64_t	pmap_kernel_root_pa(void);

void		pmap_stats(void);

#endif /* !_MACHINE_PMAP_H_ */
