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

/*
 * TLB SHOOTDOWN: TELLING THE OTHER PROCESSORS TO FORGET A TRANSLATION.
 *
 * invlpg is a local instruction.  It empties one entry out of the TLB of the
 * processor that executes it and says nothing to any other, and every other
 * processor's TLB is a private cache of the SAME page tables -- so a mapping
 * this CPU has just removed or changed can go on being used elsewhere, with
 * no fault, no message and no bound on how long.  That is not a race that
 * shows up as a crash; it is a stale translation, which is a store landing in
 * a page that now belongs to somebody else.
 *
 * The architecture provides no way to invalidate another processor's TLB.
 * The only way is to ask it to do so itself, which means an interrupt, which
 * means the far processor has to be in a state where it can take one --
 * AND EVERY SPINLOCK IN THIS KERNEL NOW TURNS INTERRUPTS OFF.  That is the
 * whole difficulty of this rung, and it is a deadlock, not a delay:
 *
 *	CPU A takes a pmap's lock, changes a mapping, sends the request and
 *	waits for an acknowledgement.  CPU B is spinning for that same pmap's
 *	lock with interrupts off.  A waits for B to answer; B waits for A to
 *	let go.  Neither is doing anything wrong.
 *
 * So the request is not delivered ONLY by interrupt.  It is published as a
 * serial number, and every loop in this kernel that spins with interrupts off
 * calls pmap_tlb_poll -- kern/spinlock.c's acquire above all.  The interrupt
 * is then an optimisation for processors that are not spinning, and the
 * correctness comes from the poll.  A processor may notice the same request
 * twice, once each way, which is why the answer is a NUMBER IT STORES rather
 * than a counter it decrements: storing the same value twice is nothing, and
 * a second decrement would let the sender leave while a CPU still held the
 * stale entry.
 *
 * One request at a time, under one lock.  A per-CPU queue of pending
 * invalidations would let several proceed at once and would need each entry
 * to be acknowledged separately; with a handful of processors and a shootdown
 * measured in microseconds, the queue is not yet worth the state it takes to
 * be wrong about.
 *
 * ⚠ WHAT IS NOT DONE HERE: narrowing the audience.  Every online processor is
 * asked, including ones that have never had this pmap in CR3 and cannot
 * possibly be holding a translation from it.  Doing better means tracking
 * which CPUs have a pmap active, which is a bitmask maintained by
 * pmap_activate and by the scheduler's switch -- worth doing when the
 * measurement below says it is, and dishonest to claim before then.
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
	 * boot.S sets CR0.WP, and everything this file promises about a
	 * read-only mapping depends on it: with WP clear the read-only bit
	 * binds ring 3 only, and a kernel store through the same VA goes
	 * through without a fault.  Assert it here rather than trust the
	 * assembly, because the failure mode is not a crash -- it is a write
	 * that quietly lands in a page somebody else is also using.
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
 * Tear down a per-task pmap.  Walks the PRIVATE PDPT under PML4[0], frees
 * the PD/PT pages below it that BELONG TO THIS TASK, then frees the PDPT
 * and PML4 pages themselves.  Refusing to destroy kernel_pmap is a hard
 * panic; we never want to be one stray pointer away from unmapping the
 * world.
 *
 * ⚠ WHICH ENTRIES ARE THIS TASK'S IS NOT "ALL BUT SLOT 0".  pmap_create
 * copies the whole kernel PDPT-0 into the private one, deliberately, so a
 * kernel mapping under PML4[0] stays reachable while this pmap is loaded --
 * and every entry it copied names a page table the KERNEL allocated and is
 * still using.  This used to free everything from slot 1 up on the theory
 * that only slot 0 could be shared, which was true only while slot 0 was
 * the only kernel mapping under PML4[0].
 *
 * The local APIC ended that: mapping its registers at 0xFEE00000 puts a PD
 * and a PT under slot 3, every task copied them, and the first task to die
 * handed both back to the page allocator while the kernel was still reading
 * the APIC through them.  The SECOND task to die is what made a noise --
 * pmm's double-free assertion -- and it is worth being clear that the noise
 * was luck.  Had a frame been handed out and written before the second
 * death, the symptom would have been an interrupt controller quietly
 * answering from somebody else's memory.
 *
 * So the rule is one sentence instead of two: an entry the kernel's own
 * PDPT-0 still names is not ours to free, whatever slot it is in.  That
 * also covers slot 0 without a special case.
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
			 * Still the kernel's.  Note this compares the whole
			 * entry, not just the address: an entry the kernel has
			 * since REPLACED would not match and would be freed,
			 * which is only safe because intermediate tables here
			 * are created and never swapped out.
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
 * Carry out whatever invalidation is outstanding, if this CPU has not already.
 *
 * ⚠ MUST BE CALLED WITH INTERRUPTS OFF, which every one of its callers has by
 * construction: the interrupt handler is entered through a gate that clears
 * IF, and the spin loops that call it have just disabled them to take a lock.
 * With interrupts on, curcpu() is a question whose answer can change between
 * the read and the store, and this would credit the wrong processor.
 *
 * Cheap enough to sit in a spin loop: two loads and a compare when there is
 * nothing to do, which is the case every time but the one that matters.
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

	/*
	 * The acquire above is what makes the address safe to read: the sender
	 * wrote it before publishing the number, so a processor that can see
	 * this number can see the address that came with it.
	 */
	pmap_invlpg_local(tlb_va);

	/*
	 * Released last, and with a barrier, because the sender is spinning on
	 * it: it means "the entry is gone from this processor", and the entry
	 * has to actually be gone before it can mean that.
	 */
	__atomic_store_n(&cp->cp_tlb_gen, gen, __ATOMIC_RELEASE);
	return (true);
}

/*
 * The two ways in, counted apart -- because which one does the work is the
 * only evidence there is that the poll is load-bearing rather than
 * decorative.  A processor sitting idle answers by interrupt; a processor
 * spinning for a lock with interrupts off can only answer from the spin, and
 * that is precisely the case that would otherwise deadlock.  If the second
 * number is zero for ever, the argument in the block comment above is a story
 * about a thing that never happens.
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
 * Ask every other online processor to forget `va', and wait until they all
 * say they have.
 *
 * The wait is the point.  Returning before the acknowledgements would leave
 * the caller free to hand the physical page to somebody else while a
 * processor could still reach it through the mapping being removed, which is
 * the exact corruption this whole mechanism exists to prevent.
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
	 * The sender counts itself as done before publishing, because it is:
	 * pmap_invlpg has already run the instruction locally.  Doing it in
	 * this order also keeps the invariant the poll relies on -- no CPU is
	 * ever behind on a generation it has already carried out.
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
				 * Giving up leaves a stale translation on that
				 * processor, which is the very thing this is
				 * for -- so it is counted and named rather
				 * than absorbed.  The alternative is waiting
				 * for ever for a CPU that is not going to
				 * answer, which turns one wrong mapping into a
				 * dead machine.
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

	/*
	 * Nobody to tell.  True for the whole of boot up to the rung that
	 * starts the other processors, and true for ever on a machine with one
	 * -- so the everyday cost of having a shootdown at all is this load.
	 */
	if (cpu_online_count() < 2)
		return;

	pmap_shootdown(va);
}

void
pmap_tlb_init(void)
{

	/*
	 * Installed here rather than beside the other processors' bring-up,
	 * because the vector has to be answerable before the first processor
	 * that could be asked exists -- and because a handler installed by the
	 * code that owns the mechanism is one fewer thing to keep in step.
	 */
	intr_install_local(INTR_VEC_TLB, pmap_tlb_ipi);
}

/*
 * Prove the round trip, and say what it costs.
 *
 * Every acknowledgement is evidence: it can only be stored by the far
 * processor, and only after it has run our handler or our poll, so a
 * processor that had never left the trampoline, or whose IDT was wrong, or
 * whose APIC was not accepting, would show up here as a timeout rather than
 * as a mystery three subsystems later.
 *
 * The address is a page of the kernel's own identity map.  Invalidating a
 * live translation is harmless -- the next access walks the tables and finds
 * the same entry -- and what is being tested is the message, not the mapping.
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
	 * And what talking to the other processors has cost.  The total is the
	 * number that decides whether narrowing the audience is worth doing:
	 * every microsecond here is a processor standing still inside a page
	 * table change, and this is the only place it is visible.
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
