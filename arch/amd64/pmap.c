/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kmem.h"
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
 * Per-pmap state.  Lock key:
 *	(c) const after pmap_create / pmap_bootstrap
 *	(p) protected by pm_lock
 *
 * The kernel pmap is the singleton initialised by pmap_bootstrap from
 * the live CR3; user pmaps come from pmap_create and share kernel_pmap's
 * upper-PML4 entries (so the boot identity map and any future kernel-VA
 * mapping is visible from every task).
 */
struct pmap {
	struct spinlock	 pm_lock;
	uint64_t	*pm_pml4;		/* (c) PML4 VA       */
	uint64_t	 pm_pml4_pa;		/* (c) PML4 PA == CR3 */
	uint64_t	 pm_leafs;		/* (p) live 4 KiB leaves     */
	uint64_t	 pm_intermediates;	/* (p) intermediate tables   */
	bool		 pm_is_kernel;		/* (c) skip teardown        */
};

static struct pmap	 kernel_pmap_store = {
	.pm_lock      = SPINLOCK_INIT("kpmap"),
	.pm_is_kernel = true,
};

struct pmap		*kernel_pmap = &kernel_pmap_store;

static uint64_t	*table_va(uint64_t pte);
static uint64_t	*ensure_table(struct pmap *pm, uint64_t *parent, size_t idx,
		    bool user);
static uint64_t	 leaf_flags(uint32_t prot);
static bool	 pmap_enter_locked(struct pmap *pm, uint64_t va, uint64_t pa,
		    uint32_t flags);
static bool	 pmap_remove_locked(struct pmap *pm, uint64_t va);
static uint64_t	 pmap_extract_locked(struct pmap *pm, uint64_t va);

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
	kernel_pmap->pm_pml4_pa = cr3 & PTE_PA_MASK;
	kernel_pmap->pm_pml4    =
	    (uint64_t *)pmm_kva_from_pa(kernel_pmap->pm_pml4_pa);
	kernel_pmap->pm_intermediates = 0;
	kernel_pmap->pm_leafs         = 0;

	kprintf("pmap: kernel CR3 = 0x%llx (PML4 at %p)\n",
	    (unsigned long long)kernel_pmap->pm_pml4_pa,
	    (void *)kernel_pmap->pm_pml4);
}

/*
 * pmap_create: build a fresh per-task PML4.
 *
 * Memory layout we end up with:
 *	new_pml4[i]      for i in 1..511		= kernel_pmap->pm_pml4[i]
 *	new_pml4[0]      = pa(new_pdpt0) | P|RW|US
 *	new_pdpt0[0]     = kernel_pdpt0[0] (boot identity 0..1 GiB)
 *	new_pdpt0[i]     for i in 1..511		= 0  (filled lazily)
 *
 * Sharing entries 1..511 at the PML4 level means any kernel mapping
 * placed under those slots after this pmap is created automatically
 * shows up here too -- the next-level tables are the same pages.  PDPT
 * 0 is forked because that PDPT is where the per-task user-VA pages
 * live (USER_CODE_VA = 0x40000000 sits in PDPT slot 1 of PML4[0]); we
 * still want the boot identity range to remain reachable for kernel
 * code running with this pmap loaded, hence the PDPT[0]-only copy.
 */
struct pmap *
pmap_create(void)
{
	struct pmap	*pm;
	uint64_t	 pml4_pa;
	uint64_t	 pdpt_pa;
	uint64_t	*new_pml4;
	uint64_t	*new_pdpt0;
	uint64_t	*kern_pdpt0;
	uint64_t	 e;
	size_t		 i;

	pm = (struct pmap *)kmalloc(sizeof(*pm));
	if (pm == NULL)
		return (NULL);

	pml4_pa = pmm_alloc_page();
	if (pml4_pa == PA_INVALID) {
		kfree(pm);
		return (NULL);
	}
	pdpt_pa = pmm_alloc_page();
	if (pdpt_pa == PA_INVALID) {
		pmm_free_page(pml4_pa);
		kfree(pm);
		return (NULL);
	}

	new_pml4  = (uint64_t *)pmm_kva_from_pa(pml4_pa);
	new_pdpt0 = (uint64_t *)pmm_kva_from_pa(pdpt_pa);
	for (i = 0; i < 512; i++) {
		new_pml4[i]  = 0;
		new_pdpt0[i] = 0;
	}

	/*
	 * Snapshot kernel PML4 first, then weld the fresh PDPT into slot 0.
	 * Copy entries 1..511 verbatim -- they point at next-level pages we
	 * deliberately share so kernel-side mappings stay coherent across
	 * tasks.  Entry 0 we override; the original kernel PDPT-0 contents
	 * are folded into our new PDPT-0 below.
	 */
	for (i = 1; i < 512; i++)
		new_pml4[i] = kernel_pmap->pm_pml4[i];

	e = kernel_pmap->pm_pml4[0];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0) {
		pmm_free_page(pdpt_pa);
		pmm_free_page(pml4_pa);
		kfree(pm);
		return (NULL);
	}

	kern_pdpt0 = (uint64_t *)pmm_kva_from_pa(e & PTE_PA_MASK);
	/*
	 * Copy the WHOLE kernel PDPT-0, not just slot 0.  In today's tree
	 * only slot 0 (the boot_pd huge-page chain) is populated, but a
	 * future caller adding e.g. a high-MMIO mapping under PML4[0] would
	 * land in another slot; this guards against that drift.  User-VA
	 * installs on this pmap take the same slots in our private PDPT
	 * and overwrite whatever kernel had there -- safe because the
	 * kernel never installs user-VA mappings under PML4[0] outside the
	 * boot identity range itself.
	 */
	for (i = 0; i < 512; i++)
		new_pdpt0[i] = kern_pdpt0[i];

	/*
	 * PML4 entry for our PDPT-0.  US=1 so a future user leaf below
	 * passes the ring-3 walk; the leaf itself decides accessibility.
	 */
	new_pml4[0] = pdpt_pa | PTE_P | PTE_RW | PTE_US;

	spin_init(&pm->pm_lock, "pmap");
	pm->pm_pml4          = new_pml4;
	pm->pm_pml4_pa       = pml4_pa;
	pm->pm_leafs         = 0;
	pm->pm_intermediates = 1;	/* the PDPT we just allocated */
	pm->pm_is_kernel     = false;
	return (pm);
}

/*
 * Tear down a per-task pmap.  Walks the PRIVATE PDPT under PML4[0],
 * frees the PD/PT pages reachable from PDPT[1..511] (PDPT[0] is the
 * shared boot_pd chain -- leave alone), then frees the PDPT and PML4
 * pages themselves.  Refusing to destroy kernel_pmap is a hard panic;
 * we never want to be one stray pointer away from unmapping the world.
 */
void
pmap_destroy(struct pmap *pm)
{
	uint64_t	*pdpt;
	uint64_t	*pd;
	uint64_t	 pml4_e, pdpt_e, pd_e;
	uint64_t	 pdpt_pa;
	size_t		 i, j;

	if (pm == NULL)
		return;
	if (pm->pm_is_kernel)
		panic("pmap_destroy: attempt to destroy kernel_pmap");

	pml4_e = pm->pm_pml4[0];
	if ((pml4_e & PTE_P) != 0 && (pml4_e & PTE_PS) == 0) {
		pdpt_pa = pml4_e & PTE_PA_MASK;
		pdpt    = (uint64_t *)pmm_kva_from_pa(pdpt_pa);

		/*
		 * Skip slot 0 -- that's a copy of kern_pdpt0[0], which points
		 * at boot_pd (boot identity huge pages).  Freeing that would
		 * unmap the world from under every other task and the kernel.
		 */
		for (i = 1; i < 512; i++) {
			pdpt_e = pdpt[i];
			if ((pdpt_e & PTE_P) == 0 || (pdpt_e & PTE_PS) != 0)
				continue;
			pd = (uint64_t *)pmm_kva_from_pa(pdpt_e & PTE_PA_MASK);
			for (j = 0; j < 512; j++) {
				pd_e = pd[j];
				if ((pd_e & PTE_P) == 0 ||
				    (pd_e & PTE_PS) != 0)
					continue;
				/* Leaf PT page. */
				pmm_free_page(pd_e & PTE_PA_MASK);
			}
			pmm_free_page(pdpt_e & PTE_PA_MASK);
		}
		pmm_free_page(pdpt_pa);
	}

	pmm_free_page(pm->pm_pml4_pa);
	kfree(pm);
}

bool
pmap_enter(struct pmap *pm, uint64_t va, uint64_t pa, uint32_t flags)
{
	bool	ok;

	if (pm == NULL)
		return (false);

	spin_lock(&pm->pm_lock);
	ok = pmap_enter_locked(pm, va, pa, flags);
	spin_unlock(&pm->pm_lock);

	return (ok);
}

bool
pmap_remove(struct pmap *pm, uint64_t va)
{
	bool	ok;

	if (pm == NULL)
		return (false);

	spin_lock(&pm->pm_lock);
	ok = pmap_remove_locked(pm, va);
	spin_unlock(&pm->pm_lock);

	return (ok);
}

uint64_t
pmap_extract(struct pmap *pm, uint64_t va)
{
	uint64_t	pa;

	if (pm == NULL)
		return (PA_INVALID);

	spin_lock(&pm->pm_lock);
	pa = pmap_extract_locked(pm, va);
	spin_unlock(&pm->pm_lock);

	return (pa);
}

void
pmap_activate(struct pmap *pm)
{
	uint64_t	cr3_cur;

	if (pm == NULL)
		return;

	__asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3_cur));
	if ((cr3_cur & PTE_PA_MASK) == pm->pm_pml4_pa)
		return;
	__asm__ __volatile__ ("mov %0, %%cr3"
	    :
	    : "r"(pm->pm_pml4_pa)
	    : "memory");
}

bool
pmap_kenter(uint64_t va, uint64_t pa, uint32_t flags)
{

	return (pmap_enter(kernel_pmap, va, pa, flags));
}

bool
pmap_kremove(uint64_t va)
{

	return (pmap_remove(kernel_pmap, va));
}

uint64_t
pmap_kextract(uint64_t va)
{

	return (pmap_extract(kernel_pmap, va));
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

	spin_lock(&kernel_pmap->pm_lock);
	leafs  = kernel_pmap->pm_leafs;
	inters = kernel_pmap->pm_intermediates;
	spin_unlock(&kernel_pmap->pm_lock);

	kprintf("pmap: %llu leaf mappings, %llu intermediate tables, "
	    "kernel CR3 = 0x%llx\n",
	    (unsigned long long)leafs,
	    (unsigned long long)inters,
	    (unsigned long long)kernel_pmap->pm_pml4_pa);
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
ensure_table(struct pmap *pm, uint64_t *parent, size_t idx, bool user)
{
	uint64_t	 e, pa;
	uint64_t	*tbl;
	size_t		 i;

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
	for (i = 0; i < 512; i++)
		tbl[i] = 0;

	/*
	 * Intermediate entries are P + RW; the actual permission gate
	 * lives at the leaf.  Letting RW propagate down means a writable
	 * leaf is honoured; clearing RW here would shadow the leaf and
	 * make every page read-only.  US is set on demand from the
	 * caller's intent so ring-3 walks land on user leaves.
	 */
	parent[idx] = pa | PTE_P | PTE_RW | (user ? PTE_US : 0);
	pm->pm_intermediates++;
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
pmap_enter_locked(struct pmap *pm, uint64_t va, uint64_t pa, uint32_t prot)
{
	uint64_t	*pdpt, *pd, *pt;
	uint64_t	 old;
	bool		 user;

	KASSERT((va & PAGE_MASK) == 0, "pmap_enter: unaligned VA");
	KASSERT((pa & PAGE_MASK) == 0, "pmap_enter: unaligned PA");

	user = (prot & VM_PROT_USER) != 0;

	pdpt = ensure_table(pm, pm->pm_pml4, pml4_idx(va), user);
	if (pdpt == NULL)
		return (false);

	pd = ensure_table(pm, pdpt, pdpt_idx(va), user);
	if (pd == NULL)
		return (false);

	pt = ensure_table(pm, pd, pd_idx(va), user);
	if (pt == NULL)
		return (false);

	old = pt[pt_idx(va)];
	pt[pt_idx(va)] = (pa & PTE_PA_MASK) | leaf_flags(prot);

	if (old & PTE_P)
		pmap_invlpg(va);
	else
		pm->pm_leafs++;

	return (true);
}

static bool
pmap_remove_locked(struct pmap *pm, uint64_t va)
{
	uint64_t	*pdpt, *pd, *pt;
	uint64_t	 e;

	e = pm->pm_pml4[pml4_idx(va)];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0)
		return (false);
	pdpt = table_va(e);

	e = pdpt[pdpt_idx(va)];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0)
		return (false);
	pd = table_va(e);

	e = pd[pd_idx(va)];
	if ((e & PTE_P) == 0 || (e & PTE_PS) != 0)
		return (false);
	pt = table_va(e);

	if ((pt[pt_idx(va)] & PTE_P) == 0)
		return (false);

	pt[pt_idx(va)] = 0;
	pmap_invlpg(va);
	pm->pm_leafs--;
	return (true);
}

static uint64_t
pmap_extract_locked(struct pmap *pm, uint64_t va)
{
	uint64_t	*pdpt, *pd, *pt;
	uint64_t	 e, pa;

	e = pm->pm_pml4[pml4_idx(va)];
	if ((e & PTE_P) == 0)
		return (PA_INVALID);
	pdpt = table_va(e);

	e = pdpt[pdpt_idx(va)];
	if ((e & PTE_P) == 0)
		return (PA_INVALID);
	if (e & PTE_PS) {				/* 1 GiB huge page */
		pa = (e & PTE_PA_MASK) | (va & 0x3FFFFFFFULL);
		return (pa);
	}
	pd = table_va(e);

	e = pd[pd_idx(va)];
	if ((e & PTE_P) == 0)
		return (PA_INVALID);
	if (e & PTE_PS) {				/* 2 MiB huge page */
		pa = (e & PTE_PA_MASK) | (va & 0x1FFFFFULL);
		return (pa);
	}
	pt = table_va(e);

	e = pt[pt_idx(va)];
	if ((e & PTE_P) == 0)
		return (PA_INVALID);
	pa = (e & PTE_PA_MASK) | (va & PAGE_MASK);
	return (pa);
}
