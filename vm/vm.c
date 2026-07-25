/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "kmem.h"
#include "kprintf.h"
#include "panic.h"
#include "pmap.h"
#include "pmm.h"
#include "smap.h"
#include "spinlock.h"
#include "vm.h"
#include "vm_object.h"

/*
 * 4 KiB page constants -- duplicated locally so vm.c does not have to
 * pull arch/amd64/pmap.h.  If you ever change the kernel's page size
 * here is one of the spots to grep for.
 */
#define	VM_PAGE_SHIFT		12u
#define	VM_PAGE_SIZE		(1ull << VM_PAGE_SHIFT)
#define	VM_PAGE_MASK		(VM_PAGE_SIZE - 1u)

#define	VM_ALIGN_DOWN(x)	((x) & ~VM_PAGE_MASK)
#define	VM_ALIGN_UP(x)		(((x) + VM_PAGE_MASK) & ~VM_PAGE_MASK)

static void	vm_print_entry(const struct vm_map_entry *);
static bool	vm_map_clip_locked(struct vm_map *, uint64_t at);

/* Fault counters.  Plain writes -- they are diagnostics, not state. */
static uint64_t	vm_n_fault_zero;	/* pages zero-filled            */
static uint64_t	vm_n_fault_file;	/* pages paged in from a file   */
static uint64_t	vm_n_fault_race;	/* someone else got there first */
static uint64_t	vm_n_fault_fail;	/* refused or could not fill    */
static uint64_t	vm_us_pager;		/* microseconds inside the pager */
static uint64_t	vm_us_pager_max;	/* worst single page            */
static uint64_t	vm_n_fork_shared;	/* pages fork shared, not copied */
static uint64_t	vm_n_cow_copy;		/* write faults that copied     */
static uint64_t	vm_n_cow_steal;		/* ...that found no other owner */
static uint64_t	vm_n_cow_race;		/* ...that lost to another one  */
static uint64_t	vm_n_image_borrowed;	/* program pages mapped in situ */
static uint64_t	vm_n_image_copied;	/* ...allocated and filled      */
static uint64_t	vm_n_payload_shared;	/* OOL pages shared with sender */
static uint64_t	vm_n_payload_copied;	/* ...that had to be copied     */

/*
 * Cutting.  Two counters rather than one, because they answer different
 * questions and only the pair is informative: vm_n_split says the mechanism
 * ran at all, vm_n_rel_cut says it changed an outcome.  A release that needed
 * no cut is one the map could always serve; a release that needed one is a
 * request that used to come back refused.
 */
static uint64_t	vm_n_split;		/* entries cut in two             */
static uint64_t	vm_n_rel_whole;		/* released without cutting       */
static uint64_t	vm_n_rel_cut;		/* released only because we can   */
static uint64_t	vm_n_rel_refuse;	/* hole, stray, or not ours       */

/*
 * Drop one entry.  An entry owns a reference on its object, so releasing the
 * storage has to release that too -- this is the single place that knows it.
 */
static void
entry_free(struct vm_map_entry *e)
{

	if (e == NULL)
		return;
	vm_object_deref(e->vme_object);
	kfree(e);
}

void
vm_init(void)
{

	/*
	 * No global state to bring up yet -- per-VM maps are allocated
	 * lazily via vm_map_create.  Keep the symbol around so a future
	 * shared "kernel template VM" (for the per-task PML4 commit)
	 * can plug in here without churn at the call sites.
	 */
}

struct vm_map *
vm_map_create(uint64_t lo, uint64_t hi)
{
	struct vm_map	*map;

	if (lo >= hi || (lo & VM_PAGE_MASK) != 0 || (hi & VM_PAGE_MASK) != 0)
		return (NULL);

	map = kmalloc(sizeof(*map));
	if (map == NULL)
		return (NULL);

	spin_init(&map->vm_lock, "vm_map");
	map->vm_lo    = lo;
	map->vm_hi    = hi;
	map->vm_hint  = lo;
	map->vm_head  = NULL;
	map->vm_count = 0;
	return (map);
}

void
vm_map_destroy(struct vm_map *map)
{
	struct vm_map_entry	*e, *next;

	if (map == NULL)
		return;

	/*
	 * No lock: a map should only ever be destroyed after its owning
	 * task has stopped running, so concurrent mutators are
	 * impossible.  KASSERT'd via the caller (task_deref) which
	 * holds the "task is dead" invariant.
	 */
	e = map->vm_head;
	while (e != NULL) {
		next = e->vme_next;
		entry_free(e);
		e = next;
	}
	kfree(map);
}

void
vm_map_release_anon(struct vm_map *map, struct pmap *pm)
{
	struct vm_map_entry	*e;
	uint64_t		 va;
	uint64_t		 pa;

	if (map == NULL || pm == NULL)
		return;

	/*
	 * Same single-thread invariant as vm_map_destroy -- this is the
	 * tail of task teardown, no mutators are racing us.  Anonymous
	 * entries own their frames -- which since fork learned to share can
	 * mean owning one of several references, so each present leaf is
	 * unmapped and its frame released, and pmm decides whether that was
	 * the last claim on it.
	 */
	for (e = map->vm_head; e != NULL; e = e->vme_next) {
		if ((e->vme_flags & VME_F_ANON) == 0)
			continue;
		for (va = e->vme_start; va < e->vme_end; va += VM_PAGE_SIZE) {
			pa = pmap_extract(pm, va);
			if (pa == PA_INVALID)
				continue;
			(void)pmap_remove(pm, va);
			pmm_free_page(pa);
		}
	}
}

bool
vm_map_release(struct vm_map *map, struct pmap *pm,
    uint64_t va, uint64_t size)
{
	struct vm_map_entry	*e;
	uint64_t		 end;
	uint64_t		 pa;
	uint64_t		 v;
	uint64_t		 covered;
	bool			 cut;

	if (map == NULL || pm == NULL || size == 0)
		return (false);
	if ((va & VM_PAGE_MASK) != 0 || (size & VM_PAGE_MASK) != 0)
		return (false);

	end = va + size;
	if (end < va)
		return (false);
	if (va < map->vm_lo || end > map->vm_hi)
		return (false);

	spin_lock(&map->vm_lock);

	/*
	 * Establish that the whole range is ours to take before touching any
	 * of it.  Two separate questions, and the walk answers both:
	 *
	 *	Is it covered?  A hole is refused rather than skipped over.
	 *	  POSIX would have munmap succeed on unmapped pages, but a
	 *	  caller here naming an address it does not own has made a
	 *	  mistake, and this kernel would rather say so.  Refusing also
	 *	  keeps the answer to a stray munmap what it has always been.
	 *
	 *	Is every entry anonymous?  Checked per entry, because the flag
	 *	  decides whether the frames beneath may be handed back.  A
	 *	  borrowed image page belongs to the kernel and is shared by
	 *	  every task running that program; returning one to the
	 *	  allocator would not be merely wrong, it would be quiet.
	 */
	covered = va;
	cut     = false;
	for (e = map->vm_head; e != NULL && covered < end; e = e->vme_next) {
		if (e->vme_end <= covered)
			continue;
		if (e->vme_start > covered)
			break;			/* a hole */
		if ((e->vme_flags & VME_F_ANON) == 0)
			break;			/* not ours to free */
		if (e->vme_start < va || e->vme_end > end)
			cut = true;		/* sticks out; needs cutting */
		covered = e->vme_end;
	}
	if (covered < end) {
		vm_n_rel_refuse++;
		spin_unlock(&map->vm_lock);
		return (false);
	}

	/*
	 * Cut the edges, so every entry overlapping the request now lies
	 * wholly inside it and vm_map_remove takes them all.  Failure here is
	 * out of memory; it can leave the first cut made, and that is harmless
	 * -- two entries describing what one described before is a different
	 * shape for the same memory, not a wrong one.
	 */
	if (!vm_map_clip_locked(map, va) || !vm_map_clip_locked(map, end)) {
		vm_n_rel_refuse++;
		spin_unlock(&map->vm_lock);
		return (false);
	}
	if (cut)
		vm_n_rel_cut++;
	else
		vm_n_rel_whole++;
	spin_unlock(&map->vm_lock);

	for (v = va; v < end; v += VM_PAGE_SIZE) {
		pa = pmap_extract(pm, v);
		if (pa == PA_INVALID)
			continue;
		(void)pmap_remove(pm, v);
		pmm_free_page(pa);
	}

	(void)vm_map_remove(map, va, size);
	return (true);
}

void
vm_map_reset(struct vm_map *map)
{
	struct vm_map_entry	*e, *next;

	if (map == NULL)
		return;

	/*
	 * Same no-lock single-thread invariant as vm_map_destroy: the
	 * owning task sits between images inside execve, and its only
	 * thread is the one running this reset.
	 */
	e = map->vm_head;
	while (e != NULL) {
		next = e->vme_next;
		entry_free(e);
		e = next;
	}
	map->vm_head  = NULL;
	map->vm_count = 0;
	map->vm_hint  = map->vm_lo;
}

bool
vm_map_fork_share(struct vm_map *src, struct pmap *src_pm,
    struct vm_map *dst, struct pmap *dst_pm)
{
	struct vm_map_entry	*e;
	uint64_t		 pa;
	uint64_t		 va;
	uint8_t			 flags;
	uint8_t			 shared_prot;
	bool			 owned;
	bool			 writable;

	if (src == NULL || src_pm == NULL || dst == NULL || dst_pm == NULL)
		return (false);

	/*
	 * No lock on src: the parent's only thread is parked in the fork
	 * syscall, so no mutator can race the walk (the same invariant
	 * vm_map_release_anon documents).  dst belongs to a task that has
	 * not run yet.  That same quiescence is what lets the loop below
	 * rewrite the parent's own flags and page tables underneath it.
	 *
	 * Holes inside an entry (PA_INVALID) stay holes, mirroring exactly
	 * what the parent had faulted in -- which for an eagerly loaded
	 * segment means "everything", and for a lazy mapping means only the
	 * pages the parent actually touched.  The child inherits the entry's
	 * pager along with its range, so a page neither of them has touched
	 * yet still knows where to come from.
	 */
	for (e = src->vm_head; e != NULL; e = e->vme_next) {
		writable = (e->vme_prot & VM_PROT_WRITE) != 0;

		/*
		 * A writable range becomes copy-on-write in BOTH maps.  Marking
		 * only the child would be the classic half-fix: the parent
		 * would keep a writable page table entry over a frame the child
		 * can see, and its next store would be delivered to both of
		 * them without ever faulting.  Which of the two writes first is
		 * not knowable here, so neither is allowed to.
		 *
		 * A read-only range needs none of this.  It is shared outright
		 * and stays shared; a store into it was already a violation
		 * before fork and still is, so there is nothing for a fault to
		 * usefully do.
		 */
		flags = e->vme_flags;
		shared_prot = e->vme_prot;
		if (writable) {
			flags |= VME_F_COW;
			shared_prot = (uint8_t)(e->vme_prot & ~VM_PROT_WRITE);
		}

		vm_object_ref(e->vme_object);
		if (!vm_map_enter_backed(dst, e->vme_start,
		    e->vme_end - e->vme_start, e->vme_prot, flags,
		    e->vme_object, e->vme_offset)) {
			vm_object_deref(e->vme_object);
			return (false);
		}
		e->vme_flags = flags;

		/*
		 * Frames under a borrowed range -- a program image mapped
		 * straight out of the kernel -- are not the allocator's to
		 * count.  They were never handed out, nothing will ever hand
		 * them back, and asking pmm to record one more owner of a
		 * frame it does not consider allocated is a bug it will say so
		 * about.  The child gets the same mapping and that is all.
		 */
		owned = (e->vme_flags & VME_F_ANON) != 0;

		for (va = e->vme_start; va < e->vme_end; va += VM_PAGE_SIZE) {
			pa = pmap_extract(src_pm, va);
			if (pa == PA_INVALID)
				continue;
			/*
			 * Order matters: take the reference BEFORE installing
			 * the second mapping.  The reverse would leave a window
			 * where two page tables reach a frame the allocator
			 * still believes has one owner, and any teardown inside
			 * that window would hand a live page back.
			 */
			if (owned)
				pmm_page_ref(pa);
			if (!pmap_enter(dst_pm, va, pa, shared_prot)) {
				if (owned)
					pmm_free_page(pa);
				return (false);
			}
			if (writable &&
			    !pmap_enter(src_pm, va, pa, shared_prot))
				return (false);
			vm_n_fork_shared++;
		}
	}
	return (true);
}

/* ---- payloads of pages, carried between address spaces ------------------ */

/*
 * Fill one freshly allocated frame with the slice of a payload that belongs
 * in it, zeroing whatever the payload does not reach.  `src` may be a user
 * address, which is why the caller must hold no lock: touching a page the
 * sender has not faulted in yet takes a fault, and that fault wants the map
 * lock this would otherwise be holding.
 */
static uint64_t
page_from_bytes(const uint8_t *src, uint64_t off, uint64_t size, bool user)
{
	uint8_t		*kva;
	uint64_t	 pa;
	uint64_t	 have;
	size_t		 i;

	pa = pmm_alloc_page();
	if (pa == PA_INVALID)
		return (PA_INVALID);
	kva = (uint8_t *)pmm_kva_from_pa(pa);

	have = size - off;
	if (have > VM_PAGE_SIZE)
		have = VM_PAGE_SIZE;

	if (user)
		smap_user_access_begin();
	for (i = 0; i < have; i++)
		kva[i] = src[off + i];
	if (user)
		smap_user_access_end();
	for (i = (size_t)have; i < VM_PAGE_SIZE; i++)
		kva[i] = 0;

	return (pa);
}

size_t
vm_pages_capture_kernel(const void *src, uint64_t size, uint64_t *pa_out,
    size_t max_pages)
{
	size_t	i, npages;

	if (src == NULL || pa_out == NULL || size == 0)
		return (0);
	npages = (size_t)(VM_ALIGN_UP(size) >> VM_PAGE_SHIFT);
	if (npages > max_pages)
		return (0);

	for (i = 0; i < npages; i++) {
		pa_out[i] = page_from_bytes((const uint8_t *)src,
		    (uint64_t)i * VM_PAGE_SIZE, size, false);
		if (pa_out[i] == PA_INVALID) {
			vm_pages_release(pa_out, i);
			return (0);
		}
		vm_n_payload_copied++;
	}
	return (npages);
}

size_t
vm_pages_capture_user(struct vm_map *map, struct pmap *pm, uint64_t addr,
    uint64_t size, uint64_t *pa_out, size_t max_pages)
{
	struct vm_map_entry	*e;
	uint64_t		 va;
	size_t			 i, npages, taken;

	if (map == NULL || pm == NULL || pa_out == NULL || size == 0)
		return (0);
	npages = (size_t)(VM_ALIGN_UP(size) >> VM_PAGE_SHIFT);
	if (npages > max_pages)
		return (0);

	for (i = 0; i < npages; i++)
		pa_out[i] = PA_INVALID;

	/*
	 * First pass, under the lock: claim every page that can be shared.
	 * Nothing here can fault -- it reads page tables, not memory -- which
	 * is what makes it safe to do with the map lock held, and the copies
	 * that cannot be avoided are deliberately left to the second pass.
	 */
	taken = 0;
	if ((addr & VM_PAGE_MASK) == 0) {
		spin_lock(&map->vm_lock);
		e = vm_map_lookup(map, addr);
		/*
		 * One anonymous entry has to cover the whole payload.  A range
		 * that spans entries would need each one marked, and a range
		 * over borrowed frames must not be reference-counted at all
		 * (vm/pmm.h) -- neither is worth the code for a case that only
		 * arises when a sender points at something other than a buffer
		 * it allocated.  Both simply fall through to copying.
		 */
		if (e != NULL && (e->vme_flags & VME_F_ANON) != 0 &&
		    e->vme_end >= addr + (uint64_t)npages * VM_PAGE_SIZE) {
			for (i = 0; i < npages; i++) {
				uint64_t	pa;

				/* Partial last page: never shared. */
				if ((uint64_t)(i + 1) * VM_PAGE_SIZE > size)
					break;
				va = addr + (uint64_t)i * VM_PAGE_SIZE;
				pa = pmap_extract(pm, va);
				if (pa == PA_INVALID)
					continue;	/* not faulted in yet */
				pmm_page_ref(pa);
				/*
				 * The sender keeps reading it and stops being
				 * able to write it.  A range that was already
				 * read-only needs neither.
				 */
				if ((e->vme_prot & VM_PROT_WRITE) != 0 &&
				    !pmap_enter(pm, va, pa, (uint8_t)
				    (e->vme_prot & ~VM_PROT_WRITE))) {
					pmm_free_page(pa);
					continue;
				}
				pa_out[i] = pa;
				taken++;
			}
			if (taken != 0 && (e->vme_prot & VM_PROT_WRITE) != 0)
				e->vme_flags |= VME_F_COW;
		}
		spin_unlock(&map->vm_lock);
	}
	vm_n_payload_shared += taken;

	/* Second pass, nothing held: everything sharing could not take. */
	for (i = 0; i < npages; i++) {
		if (pa_out[i] != PA_INVALID)
			continue;
		pa_out[i] = page_from_bytes((const uint8_t *)(uintptr_t)addr,
		    (uint64_t)i * VM_PAGE_SIZE, size, true);
		if (pa_out[i] == PA_INVALID) {
			vm_pages_release(pa_out, npages);
			return (0);
		}
		vm_n_payload_copied++;
	}
	return (npages);
}

bool
vm_pages_install(struct vm_map *map, struct pmap *pm, const uint64_t *pas,
    size_t npages, uint64_t *va_out)
{
	uint64_t	 landing;
	uint64_t	 span;
	size_t		 i;

	if (map == NULL || pm == NULL || pas == NULL || npages == 0 ||
	    va_out == NULL)
		return (false);

	span = (uint64_t)npages * VM_PAGE_SIZE;
	if (!vm_map_find_space(map, span, &landing))
		return (false);

	/*
	 * The entry says writable, the hardware says not.  That gap is the
	 * mechanism: a receiver that only reads goes on sharing the sender's
	 * frames, and one that writes takes a fault and gets its own page.
	 */
	if (!vm_map_enter(map, landing, span,
	    VM_PROT_READ | VM_PROT_WRITE | VM_PROT_USER,
	    VME_F_ANON | VME_F_COW))
		return (false);

	for (i = 0; i < npages; i++) {
		if (pmap_enter(pm, landing + (uint64_t)i * VM_PAGE_SIZE,
		    pas[i], VM_PROT_READ | VM_PROT_USER))
			continue;
		/*
		 * Undo the mappings but NOT the frames: they still belong to
		 * whoever captured them, who will release them when it learns
		 * this failed.  Freeing here as well is the double-free this
		 * split of ownership exists to prevent.
		 */
		while (i-- > 0)
			(void)pmap_remove(pm,
			    landing + (uint64_t)i * VM_PAGE_SIZE);
		(void)vm_map_remove(map, landing, span);
		return (false);
	}

	*va_out = landing;
	return (true);
}

void
vm_pages_release(const uint64_t *pas, size_t npages)
{
	size_t	i;

	if (pas == NULL)
		return;
	for (i = 0; i < npages; i++)
		if (pas[i] != PA_INVALID)
			pmm_free_page(pas[i]);
}

void
vm_pages_stats(void)
{
	uint64_t	total;

	total = vm_n_payload_shared + vm_n_payload_copied;
	if (total == 0)
		return;
	kprintf("vm: %llu message payload pages -- %llu shared with the "
	    "sender, %llu copied\n",
	    (unsigned long long)total,
	    (unsigned long long)vm_n_payload_shared,
	    (unsigned long long)vm_n_payload_copied);
}

/* See vm.h for what this decides and why both loaders share it. */
int
vm_map_image(struct vm_map *map, struct pmap *pm, uint64_t seg_va,
    uint64_t vmsize, uint64_t filesize, const void *bytes, uint8_t prot)
{
	const uint8_t	*src;
	uint8_t		*kva;
	uint64_t	 borrow_start, borrow_end;
	uint64_t	 img_pa;
	uint64_t	 va, va_start, va_end;
	uint64_t	 hi, lo;
	uint64_t	 pa;
	size_t		 i;

	if (map == NULL || pm == NULL || bytes == NULL || filesize > vmsize)
		return (VM_IMAGE_MAP);
	if (vmsize == 0)
		return (VM_IMAGE_OK);

	src      = (const uint8_t *)bytes;
	va_start = seg_va & ~VM_PAGE_MASK;
	va_end   = VM_ALIGN_UP(seg_va + vmsize);

	/*
	 * The half-open range of pages that can point at the image itself.
	 * Empty unless all three conditions in vm.h hold.
	 */
	img_pa       = pmm_pa_from_kva(src);
	borrow_start = va_start;
	borrow_end   = va_start;
	if ((prot & VM_PROT_WRITE) == 0 &&
	    ((img_pa ^ seg_va) & VM_PAGE_MASK) == 0) {
		borrow_start = VM_ALIGN_UP(seg_va);
		borrow_end   = VM_ALIGN_DOWN(seg_va + filesize);
		if (borrow_end <= borrow_start) {
			borrow_start = va_start;
			borrow_end   = va_start;
		}
	}

	/*
	 * Entries before frames, deliberately.  A failure part-way through
	 * the population loop below leaves the task holding frames that are
	 * mapped but not described, and teardown walks the map -- so an entry
	 * that already covers them is the difference between a reclaimed
	 * partial load and a leaked one.
	 */
	if (borrow_end > borrow_start) {
		if (borrow_start > va_start &&
		    !vm_map_enter(map, va_start, borrow_start - va_start,
		    prot, VME_F_ANON))
			return (VM_IMAGE_MAP);
		if (!vm_map_enter(map, borrow_start, borrow_end - borrow_start,
		    prot, 0))
			return (VM_IMAGE_MAP);
		if (va_end > borrow_end &&
		    !vm_map_enter(map, borrow_end, va_end - borrow_end,
		    prot, VME_F_ANON))
			return (VM_IMAGE_MAP);
	} else if (!vm_map_enter(map, va_start, va_end - va_start, prot,
	    VME_F_ANON))
		return (VM_IMAGE_MAP);

	for (va = va_start; va < va_end; va += VM_PAGE_SIZE) {
		if (va >= borrow_start && va < borrow_end) {
			if (!pmap_enter(pm, va, img_pa + (va - seg_va), prot))
				return (VM_IMAGE_MAP);
			vm_n_image_borrowed++;
			continue;
		}

		pa = pmm_alloc_page();
		if (pa == PA_INVALID)
			return (VM_IMAGE_NOMEM);
		kva = (uint8_t *)pmm_kva_from_pa(pa);
		for (i = 0; i < VM_PAGE_SIZE; i++)
			kva[i] = 0;

		/*
		 * Whatever part of this page the file covers.  Zeroes are
		 * already right for the rest, which is the BSS tail and the
		 * slack before a segment that does not start on a boundary.
		 */
		lo = (va > seg_va) ? va : seg_va;
		hi = va + VM_PAGE_SIZE;
		if (hi > seg_va + filesize)
			hi = seg_va + filesize;
		for (i = 0; lo + i < hi; i++)
			kva[lo - va + i] = src[lo - seg_va + i];

		if (!pmap_enter(pm, va, pa, prot)) {
			pmm_free_page(pa);
			return (VM_IMAGE_MAP);
		}
		vm_n_image_copied++;
	}

	return (VM_IMAGE_OK);
}

void
vm_image_stats(void)
{
	uint64_t	total;

	total = vm_n_image_borrowed + vm_n_image_copied;
	if (total == 0)
		return;
	kprintf("vm: %llu program pages -- %llu borrowed from the kernel "
	    "image, %llu copied (%llu KiB saved)\n",
	    (unsigned long long)total,
	    (unsigned long long)vm_n_image_borrowed,
	    (unsigned long long)vm_n_image_copied,
	    (unsigned long long)(vm_n_image_borrowed * VM_PAGE_SIZE / 1024));
}

void
vm_map_stats(void)
{

	kprintf("vm: %llu ranges released -- %llu whole, %llu needing a cut "
	    "(%llu entries split), %llu refused\n",
	    (unsigned long long)(vm_n_rel_whole + vm_n_rel_cut),
	    (unsigned long long)vm_n_rel_whole,
	    (unsigned long long)vm_n_rel_cut,
	    (unsigned long long)vm_n_split,
	    (unsigned long long)vm_n_rel_refuse);
}

/*
 * Turn a promise into memory.
 *
 * The shape of this is dictated by one rule: the pager sleeps.  Reading a
 * file-backed page goes down through the filesystem to the disk driver, which
 * blocks waiting for the drive's interrupt, and a thread that blocks while
 * holding a spinlock in this kernel never wakes up again (kern/bio.c has the
 * full account).  So the map lock is taken twice with the slow part outside
 * both windows: once to read what the entry promises, then again to install
 * the result -- and the second window has to re-examine everything the first
 * one learned, because the range can be unmapped and another thread can fault
 * the same page while this one is down at the disk.
 *
 * Both races end the same cheap way.  If the page is already present when we
 * come back, the frame we prepared is simply freed: the loser of the race
 * built a copy nobody needs, and the winner's page is just as correct.
 */
int
vm_fault(struct vm_map *map, struct pmap *pm, uint64_t va, bool write)
{
	struct vm_map_entry	*e;
	struct vm_object	*obj;
	uint8_t			*kva;
	uint64_t		 page;
	uint64_t		 off;
	uint64_t		 old;
	uint64_t		 pa;
	uint8_t			 prot;
	size_t			 i;

	if (map == NULL || pm == NULL)
		return (VM_FAULT_NOMAP);
	page = VM_ALIGN_DOWN(va);

	spin_lock(&map->vm_lock);
	e = vm_map_lookup(map, page);
	if (e == NULL) {
		spin_unlock(&map->vm_lock);
		vm_n_fault_fail++;
		return (VM_FAULT_NOMAP);
	}
	/*
	 * A range mapped PROT_NONE is a reservation, not memory.  Filling it
	 * would install a leaf the faulting thread still cannot touch, and the
	 * same instruction would fault again forever.
	 *
	 * This asks vme_prot, which is what the range PERMITS.  What the page
	 * tables currently GRANT can be less -- that is how copy-on-write is
	 * arranged -- so the two have to be consulted for different questions,
	 * and this one is "was the access legal at all".
	 */
	if ((e->vme_prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) == 0 ||
	    (write && (e->vme_prot & VM_PROT_WRITE) == 0)) {
		spin_unlock(&map->vm_lock);
		vm_n_fault_fail++;
		return (VM_FAULT_PROT);
	}

	/*
	 * Something is already mapped here, so this was a protection fault and
	 * not a missing page.  The legal access check above has already passed,
	 * which leaves exactly one way to get here: a store to a page whose
	 * write bit fork cleared.  Give the writer a page of its own.
	 *
	 * Note this happens BEFORE the VME_F_LAZY test below.  A lazily
	 * populated range is not the only kind that can be shared -- the
	 * loader populates a program's data segment eagerly, and that segment
	 * is precisely what a forked child writes to first.
	 */
	old = pmap_extract(pm, page);
	if (old != PA_INVALID) {
		if (!write || (e->vme_flags & VME_F_COW) == 0) {
			spin_unlock(&map->vm_lock);
			vm_n_fault_fail++;
			return (VM_FAULT_PROT);
		}
		/*
		 * Only a writable range is ever marked copy-on-write, and only
		 * an owned range is ever writable -- a borrowed one is a
		 * read-only window onto the kernel's own memory.  Stated here
		 * because the two paths below would otherwise quietly hand a
		 * kernel-image frame to pmm_free_page.
		 */
		KASSERT((e->vme_flags & VME_F_ANON) != 0,
		    "copy-on-write on a range whose frames are borrowed");
		prot = e->vme_prot;

		/*
		 * Nobody else holds this frame any more -- the other sharers
		 * have written their own copies, or simply died.  Copying it
		 * would produce a page identical to the one already here, so
		 * the whole fault is just a stale write bit to hand back.
		 */
		if (pmm_page_refs(old) == 1) {
			if (!pmap_enter(pm, page, old, prot)) {
				spin_unlock(&map->vm_lock);
				vm_n_fault_fail++;
				return (VM_FAULT_NOMEM);
			}
			vm_n_cow_steal++;
			spin_unlock(&map->vm_lock);
			return (VM_FAULT_OK);
		}

		/*
		 * Shared for real.  Allocate and copy with the lock down, the
		 * same discipline the pager path below follows and for a
		 * weaker reason -- this cannot sleep -- but the window has to
		 * be re-checked either way, because another thread of this
		 * same task can be faulting the very same page.
		 */
		spin_unlock(&map->vm_lock);
		pa = pmm_alloc_page();
		if (pa == PA_INVALID) {
			vm_n_fault_fail++;
			return (VM_FAULT_NOMEM);
		}
		{
			const uint64_t	*sk = (const uint64_t *)
			    pmm_kva_from_pa(old);
			uint64_t	*dk = (uint64_t *)pmm_kva_from_pa(pa);

			for (i = 0; i < VM_PAGE_SIZE / sizeof(uint64_t); i++)
				dk[i] = sk[i];
		}

		spin_lock(&map->vm_lock);
		e = vm_map_lookup(map, page);
		if (e == NULL || pmap_extract(pm, page) != old) {
			/*
			 * Somebody else resolved it while we copied, or the
			 * range went away.  Either way the copy is worthless
			 * and the instruction can simply be retried: it will
			 * either succeed against the winner's page or take a
			 * fresh fault against nothing.
			 */
			spin_unlock(&map->vm_lock);
			pmm_free_page(pa);
			vm_n_cow_race++;
			return (VM_FAULT_OK);
		}
		if (!pmap_enter(pm, page, pa, e->vme_prot)) {
			spin_unlock(&map->vm_lock);
			pmm_free_page(pa);
			vm_n_fault_fail++;
			return (VM_FAULT_NOMEM);
		}
		spin_unlock(&map->vm_lock);
		/*
		 * Last: this map no longer holds the original.  Dropped after
		 * the new leaf is installed, so there is no instant at which
		 * this task's page table points at a frame it has released.
		 */
		pmm_free_page(old);
		vm_n_cow_copy++;
		return (VM_FAULT_OK);
	}

	if ((e->vme_flags & VME_F_LAZY) == 0) {
		spin_unlock(&map->vm_lock);
		vm_n_fault_fail++;
		return (VM_FAULT_NOMAP);
	}
	obj = e->vme_object;
	off = e->vme_offset + (page - e->vme_start);
	vm_object_ref(obj);
	spin_unlock(&map->vm_lock);

	pa = pmm_alloc_page();
	if (pa == PA_INVALID) {
		vm_object_deref(obj);
		vm_n_fault_fail++;
		return (VM_FAULT_NOMEM);
	}
	kva = (uint8_t *)pmm_kva_from_pa(pa);
	for (i = 0; i < VM_PAGE_SIZE; i++)
		kva[i] = 0;

	/* The slow part, with nothing held.  Zeroes are already right for anon. */
	if (obj != NULL) {
		uint64_t	t0, dt;

		t0 = clock_uptime_us();
		if (vm_object_page(obj, off, kva) != 0) {
			vm_object_deref(obj);
			pmm_free_page(pa);
			vm_n_fault_fail++;
			return (VM_FAULT_IO);
		}
		dt = clock_uptime_us() - t0;
		vm_us_pager += dt;
		if (dt > vm_us_pager_max)
			vm_us_pager_max = dt;
	}

	spin_lock(&map->vm_lock);
	e = vm_map_lookup(map, page);
	if (e == NULL || (e->vme_flags & VME_F_LAZY) == 0) {
		/* Unmapped underneath us: the retry will fault and die. */
		spin_unlock(&map->vm_lock);
		vm_object_deref(obj);
		pmm_free_page(pa);
		vm_n_fault_fail++;
		return (VM_FAULT_NOMAP);
	}
	if (pmap_extract(pm, page) != PA_INVALID) {
		spin_unlock(&map->vm_lock);
		vm_object_deref(obj);
		pmm_free_page(pa);
		vm_n_fault_race++;
		return (VM_FAULT_OK);
	}
	prot = e->vme_prot;
	if (!pmap_enter(pm, page, pa, prot)) {
		spin_unlock(&map->vm_lock);
		vm_object_deref(obj);
		pmm_free_page(pa);
		vm_n_fault_fail++;
		return (VM_FAULT_NOMEM);
	}
	if (obj != NULL)
		vm_n_fault_file++;
	else
		vm_n_fault_zero++;
	spin_unlock(&map->vm_lock);
	vm_object_deref(obj);
	return (VM_FAULT_OK);
}

void
vm_fault_stats(void)
{

	kprintf("vm: %llu faults filled -- %llu zero, %llu from a file"
	    ", %llu raced, %llu refused\n",
	    (unsigned long long)(vm_n_fault_zero + vm_n_fault_file),
	    (unsigned long long)vm_n_fault_zero,
	    (unsigned long long)vm_n_fault_file,
	    (unsigned long long)vm_n_fault_race,
	    (unsigned long long)vm_n_fault_fail);
	if (vm_n_fault_file != 0)
		kprintf("vm: pager spent %llu us over %llu pages "
		    "(%llu us each, worst %llu)\n",
		    (unsigned long long)vm_us_pager,
		    (unsigned long long)vm_n_fault_file,
		    (unsigned long long)(vm_us_pager / vm_n_fault_file),
		    (unsigned long long)vm_us_pager_max);
	/*
	 * The two numbers that say whether copy-on-write is worth having.
	 * `shared` is what fork did not copy; `copied` is what a write made it
	 * pay for afterwards.  The gap between them is the win, and it is only
	 * a win because most pages a forked child inherits are never written
	 * -- a program that touched all of its memory would simply pay the
	 * same cost one fault at a time.
	 */
	kprintf("vm: fork shared %llu pages, %llu later copied on write, "
	    "%llu reclaimed by their last owner, %llu raced\n",
	    (unsigned long long)vm_n_fork_shared,
	    (unsigned long long)vm_n_cow_copy,
	    (unsigned long long)vm_n_cow_steal,
	    (unsigned long long)vm_n_cow_race);
}

/*
 * Caller holds vm_lock.  Returns true if [va, va+size) does not
 * overlap any existing entry, false otherwise.
 */
static bool
vm_range_free_locked(struct vm_map *map, uint64_t va, uint64_t size)
{
	struct vm_map_entry	*e;
	uint64_t		 end;

	end = va + size;
	for (e = map->vm_head; e != NULL; e = e->vme_next) {
		if (e->vme_start >= end)
			return (true);		/* entries past us  */
		if (e->vme_end <= va)
			continue;		/* entries before us */
		return (false);			/* overlap          */
	}
	return (true);
}

bool
vm_map_enter(struct vm_map *map, uint64_t va, uint64_t size,
    uint8_t prot, uint8_t flags)
{

	return (vm_map_enter_backed(map, va, size, prot, flags, NULL, 0));
}

bool
vm_map_enter_backed(struct vm_map *map, uint64_t va, uint64_t size,
    uint8_t prot, uint8_t flags, struct vm_object *obj, uint64_t offset)
{
	struct vm_map_entry	*ne, *cur, *prev;
	uint64_t		 end;

	if (map == NULL || size == 0)
		return (false);
	if ((va & VM_PAGE_MASK) != 0 || (size & VM_PAGE_MASK) != 0)
		return (false);

	end = va + size;
	if (end <= va)				/* wrap */
		return (false);
	if (va < map->vm_lo || end > map->vm_hi)
		return (false);

	ne = kmalloc(sizeof(*ne));
	if (ne == NULL)
		return (false);
	ne->vme_start  = va;
	ne->vme_end    = end;
	ne->vme_offset = offset;
	ne->vme_object = obj;
	ne->vme_prot   = prot;
	ne->vme_flags  = flags;
	ne->vme_pad    = 0;
	ne->vme_next   = NULL;

	spin_lock(&map->vm_lock);
	if (!vm_range_free_locked(map, va, size)) {
		spin_unlock(&map->vm_lock);
		kfree(ne);
		return (false);
	}

	/*
	 * Insert sorted by start.  prev == NULL means new head.
	 */
	prev = NULL;
	cur  = map->vm_head;
	while (cur != NULL && cur->vme_start < va) {
		prev = cur;
		cur  = cur->vme_next;
	}
	ne->vme_next = cur;
	if (prev == NULL)
		map->vm_head = ne;
	else
		prev->vme_next = ne;
	map->vm_count++;
	spin_unlock(&map->vm_lock);
	return (true);
}

/*
 * CUTTING AN ENTRY IN TWO
 *
 * An entry says one thing about a whole range: one protection, one set of
 * flags, one origin.  Any request that names part of a range therefore needs
 * the map to be able to describe the two parts separately, and the only way
 * to do that is with two entries where there was one.  Mach calls this
 * vm_map_clip_start/clip_end; without it every caller that named a sub-range
 * had to be refused, which is what three of them did.
 *
 * The delicate field is vme_offset.  An entry maps byte vme_offset of its
 * object at vme_start, so a half that begins further along the range begins
 * further into the object by exactly the same distance.  Getting that wrong
 * breaks nothing visible: the map stays well-formed, the pages still fault in
 * on demand, and they quietly hold the wrong part of the file.  It is the one
 * line here worth testing on purpose.
 *
 * Caller holds vm_lock, and `at` must be page-aligned and lie strictly inside
 * `e`.  Returns false only when there is no memory for the second entry, in
 * which case nothing has changed.
 */
static bool
vm_entry_split_locked(struct vm_map *map, struct vm_map_entry *e, uint64_t at)
{
	struct vm_map_entry	*ne;

	KASSERT(e->vme_start < at && at < e->vme_end,
	    "vm_entry_split_locked: the cut is not inside the entry");
	KASSERT((at & VM_PAGE_MASK) == 0,
	    "vm_entry_split_locked: the cut is not page-aligned");

	ne = kmalloc(sizeof(*ne));
	if (ne == NULL)
		return (false);

	ne->vme_start  = at;
	ne->vme_end    = e->vme_end;
	ne->vme_offset = e->vme_offset + (at - e->vme_start);
	ne->vme_object = e->vme_object;
	ne->vme_prot   = e->vme_prot;
	ne->vme_flags  = e->vme_flags;
	ne->vme_pad    = 0;

	/*
	 * Both halves name the object now, so both hold a reference on it:
	 * entry_free drops one per entry, and an object that gained a second
	 * namer without gaining a second reference would be freed under the
	 * half that outlived the other.
	 */
	vm_object_ref(ne->vme_object);

	ne->vme_next = e->vme_next;
	e->vme_next  = ne;
	e->vme_end   = at;
	map->vm_count++;
	vm_n_split++;
	return (true);
}

/*
 * Make `at` an entry boundary.  A no-op when it already is one, or when it
 * falls in a hole -- there is nothing there to cut.  Caller holds vm_lock.
 */
static bool
vm_map_clip_locked(struct vm_map *map, uint64_t at)
{
	struct vm_map_entry	*e;

	e = vm_map_lookup(map, at);
	if (e == NULL || e->vme_start == at)
		return (true);
	return (vm_entry_split_locked(map, e, at));
}

size_t
vm_map_remove(struct vm_map *map, uint64_t va, uint64_t size)
{
	struct vm_map_entry	**pp, *cur, *gone;
	uint64_t		 end;
	size_t			 n;

	if (map == NULL || size == 0)
		return (0);

	end = va + size;
	n   = 0;

	spin_lock(&map->vm_lock);
	pp = &map->vm_head;
	while (*pp != NULL) {
		cur = *pp;
		if (cur->vme_start >= end)
			break;
		if (cur->vme_start >= va && cur->vme_end <= end) {
			gone = cur;
			*pp = cur->vme_next;
			entry_free(gone);
			map->vm_count--;
			n++;
			continue;
		}
		pp = &cur->vme_next;
	}
	spin_unlock(&map->vm_lock);
	return (n);
}

/* Caller holds vm_lock.  First hole of `size` at or after `from`, or 0. */
static uint64_t
vm_hole_locked(struct vm_map *map, uint64_t size, uint64_t from)
{
	struct vm_map_entry	*e;
	uint64_t		 cur;

	cur = (from < map->vm_lo) ? map->vm_lo : from;
	for (e = map->vm_head; e != NULL; e = e->vme_next) {
		if (e->vme_end <= cur)
			continue;
		if (e->vme_start >= cur + size)
			break;			/* [cur, cur+size) fits here */
		cur = e->vme_end;		/* slide past the obstacle */
	}
	return ((cur + size > map->vm_hi) ? 0 : cur);
}

bool
vm_map_find_space(struct vm_map *map, uint64_t size, uint64_t *va_out)
{
	uint64_t	va;

	if (map == NULL || size == 0 || va_out == NULL)
		return (false);
	size = VM_ALIGN_UP(size);

	spin_lock(&map->vm_lock);
	va = vm_hole_locked(map, size, map->vm_hint);
	/*
	 * The hint only ever moves forward, so a program that maps and unmaps
	 * in a loop would walk it to the ceiling and then fail with almost the
	 * whole window free.  Nothing could do that before mmap existed; now
	 * that something can, a failed search starts over from the floor
	 * before giving up.
	 */
	if (va == 0 && map->vm_hint > map->vm_lo)
		va = vm_hole_locked(map, size, map->vm_lo);
	if (va == 0) {
		spin_unlock(&map->vm_lock);
		return (false);
	}
	*va_out = va;
	map->vm_hint = va + size;
	spin_unlock(&map->vm_lock);
	return (true);
}

struct vm_map_entry *
vm_map_lookup(struct vm_map *map, uint64_t va)
{
	struct vm_map_entry	*e;

	if (map == NULL)
		return (NULL);

	for (e = map->vm_head; e != NULL; e = e->vme_next) {
		if (e->vme_start > va)
			return (NULL);
		if (e->vme_end > va)
			return (e);
	}
	return (NULL);
}

void
vm_map_print(struct vm_map *map)
{
	struct vm_map_entry	*e;

	if (map == NULL) {
		kprintf("vm_map: NULL\n");
		return;
	}

	spin_lock(&map->vm_lock);
	kprintf("vm_map [%016lx .. %016lx) %zu entries, hint=%016lx\n",
	    (unsigned long)map->vm_lo, (unsigned long)map->vm_hi,
	    map->vm_count, (unsigned long)map->vm_hint);
	for (e = map->vm_head; e != NULL; e = e->vme_next)
		vm_print_entry(e);
	spin_unlock(&map->vm_lock);
}

size_t
vm_map_snapshot(struct vm_map *map, struct mach_vm_region_entry *out,
    size_t max_entries)
{
	struct vm_map_entry		*e;
	struct mach_vm_region_entry	*o;
	size_t				 i;
	size_t				 n;

	if (map == NULL || out == NULL || max_entries == 0)
		return (0);

	n = 0;
	spin_lock(&map->vm_lock);
	for (e = map->vm_head; e != NULL && n < max_entries; e = e->vme_next) {
		o = &out[n];
		o->mvr_start  = e->vme_start;
		o->mvr_end    = e->vme_end;
		o->mvr_offset = e->vme_offset;
		o->mvr_prot   = e->vme_prot;
		o->mvr_flags  = e->vme_flags;
		for (i = 0; i < sizeof(o->mvr_pad); i++)
			o->mvr_pad[i] = 0;
		n++;
	}
	spin_unlock(&map->vm_lock);
	return (n);
}

size_t
vm_map_region_count(struct vm_map *map)
{
	size_t	n;

	if (map == NULL)
		return (0);

	spin_lock(&map->vm_lock);
	n = map->vm_count;
	spin_unlock(&map->vm_lock);
	return (n);
}

static void
vm_print_entry(const struct vm_map_entry *e)
{
	char	prot[5];

	prot[0] = (e->vme_prot & 0x01) ? 'r' : '-';	/* VM_PROT_READ  */
	prot[1] = (e->vme_prot & 0x02) ? 'w' : '-';	/* VM_PROT_WRITE */
	prot[2] = (e->vme_prot & 0x04) ? 'x' : '-';	/* VM_PROT_EXEC  */
	prot[3] = (e->vme_prot & 0x08) ? 'u' : 's';	/* VM_PROT_USER  */
	prot[4] = '\0';

	kprintf("  %016lx-%016lx %s flags=%s%s%s size=%lu KiB%s%s\n",
	    (unsigned long)e->vme_start,
	    (unsigned long)e->vme_end,
	    prot,
	    (e->vme_flags & VME_F_ANON) ? "A" : "-",
	    (e->vme_flags & VME_F_COW)  ? "C" : "-",
	    (e->vme_flags & VME_F_LAZY) ? "L" : "-",
	    (unsigned long)((e->vme_end - e->vme_start) >> 10),
	    (e->vme_object != NULL) ? " <- " : "",
	    (e->vme_object != NULL) ? e->vme_object->vo_path : "");
}
