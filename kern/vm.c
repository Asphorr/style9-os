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

/* Fault counters.  Plain writes -- they are diagnostics, not state. */
static uint64_t	vm_n_fault_zero;	/* pages zero-filled            */
static uint64_t	vm_n_fault_file;	/* pages paged in from a file   */
static uint64_t	vm_n_fault_race;	/* someone else got there first */
static uint64_t	vm_n_fault_fail;	/* refused or could not fill    */

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
	 * entries own their backing frames (no sharing in v1), so each
	 * present leaf can be unmapped + freed unconditionally.
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
	struct vm_map_entry	*entry;
	uint64_t		 end;
	uint64_t		 pa;
	uint64_t		 v;

	if (map == NULL || pm == NULL || size == 0)
		return (false);
	if ((va & VM_PAGE_MASK) != 0 || (size & VM_PAGE_MASK) != 0)
		return (false);

	end = va + size;
	if (end < va)
		return (false);
	if (va < map->vm_lo || end > map->vm_hi)
		return (false);

	entry = vm_map_lookup(map, va);
	if (entry == NULL)
		return (false);
	/*
	 * Exactly one entry, exactly its extent.  A request for part of an
	 * entry used to be accepted here, and it freed the frames under that
	 * part while vm_map_remove -- which only drops entries lying wholly
	 * inside the range -- left the entry in place still claiming it.  The
	 * map would then promise memory that had been handed back to the
	 * allocator.  Splitting an entry is the real answer; until there is
	 * one, refuse.
	 */
	if (entry->vme_start != va || entry->vme_end != end)
		return (false);
	if ((entry->vme_flags & VME_F_ANON) == 0)
		return (false);

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
vm_map_fork_copy(struct vm_map *src, struct pmap *src_pm,
    struct vm_map *dst, struct pmap *dst_pm)
{
	struct vm_map_entry	*e;
	uint64_t		*dk;
	uint64_t		*sk;
	uint64_t		 npa;
	uint64_t		 pa;
	uint64_t		 va;
	size_t			 i;

	if (src == NULL || src_pm == NULL || dst == NULL || dst_pm == NULL)
		return (false);

	/*
	 * No lock on src: the parent's only thread is parked in the fork
	 * syscall, so no mutator can race the walk (the same invariant
	 * vm_map_release_anon documents).  dst belongs to a task that has
	 * not run yet.
	 *
	 * The copy is eager: every present leaf gets its own frame in the
	 * child.  Holes inside an entry (PA_INVALID) are preserved as
	 * holes, mirroring exactly what the parent had faulted in -- which
	 * for an eagerly loaded segment means "everything", and for a lazy
	 * mapping means only the pages the parent actually touched.  The
	 * child inherits the entry's pager along with its range, so a page
	 * neither of them has touched yet still knows where to come from.
	 */
	for (e = src->vm_head; e != NULL; e = e->vme_next) {
		vm_object_ref(e->vme_object);
		if (!vm_map_enter_backed(dst, e->vme_start,
		    e->vme_end - e->vme_start, e->vme_prot, e->vme_flags,
		    e->vme_object, e->vme_offset)) {
			vm_object_deref(e->vme_object);
			return (false);
		}
		for (va = e->vme_start; va < e->vme_end; va += VM_PAGE_SIZE) {
			pa = pmap_extract(src_pm, va);
			if (pa == PA_INVALID)
				continue;
			npa = pmm_alloc_page();
			if (npa == PA_INVALID)
				return (false);
			sk = (uint64_t *)pmm_kva_from_pa(pa);
			dk = (uint64_t *)pmm_kva_from_pa(npa);
			for (i = 0; i < VM_PAGE_SIZE / sizeof(uint64_t); i++)
				dk[i] = sk[i];
			if (!pmap_enter(dst_pm, va, npa, e->vme_prot)) {
				pmm_free_page(npa);
				return (false);
			}
		}
	}
	return (true);
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
	uint64_t		 pa;
	uint8_t			 prot;
	size_t			 i;

	if (map == NULL || pm == NULL)
		return (VM_FAULT_NOMAP);
	page = VM_ALIGN_DOWN(va);

	spin_lock(&map->vm_lock);
	e = vm_map_lookup(map, page);
	if (e == NULL || (e->vme_flags & VME_F_LAZY) == 0) {
		spin_unlock(&map->vm_lock);
		vm_n_fault_fail++;
		return (VM_FAULT_NOMAP);
	}
	/*
	 * A range mapped PROT_NONE is a reservation, not memory.  Filling it
	 * would install a leaf the faulting thread still cannot touch, and the
	 * same instruction would fault again forever.
	 */
	if ((e->vme_prot & (VM_PROT_READ | VM_PROT_WRITE | VM_PROT_EXEC)) == 0 ||
	    (write && (e->vme_prot & VM_PROT_WRITE) == 0)) {
		spin_unlock(&map->vm_lock);
		vm_n_fault_fail++;
		return (VM_FAULT_PROT);
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
	if (obj != NULL && vm_object_page(obj, off, kva) != 0) {
		vm_object_deref(obj);
		pmm_free_page(pa);
		vm_n_fault_fail++;
		return (VM_FAULT_IO);
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
