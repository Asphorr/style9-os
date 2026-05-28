/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_VM_H_
#define	_SYS_VM_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "spinlock.h"

/*
 * Machine-independent VM bookkeeping: per-task sorted record of which
 * virtual ranges are live, what each is backing, and what protection
 * it carries.  Sits one layer above the machine-dependent pmap, which
 * stays authoritative for the actual hardware page tables.
 *
 * vm_map is the per-task source of truth for live virtual ranges.
 * vm_map_enter records a mapping the caller has already installed via
 * pmap_kenter (kernel master) or pmap_enter (per-task pmap); the OOL
 * recv path uses vm_map_find_space to place a fresh range without
 * stomping the receiver's existing mappings; vm_map_release_anon walks
 * VME_F_ANON entries at task teardown to drop the backing frames
 * before pmap_destroy frees the page-table tree itself.
 *
 * Design notes:
 *
 *	- Entries form a singly-linked list, head-sorted by vme_start,
 *	  with no overlaps.  Lookup is O(N) but N is tiny for the
 *	  workloads here (a few code/stack/heap entries per task); a
 *	  future BST swap-in is a local change.
 *
 *	- Adjacency (entry A's vme_end == entry B's vme_start) is
 *	  permitted; they just stay separate entries.  Coalescing is a
 *	  follow-up nicety.
 *
 *	- vme_object is a placeholder for future vm_object support;
 *	  it is currently always NULL and vme_flags tells the consumer
 *	  where the pages actually came from (anonymous frames pmm
 *	  allocated, or an objcopy-embedded blob for ELF .text/.rodata).
 *
 *	- Protections reuse the existing VM_PROT_* bits from
 *	  arch/amd64/pmap.h so call sites do not have to translate.
 */

struct vm_object;	/* defined when vm_object support lands */
struct task;

#define	VME_F_ANON		0x01	/* anonymous (pmm) backing       */
#define	VME_F_COW		0x02	/* future: copy-on-write share   */

struct vm_map_entry {
	uint64_t		 vme_start;	/* (m) inclusive       */
	uint64_t		 vme_end;	/* (m) exclusive       */
	uint64_t		 vme_offset;	/* (m) into vme_object */
	struct vm_object	*vme_object;	/* (m) NULL until vm_object lands */
	uint8_t			 vme_prot;	/* (c) VM_PROT_*       */
	uint8_t			 vme_flags;	/* (c) VME_F_*         */
	uint16_t		 vme_pad;
	struct vm_map_entry	*vme_next;	/* (m)                 */
};

/*
 * Per-task VM map.  Lock key:
 *	(m) protected by vm_lock
 *	(c) const after vm_map_create
 */
struct vm_map {
	struct spinlock		 vm_lock;
	uint64_t		 vm_lo;		/* (c) valid range floor   */
	uint64_t		 vm_hi;		/* (c) valid range ceiling */
	uint64_t		 vm_hint;	/* (m) next-search seed    */
	struct vm_map_entry	*vm_head;	/* (m) sorted list head    */
	size_t			 vm_count;	/* (m) live entry count    */
};

/*
 * Defaults for vm_map_create: the user-VA window the current loader +
 * usermode launcher hands out.  Kept in vm.h so future per-VM ranges
 * (e.g. heap, mmap pool) can be sliced out of the same constants.
 */
#define	VM_USER_VA_LO		0x40000000ULL
#define	VM_USER_VA_HI		0x80000000ULL

void			 vm_init(void);

struct vm_map		*vm_map_create(uint64_t lo, uint64_t hi);
void			 vm_map_destroy(struct vm_map *);

/*
 * Record a mapping in `map`.  Fails (returns false) if [va, va+size)
 * overlaps an existing entry or escapes [vm_lo, vm_hi).  Does NOT
 * touch pmap -- the caller is responsible for the hardware install
 * (pmap_kenter into the kernel master pmap, or pmap_enter into the
 * task's per-task pmap).
 */
bool			 vm_map_enter(struct vm_map *,
			    uint64_t va, uint64_t size,
			    uint8_t prot, uint8_t flags);

/*
 * Remove every entry that lies fully inside [va, va+size).  Partial
 * overlaps are not supported in this commit (a future split-at-edges
 * pass will handle protect/unmap of sub-ranges).  Returns the number
 * of entries removed.
 */
size_t			 vm_map_remove(struct vm_map *,
			    uint64_t va, uint64_t size);

/*
 * Find an unmapped hole of at least `size` bytes within [lo, hi),
 * page-aligned.  Writes the chosen start to *va_out and returns true.
 * Used by the OOL recv path to place an incoming range without
 * stomping the receiver's existing mappings.
 */
bool			 vm_map_find_space(struct vm_map *,
			    uint64_t size, uint64_t *va_out);

/*
 * Returns the entry covering `va`, or NULL if `va` falls in a hole.
 * Caller must hold vm_lock OR be confident no concurrent
 * vm_map_remove can run (e.g. during single-thread task teardown).
 */
struct vm_map_entry	*vm_map_lookup(struct vm_map *, uint64_t va);

void			 vm_map_print(struct vm_map *);

/*
 * Wire-format snapshot of one entry in a vm_map.  Returned in arrays
 * by SYS_TASK_GET_VM_REGIONS.  Mirrors struct vm_map_entry's externally
 * meaningful fields -- the head/list pointers + vm_object placeholder
 * stay kernel-private.  ABI-stable; future revisions may append new
 * trailing fields but never reorder.
 */
#define	MACH_VM_REGION_MAX		64

/* WIRE FORMAT.  ABI-stable. */
struct mach_vm_region_entry {
	uint64_t	mvr_start;	/* inclusive base VA              */
	uint64_t	mvr_end;	/* exclusive top VA               */
	uint64_t	mvr_offset;	/* into vm_object (0 today)       */
	uint8_t		mvr_prot;	/* VM_PROT_*                      */
	uint8_t		mvr_flags;	/* VME_F_*                        */
	uint8_t		mvr_pad[6];
};

/*
 * Snapshot every live entry in `map` into `out`, up to `max_entries`.
 * Best-effort: walks under vm_lock, copies field-by-field.  Returns
 * the number of entries written.  Drives the userspace `vmmap` tool
 * (Darwin's `vmmap(1)` analog -- shows a process's VM layout, again
 * a question Linux has no equivalent to without /proc/pid/maps).
 */
size_t			 vm_map_snapshot(struct vm_map *map,
			    struct mach_vm_region_entry *out,
			    size_t max_entries);

struct pmap;

/*
 * Walk every VME_F_ANON entry in `map`, pmap_extract each 4 KiB page
 * out of `pm`, drop the mapping, and pmm_free_page the frame.  Called
 * from task teardown after the per-task threads have stopped running
 * (so there are no concurrent vm_map mutators) and before pmap_destroy
 * tears down the page tables themselves.  No-op on the map's entries
 * past the walk -- vm_map_destroy still has to free vme storage.
 */
void			 vm_map_release_anon(struct vm_map *,
			    struct pmap *pm);

/*
 * Tear down a single anonymous range previously installed by some
 * combination of vm_map_find_space + per-page pmap_enter + vm_map_enter.
 *
 * Strict semantics in v1: (va, size) MUST mirror a single VME_F_ANON
 * vm_map_entry whose extent covers the request.  Partial deallocate,
 * ranges spanning multiple entries, or ranges naming a non-anonymous
 * entry all return false with no side effect.  Used both by
 * SYS_VM_DEALLOCATE and by send_capture_ool's deallocate-on-send hook.
 *
 * On success: each present pmap leaf is unmapped, the backing frame is
 * pmm_free_page'd, the matching vm_map_entry is dropped, and true is
 * returned.
 */
bool			 vm_map_release(struct vm_map *,
			    struct pmap *pm, uint64_t va, uint64_t size);

#endif /* !_SYS_VM_H_ */
