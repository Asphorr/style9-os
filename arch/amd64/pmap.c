/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "spinlock.h"

/*
 * x86_64 page-table entry bits.
 *	PTE_P	present
 *	PTE_RW	writable
 *	PTE_US	user accessible (kernel mappings clear this)
 *	PTE_PWT	page write-through
 *	PTE_PCD	page cache disable
 *	PTE_A	accessed (set by CPU)
 *	PTE_D	dirty (set by CPU)
 *	PTE_PS	page-size: 1 == 1 GiB / 2 MiB leaf, 0 == link to next level
 *	PTE_G	global (TLB survives CR3 reload if CR4.PGE set)
 *	PTE_NX	bit 63, no-execute (requires EFER.NXE)
 *
 * The 40-bit PFN sits in bits 12..51 on current CPUs.  We mask with
 * PTE_PA_MASK any time we want the physical address out of an entry.
 */
#define	PTE_P			((uint64_t)1 << 0)
#define	PTE_RW			((uint64_t)1 << 1)
#define	PTE_US			((uint64_t)1 << 2)
#define	PTE_PWT			((uint64_t)1 << 3)
#define	PTE_PCD			((uint64_t)1 << 4)
#define	PTE_A			((uint64_t)1 << 5)
#define	PTE_D			((uint64_t)1 << 6)
#define	PTE_PS			((uint64_t)1 << 7)
#define	PTE_G			((uint64_t)1 << 8)
#define	PTE_NX			((uint64_t)1 << 63)

#define	PTE_PA_MASK		((uint64_t)0x000FFFFFFFFFF000)

/*
 * Lock-key:
 *	(c) const after pmap_bootstrap
 *	(p) protected by pmap_lock
 */
static uint64_t		*pmap_kpml4;		/* (c) PML4 VA            */
static uint64_t		 pmap_kpml4_pa;		/* (c) PML4 PA, == CR3    */
static uint64_t		 pmap_intermediates;	/* (p) intermediate tables allocated */
static uint64_t		 pmap_leafs;		/* (p) live 4 KiB mappings */

static struct spinlock	pmap_lock = SPINLOCK_INIT("pmap");

static uint64_t	*table_va(uint64_t pte);
static uint64_t	*ensure_table(uint64_t *parent, size_t idx, bool user);
static uint64_t	 leaf_flags(uint32_t prot);
static bool	 pmap_kenter_locked(uint64_t va, uint64_t pa, uint32_t flags);

static inline size_t
pml4_idx(uint64_t va)
{

	return ((va >> 39) & 0x1FF);
}

static inline size_t
pdpt_idx(uint64_t va)
{

	return ((va >> 30) & 0x1FF);
}

static inline size_t
pd_idx(uint64_t va)
{

	return ((va >> 21) & 0x1FF);
}

static inline size_t
pt_idx(uint64_t va)
{

	return ((va >> 12) & 0x1FF);
}

void
pmap_bootstrap(void)
{
	uint64_t	cr3;

	__asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3));
	pmap_kpml4_pa = cr3 & PTE_PA_MASK;
	pmap_kpml4    = (uint64_t *)pmm_kva_from_pa(pmap_kpml4_pa);

	pmap_intermediates = 0;
	pmap_leafs         = 0;

	kprintf("pmap: kernel CR3 = 0x%llx (PML4 at %p)\n",
	    (unsigned long long)pmap_kpml4_pa, (void *)pmap_kpml4);
}

bool
pmap_kenter(uint64_t va, uint64_t pa, uint32_t flags)
{
	bool	ok;

	spin_lock(&pmap_lock);
	ok = pmap_kenter_locked(va, pa, flags);
	spin_unlock(&pmap_lock);

	return (ok);
}

bool
pmap_kremove(uint64_t va)
{
	uint64_t	*pdpt, *pd, *pt;
	uint64_t	 e;

	spin_lock(&pmap_lock);

	e = pmap_kpml4[pml4_idx(va)];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0) {
		spin_unlock(&pmap_lock);
		return (false);
	}
	pdpt = table_va(e);

	e = pdpt[pdpt_idx(va)];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0) {
		spin_unlock(&pmap_lock);
		return (false);
	}
	pd = table_va(e);

	e = pd[pd_idx(va)];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0) {
		spin_unlock(&pmap_lock);
		return (false);
	}
	pt = table_va(e);

	if ((pt[pt_idx(va)] & PTE_P) == 0) {
		spin_unlock(&pmap_lock);
		return (false);
	}

	pt[pt_idx(va)] = 0;
	pmap_invlpg(va);
	pmap_leafs--;

	spin_unlock(&pmap_lock);
	return (true);
}

uint64_t
pmap_kextract(uint64_t va)
{
	uint64_t	*pdpt, *pd, *pt;
	uint64_t	 e, pa;

	spin_lock(&pmap_lock);

	e = pmap_kpml4[pml4_idx(va)];
	if ((e & PTE_P) == 0) {
		spin_unlock(&pmap_lock);
		return (PA_INVALID);
	}
	pdpt = table_va(e);

	e = pdpt[pdpt_idx(va)];
	if ((e & PTE_P) == 0) {
		spin_unlock(&pmap_lock);
		return (PA_INVALID);
	}
	if (e & PTE_PS) {				/* 1 GiB huge page */
		pa = (e & PTE_PA_MASK) | (va & 0x3FFFFFFFULL);
		spin_unlock(&pmap_lock);
		return (pa);
	}
	pd = table_va(e);

	e = pd[pd_idx(va)];
	if ((e & PTE_P) == 0) {
		spin_unlock(&pmap_lock);
		return (PA_INVALID);
	}
	if (e & PTE_PS) {				/* 2 MiB huge page */
		pa = (e & PTE_PA_MASK) | (va & 0x1FFFFFULL);
		spin_unlock(&pmap_lock);
		return (pa);
	}
	pt = table_va(e);

	e = pt[pt_idx(va)];
	if ((e & PTE_P) == 0) {
		spin_unlock(&pmap_lock);
		return (PA_INVALID);
	}
	pa = (e & PTE_PA_MASK) | (va & PAGE_MASK);

	spin_unlock(&pmap_lock);
	return (pa);
}

void
pmap_invlpg(uint64_t va)
{

	__asm__ __volatile__ ("invlpg (%0)" :: "r"((uintptr_t)va) : "memory");
}

void
pmap_stats(void)
{
	uint64_t	leafs, inters;

	spin_lock(&pmap_lock);
	leafs  = pmap_leafs;
	inters = pmap_intermediates;
	spin_unlock(&pmap_lock);

	kprintf("pmap: %llu leaf mappings, %llu intermediate tables, "
	    "kernel CR3 = 0x%llx\n",
	    (unsigned long long)leafs,
	    (unsigned long long)inters,
	    (unsigned long long)pmap_kpml4_pa);
}

/* ---- internals ---------------------------------------------------------- */

static uint64_t *
table_va(uint64_t pte)
{

	return ((uint64_t *)pmm_kva_from_pa(pte & PTE_PA_MASK));
}

/*
 * Return the next-level table referenced by parent[idx], allocating
 * it from pmm and zero-clearing if it doesn't yet exist.  Refuses to
 * descend into a huge-page entry -- a caller asking for a finer
 * mapping over a hugepage is a bug and would corrupt the boot map.
 */
static uint64_t *
ensure_table(uint64_t *parent, size_t idx, bool user)
{
	uint64_t	 e, pa;
	uint64_t	*tbl;

	e = parent[idx];

	if (e & PTE_P) {
		if (e & PTE_PS)
			return (NULL);
		/*
		 * If a user leaf is going under an intermediate created
		 * for kernel-only mappings, promote the US bit.  The
		 * page-walk requires US along every level; an existing
		 * US=0 intermediate would gate a user leaf below.  US=1
		 * is harmless for kernel-only leaves -- the leaf US bit
		 * is what gates ring-3 access at the page granularity.
		 */
		if (user && (e & PTE_US) == 0) {
			parent[idx] |= PTE_US;
			pmap_invlpg((uint64_t)(uintptr_t)parent);
		}
		return (table_va(e));
	}

	pa = pmm_alloc_page();
	if (pa == PA_INVALID)
		return (NULL);

	tbl = (uint64_t *)pmm_kva_from_pa(pa);
	for (size_t i = 0; i < 512; i++)
		tbl[i] = 0;

	/*
	 * Intermediate entries are P + RW; the actual permission gate
	 * lives at the leaf.  Letting RW propagate down means a writable
	 * leaf is honoured; clearing RW here would shadow the leaf and
	 * make every page read-only.  US is set on demand from the
	 * caller's intent so ring-3 walks land on user leaves.
	 */
	parent[idx] = pa | PTE_P | PTE_RW | (user ? PTE_US : 0);
	pmap_intermediates++;
	return (tbl);
}

static uint64_t
leaf_flags(uint32_t prot)
{
	uint64_t	f;

	f = PTE_P;
	if (prot & VM_PROT_WRITE)
		f |= PTE_RW;
	if (!(prot & VM_PROT_EXEC))
		f |= PTE_NX;
	if (prot & VM_PROT_USER)
		f |= PTE_US;
	if (prot & PMAP_NOCACHE)
		f |= PTE_PCD | PTE_PWT;
	if (prot & PMAP_GLOBAL)
		f |= PTE_G;

	return (f);
}

static bool
pmap_kenter_locked(uint64_t va, uint64_t pa, uint32_t prot)
{
	uint64_t	*pdpt, *pd, *pt;
	uint64_t	 old;
	bool		 user;

	KASSERT((va & PAGE_MASK) == 0, "pmap_kenter: unaligned VA");
	KASSERT((pa & PAGE_MASK) == 0, "pmap_kenter: unaligned PA");

	user = (prot & VM_PROT_USER) != 0;

	pdpt = ensure_table(pmap_kpml4, pml4_idx(va), user);
	if (pdpt == NULL)
		return (false);

	pd = ensure_table(pdpt, pdpt_idx(va), user);
	if (pd == NULL)
		return (false);

	pt = ensure_table(pd, pd_idx(va), user);
	if (pt == NULL)
		return (false);

	old = pt[pt_idx(va)];
	pt[pt_idx(va)] = (pa & PTE_PA_MASK) | leaf_flags(prot);

	if (old & PTE_P)
		pmap_invlpg(va);
	else
		pmap_leafs++;

	return (true);
}
