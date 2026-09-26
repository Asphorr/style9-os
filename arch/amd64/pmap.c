/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cpu.h"
#include "intr.h"
#include "kmem.h"
#include "kprintf.h"
#include "lapic.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "spinlock.h"
#include "tsc.h"

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
 * Write-protect: when set, a supervisor-mode store to a page whose PTE has
 * RW clear takes a #PF instead of succeeding.  Clear at reset; boot.S turns
 * it on beside CR0.PG.
 */
#define	CR0_WP			((uint64_t)1 << 16)

/*
 * Per-pmap state.  Lock key:
 *	(c) const after pmap_create / pmap_bootstrap
 *	(p) protected by pm_lock
 *
 * The kernel pmap is the singleton initialised by pmap_bootstrap from
 * the live CR3; user pmaps come from pmap_create, which says what they
 * share with it.
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

/*
 * TLB shootdown.  invlpg empties only the executing CPU's TLB; every other
 * CPU caches the same page tables and may go on using a removed or changed
 * translation, with no fault and no time bound -- a store landing in a page
 * that now belongs to someone else.  There is no remote invalidate, so the
 * other CPUs have to be asked to do it themselves.
 *
 * Asking by interrupt alone deadlocks, because every spinlock here disables
 * interrupts: CPU A holds a pmap's lock and waits for B's acknowledgement
 * while B spins for that lock with interrupts off.  So a request is
 * published as a serial number (tlb_gen), and every loop that spins with
 * interrupts off calls pmap_tlb_poll -- kern/spinlock.c's acquire above all.
 * The IPI only speeds up CPUs that are not spinning.  A CPU may see one
 * request twice, once each way, so it acknowledges by storing the number,
 * which is idempotent, not by decrementing a counter.
 *
 * One request at a time, under tlb_lock; a per-CPU queue is not worth its
 * state at this CPU count.  Every online CPU is asked, including ones that
 * never had the pmap loaded.  Narrowing that needs a mask of CPUs with the
 * pmap active, kept by pmap_activate and the switch -- worth it only if
 * pmap_stats says so.
 */
#define	TLB_WAIT_US		100000	/* absent, not merely slow          */
#define	TLB_LATE_MAX_LINES	8	/* say so, but do not flood         */

static struct spinlock	 tlb_lock = SPINLOCK_INIT("tlb-shootdown");
static volatile uint64_t tlb_gen;	/* (tlb_lock) request serial number */
static volatile uint64_t tlb_va;	/* (tlb_lock) the page to forget    */
static uint64_t		 tlb_requests;	/* (tlb_lock) shootdowns sent       */
static uint64_t		 tlb_ipis;	/* (tlb_lock) messages that left    */
static uint64_t		 tlb_wait_us;	/* (tlb_lock) total spent waiting   */
static uint64_t		 tlb_wait_max_us; /* (tlb_lock) the worst one       */
static uint64_t		 tlb_late;	/* (tlb_lock) never acknowledged    */
static uint64_t		 tlb_late_lines;/* (tlb_lock) ...of them, printed   */
static uint64_t		 tlb_by_ipi;	/* (a) answered by the interrupt    */
static uint64_t		 tlb_by_poll;	/* (a) answered from a spin loop    */

static uint64_t	*table_va(uint64_t pte);
static bool	 pmap_tlb_apply(void);
static void	 pmap_invlpg_local(uint64_t va);
static void	 pmap_shootdown(uint64_t va);
static void	 pmap_tlb_ipi(struct trapframe *tf);
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
	uint64_t	cr0;
	uint64_t	cr3;

	__asm__ __volatile__ ("mov %%cr3, %0" : "=r"(cr3));
	kernel_pmap->pm_pml4_pa = cr3 & PTE_PA_MASK;
	kernel_pmap->pm_pml4    =
	    (uint64_t *)pmm_kva_from_pa(kernel_pmap->pm_pml4_pa);
	kernel_pmap->pm_intermediates = 0;
	kernel_pmap->pm_leafs         = 0;

	/*
	 * boot.S sets CR0.WP.  With it clear, read-only binds ring 3 only and
	 * a kernel store through a read-only mapping succeeds silently, so
	 * assert it rather than trust the assembly.
	 */
	__asm__ __volatile__ ("mov %%cr0, %0" : "=r"(cr0));
	KASSERT((cr0 & CR0_WP) != 0,
	    "pmap_bootstrap: CR0.WP is clear -- ring 0 ignores read-only");

	pmap_tlb_init();

	kprintf("pmap: kernel CR3 = 0x%llx (PML4 at %p), CR0.WP on\n",
	    (unsigned long long)kernel_pmap->pm_pml4_pa,
	    (void *)kernel_pmap->pm_pml4);
}

/*
 * pmap_create: build a fresh per-task PML4.
 *
 *	new_pml4[1..511]	= kernel_pmap->pm_pml4[1..511] (shared)
 *	new_pml4[0]		= pa(new_pdpt0) | P|RW|US
 *	new_pdpt0[0..511]	= kernel PDPT-0 (boot identity map in slot 0,
 *				  kernel MMIO such as the LAPIC in slot 3)
 *
 * Sharing PML4 entries 1..511 shares the next-level pages, so a kernel
 * mapping added there later shows up in every task.  PDPT-0 is private
 * because user VA lives under it (USER_CODE_VA = 1 GiB, PDPT slot 1); the
 * kernel's entries are copied into it so kernel code can still reach them
 * with this pmap loaded.
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

	pm = kmalloc(sizeof(*pm));
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

	/* Share kernel PML4 entries 1..511; entry 0 gets the private PDPT. */
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
	 * All of the kernel PDPT-0, not just slot 0: the LAPIC mapping sits in
	 * slot 3.  User VA (1-2 GiB) is slot 1, which the kernel leaves empty.
	 */
	for (i = 0; i < 512; i++)
		new_pdpt0[i] = kern_pdpt0[i];

	/* US=1 so the ring-3 walk gets through; each leaf decides access. */
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
 * Tear down a per-task pmap: free this task's PD/PT pages under the
 * private PDPT, then the PDPT and the PML4.  Destroying kernel_pmap panics.
 *
 * An entry the kernel's own PDPT-0 still names is the kernel's, whatever
 * its slot: pmap_create copied all of PDPT-0, so the copy names page
 * tables the kernel is still using (the LAPIC's, at 0xFEE00000, under
 * slot 3).  Freeing them would leave the kernel reaching the APIC through
 * recycled memory.
 */
void
pmap_destroy(struct pmap *pm)
{
	uint64_t	*pdpt;
	uint64_t	*pd;
	uint64_t	*kern_pdpt0;
	uint64_t	 pml4_e, pdpt_e, pd_e;
	uint64_t	 kern_pml4_e;
	uint64_t	 pdpt_pa;
	size_t		 i, j;

	if (pm == NULL)
		return;
	if (pm->pm_is_kernel)
		panic("pmap_destroy: attempt to destroy kernel_pmap");

	kern_pdpt0  = NULL;
	kern_pml4_e = kernel_pmap->pm_pml4[0];
	if ((kern_pml4_e & PTE_P) != 0 && (kern_pml4_e & PTE_PS) == 0)
		kern_pdpt0 = (uint64_t *)
		    pmm_kva_from_pa(kern_pml4_e & PTE_PA_MASK);

	pml4_e = pm->pm_pml4[0];
	if ((pml4_e & PTE_P) != 0 && (pml4_e & PTE_PS) == 0) {
		pdpt_pa = pml4_e & PTE_PA_MASK;
		pdpt    = (uint64_t *)pmm_kva_from_pa(pdpt_pa);

		for (i = 0; i < 512; i++) {
			pdpt_e = pdpt[i];
			if ((pdpt_e & PTE_P) == 0 || (pdpt_e & PTE_PS) != 0)
				continue;
			/*
			 * Still the kernel's.  The whole entry is compared,
			 * so one the kernel has since replaced would be freed;
			 * safe only because kernel intermediate tables are
			 * never swapped out.
			 */
			if (kern_pdpt0 != NULL && kern_pdpt0[i] == pdpt_e)
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

uint64_t
pmap_kernel_root_pa(void)
{

	return (kernel_pmap->pm_pml4_pa);
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

static void
pmap_invlpg_local(uint64_t va)
{

	__asm__ __volatile__ ("invlpg (%0)" :: "r"((uintptr_t)va) : "memory");
}

/*
 * Carry out the outstanding invalidation, if this CPU has not already.
 *
 * Interrupts must be off, as they are for every caller (the IPI gate clears
 * IF; the spin loops have just disabled them): with them on, curcpu() can
 * change between the read and the store and credit the wrong CPU.  Two
 * loads and a compare when there is nothing to do, so cheap in a spin loop.
 */
static bool
pmap_tlb_apply(void)
{
	struct cpu	*cp;
	uint64_t	 gen;

	gen = __atomic_load_n(&tlb_gen, __ATOMIC_ACQUIRE);
	cp  = curcpu();
	if (cp->cp_tlb_gen == gen)
		return (false);

	/* The acquire above pairs with the sender's; tlb_va was set first. */
	pmap_invlpg_local(tlb_va);

	/* Release: the sender spins on this, so it must follow the invlpg. */
	__atomic_store_n(&cp->cp_tlb_gen, gen, __ATOMIC_RELEASE);
	return (true);
}

/*
 * The two ways in are counted apart.  An idle CPU answers by interrupt; one
 * spinning for a lock with interrupts off can answer only from the spin,
 * which is the case that would otherwise deadlock.  tlb_by_poll staying zero
 * would mean the poll is not load-bearing.
 */
void
pmap_tlb_poll(void)
{

	if (pmap_tlb_apply())
		__atomic_fetch_add(&tlb_by_poll, 1, __ATOMIC_RELAXED);
}

static void
pmap_tlb_ipi(struct trapframe *tf)
{

	(void)tf;
	if (pmap_tlb_apply())
		__atomic_fetch_add(&tlb_by_ipi, 1, __ATOMIC_RELAXED);
}

/*
 * Ask every other online CPU to forget `va' and wait until all have.  The
 * wait is the point: the caller may free the page as soon as this returns.
 */
static void
pmap_shootdown(uint64_t va)
{
	struct cpu	*me;
	uint64_t	 gen;
	uint64_t	 t0;
	uint64_t	 us;
	unsigned int	 present;
	unsigned int	 sent;
	unsigned int	 i;

	spin_lock(&tlb_lock);

	me      = curcpu();
	present = cpu_present_count();

	/*
	 * The sender marks itself done before publishing (pmap_invlpg already
	 * ran invlpg here), keeping the invariant the poll relies on: no CPU
	 * is ever behind on a generation it has already carried out.
	 */
	tlb_va = va;
	gen = tlb_gen + 1;
	me->cp_tlb_gen = gen;
	__atomic_store_n(&tlb_gen, gen, __ATOMIC_RELEASE);

	sent = 0;
	for (i = 0; i < present; i++) {
		if (&cpus[i] == me || cpus[i].cp_online == 0)
			continue;
		if (lapic_ipi_vector(cpus[i].cp_lapic_id, INTR_VEC_TLB))
			sent++;
	}

	t0 = tsc_read();
	for (i = 0; i < present; i++) {
		if (&cpus[i] == me || cpus[i].cp_online == 0)
			continue;
		while (__atomic_load_n(&cpus[i].cp_tlb_gen,
		    __ATOMIC_ACQUIRE) != gen) {
			if (tsc_to_us(tsc_read() - t0) > TLB_WAIT_US) {
				/*
				 * Give up, counted and named: that CPU may
				 * keep the stale entry, but waiting for ever
				 * on one that will not answer turns a wrong
				 * mapping into a dead machine.
				 */
				tlb_late++;
				if (tlb_late_lines < TLB_LATE_MAX_LINES) {
					tlb_late_lines++;
					kprintf("pmap: cpu %u did not "
					    "acknowledge the invalidation of "
					    "0x%llx in %u us -- it may still "
					    "hold it\n", (unsigned int)i,
					    (unsigned long long)va,
					    (unsigned int)TLB_WAIT_US);
				}
				break;
			}
			__asm__ __volatile__ ("pause");
		}
	}
	us = tsc_to_us(tsc_read() - t0);

	tlb_requests++;
	tlb_ipis += sent;
	tlb_wait_us += us;
	if (us > tlb_wait_max_us)
		tlb_wait_max_us = us;

	spin_unlock(&tlb_lock);
}

void
pmap_invlpg(uint64_t va)
{

	pmap_invlpg_local(va);

	/* Nobody to tell until an AP is online; on one CPU this load is all. */
	if (cpu_online_count() < 2)
		return;

	pmap_shootdown(va);
}

void
pmap_tlb_init(void)
{

	/* Answerable before the first CPU that could be asked is started. */
	intr_install_local(INTR_VEC_TLB, pmap_tlb_ipi);
}

/*
 * Prove the round trip and time it.  Only the far CPU can store its
 * acknowledgement, after running our handler or poll, so an AP that never
 * left the trampoline, has a wrong IDT or a deaf APIC shows up here as a
 * timeout.  The address is a live page of the kernel's identity map;
 * invalidating it is harmless.
 */
#define	TLB_TEST_ROUNDS		1000

void
pmap_tlb_selftest(void)
{
	uint64_t	req0;
	uint64_t	us0;
	uint64_t	late0;
	uint64_t	us;
	uint64_t	va;
	unsigned int	i;

	if (cpu_online_count() < 2) {
		kprintf("tlb-shootdown: only one processor is running -- "
		    "nothing to tell\n");
		return;
	}

	req0  = tlb_requests;
	us0   = tlb_wait_us;
	late0 = tlb_late;
	va    = (uint64_t)(uintptr_t)&kernel_pmap_store & ~(uint64_t)PAGE_MASK;

	for (i = 0; i < TLB_TEST_ROUNDS; i++)
		pmap_invlpg(va);

	us = tlb_wait_us - us0;

	if (tlb_requests - req0 != TLB_TEST_ROUNDS) {
		kprintf("tlb-shootdown: FAIL %llu of %u rounds were sent\n",
		    (unsigned long long)(tlb_requests - req0),
		    (unsigned int)TLB_TEST_ROUNDS);
		return;
	}
	if (tlb_late != late0) {
		kprintf("tlb-shootdown: FAIL %llu round(s) went unanswered\n",
		    (unsigned long long)(tlb_late - late0));
		return;
	}

	kprintf("tlb-shootdown: PASS -- %u invalidations, every one "
	    "acknowledged by all %u other processor(s), %llu us each on "
	    "average and %llu us at worst\n",
	    (unsigned int)TLB_TEST_ROUNDS,
	    (unsigned int)(cpu_online_count() - 1),
	    (unsigned long long)(us / TLB_TEST_ROUNDS),
	    (unsigned long long)tlb_wait_max_us);
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

	/*
	 * Shootdown cost: the total decides whether narrowing the audience is
	 * worth doing.
	 */
	spin_lock(&tlb_lock);
	kprintf("pmap: %llu shootdown(s), %llu message(s) sent, %llu ms "
	    "waiting -- %llu us each, %llu us at worst%s\n",
	    (unsigned long long)tlb_requests,
	    (unsigned long long)tlb_ipis,
	    (unsigned long long)(tlb_wait_us / 1000),
	    (unsigned long long)(tlb_requests == 0 ? 0 :
	    tlb_wait_us / tlb_requests),
	    (unsigned long long)tlb_wait_max_us,
	    tlb_late != 0 ? "  *** SOME WENT UNANSWERED ***" : "");
	spin_unlock(&tlb_lock);

	kprintf("pmap: %llu answered by the interrupt, %llu from inside a "
	    "spin loop\n",
	    (unsigned long long)__atomic_load_n(&tlb_by_ipi, __ATOMIC_RELAXED),
	    (unsigned long long)__atomic_load_n(&tlb_by_poll,
	    __ATOMIC_RELAXED));
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
		 * A user leaf under a kernel-only intermediate: promote US,
		 * which the walk needs at every level.  US=1 above kernel
		 * leaves is harmless; the leaf's own US bit decides.
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
	 * Intermediates are P|RW, plus US on demand; the leaf decides.  RW
	 * clear here would make every leaf below read-only.
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
