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

	map = (struct vm_map *)kmalloc(sizeof(*map));
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
		kfree(e);
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
	if (entry->vme_start != va || entry->vme_end < end)
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

	ne = (struct vm_map_entry *)kmalloc(sizeof(*ne));
	if (ne == NULL)
		return (false);
	ne->vme_start  = va;
	ne->vme_end    = end;
	ne->vme_offset = 0;
	ne->vme_object = NULL;
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
			kfree(gone);
			map->vm_count--;
			n++;
			continue;
		}
		pp = &cur->vme_next;
	}
	spin_unlock(&map->vm_lock);
	return (n);
}

bool
vm_map_find_space(struct vm_map *map, uint64_t size, uint64_t *va_out)
{
	struct vm_map_entry	*e;
	uint64_t		 cur;

	if (map == NULL || size == 0 || va_out == NULL)
		return (false);
	size = VM_ALIGN_UP(size);

	spin_lock(&map->vm_lock);
	cur = map->vm_hint;
	if (cur < map->vm_lo)
		cur = map->vm_lo;

	for (e = map->vm_head; e != NULL; e = e->vme_next) {
		if (e->vme_end <= cur)
			continue;
		if (e->vme_start >= cur + size) {
			/* hole [cur, cur+size) fits before this entry */
			break;
		}
		/* slide past the obstacle */
		cur = e->vme_end;
	}

	if (cur + size > map->vm_hi) {
		spin_unlock(&map->vm_lock);
		return (false);
	}
	*va_out = cur;
	map->vm_hint = cur + size;
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

static void
vm_print_entry(const struct vm_map_entry *e)
{
	char	prot[5];

	prot[0] = (e->vme_prot & 0x01) ? 'r' : '-';	/* VM_PROT_READ  */
	prot[1] = (e->vme_prot & 0x02) ? 'w' : '-';	/* VM_PROT_WRITE */
	prot[2] = (e->vme_prot & 0x04) ? 'x' : '-';	/* VM_PROT_EXEC  */
	prot[3] = (e->vme_prot & 0x08) ? 'u' : 's';	/* VM_PROT_USER  */
	prot[4] = '\0';

	kprintf("  %016lx-%016lx %s flags=%s%s size=%lu KiB\n",
	    (unsigned long)e->vme_start,
	    (unsigned long)e->vme_end,
	    prot,
	    (e->vme_flags & VME_F_ANON) ? "A" : "-",
	    (e->vme_flags & VME_F_COW)  ? "C" : "-",
	    (unsigned long)((e->vme_end - e->vme_start) >> 10));
}
