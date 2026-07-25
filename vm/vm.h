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

struct vm_object;	/* kern/vm_object.h -- what the pages are made of */
struct task;

/*
 * What an entry says about its pages.  The three questions are separate and
 * this is the vocabulary for all of them:
 *
 *	VME_F_ANON  -- the frames under this range are owned, not borrowed.
 *	  Teardown drops the entry's reference to each of them, which returns
 *	  a frame to the allocator only if nobody else still holds one (see
 *	  vm/pmm.h).  True of every mapping here, including a private file
 *	  mapping: its frames hold that file's bytes but belong to the task,
 *	  which is what MAP_PRIVATE means.  What the initial content was is a
 *	  separate question, answered by vme_object (NULL = zeroes).
 *
 *	VME_F_COW   -- pages in this range may be shared with another map, and
 *	  the page-table entries under it have had their write bit cleared to
 *	  make sure a store cannot go unnoticed.  This is the only flag that
 *	  makes a *protection* fault fixable rather than fatal: vm_fault
 *	  responds by giving the writer a private copy, or, if it turns out to
 *	  be the last owner, simply by handing the write bit back.
 *
 *	  The flag says "may be shared", never "is shared".  It stays set on
 *	  ranges whose pages have all since been copied or whose sharers have
 *	  all died, and that is harmless: the count in pmm is the authority on
 *	  how many owners a given frame has, and the flag only decides whether
 *	  it is worth asking.
 *
 *	VME_F_LAZY  -- the range is promised but not populated.  There are no
 *	  page-table leaves under it yet; the first touch of each page takes
 *	  a fault, and vm_fault installs a frame then.  Without this flag an
 *	  entry is fully populated at creation and a missing leaf inside it
 *	  is a bug, not an invitation to page something in -- which is why
 *	  vm_fault refuses to fill an entry that does not carry it.
 */
#define	VME_F_ANON		0x01	/* anonymous (pmm) backing       */
#define	VME_F_COW		0x02	/* may be shared; copy on write  */
#define	VME_F_LAZY		0x04	/* populate on first touch       */

struct vm_map_entry {
	uint64_t		 vme_start;	/* (m) inclusive       */
	uint64_t		 vme_end;	/* (m) exclusive       */
	uint64_t		 vme_offset;	/* (m) into vme_object */
	struct vm_object	*vme_object;	/* (m) NULL until vm_object lands */
	uint8_t			 vme_prot;	/* (c) VM_PROT_*       */
	/*
	 * (m) VME_F_*.  Was const-after-create until fork learned to share:
	 * turning a range into a copy-on-write one is a change to an entry
	 * that already exists, in the PARENT's map, and it happens while the
	 * parent sits quiescent inside the fork syscall.
	 */
	uint8_t			 vme_flags;
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
 * vm_map_enter with a pager attached: pages of this range start life holding
 * the bytes of `obj` from `offset` onward (NULL `obj` means zero-fill, and is
 * exactly what vm_map_enter passes).  On success the entry takes over the
 * caller's reference on `obj`; on failure the caller still owns it.
 *
 * Only meaningful together with VME_F_LAZY -- an eagerly populated entry has
 * already been filled by whoever populated it, and nothing will ever consult
 * its object.
 */
bool			 vm_map_enter_backed(struct vm_map *,
			    uint64_t va, uint64_t size,
			    uint8_t prot, uint8_t flags,
			    struct vm_object *obj, uint64_t offset);

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

/*
 * O(1) live-region count for a map.  Reads vm_count under vm_lock so
 * the value is internally consistent with the snapshot walk.  Drives
 * the per-task region column in the "tasks" service / top(1).
 */
size_t			 vm_map_region_count(struct vm_map *map);

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
 * Strict semantics: (va, size) MUST name a single VME_F_ANON vm_map_entry
 * EXACTLY -- same start, same end.  Partial deallocate, ranges spanning
 * multiple entries, or ranges naming a non-anonymous entry all return false
 * with no side effect.  Used by SYS_VM_DEALLOCATE, by send_capture_ool's
 * deallocate-on-send hook, and by the Darwin personality's munmap.
 *
 * On success: each present pmap leaf is unmapped, the backing frame is
 * pmm_free_page'd, the matching vm_map_entry is dropped, and true is
 * returned.
 */
bool			 vm_map_release(struct vm_map *,
			    struct pmap *pm, uint64_t va, uint64_t size);

/*
 * Drop every entry in `map` but keep the map itself usable -- the
 * execve(2) middle step.  The caller must already have released the
 * anonymous backing frames (vm_map_release_anon); this only frees the
 * vme storage and rewinds the allocation hint.  Same single-thread
 * invariant as vm_map_destroy: the owning task is between images, its
 * only thread is the one running this teardown.
 */
void			 vm_map_reset(struct vm_map *map);

/*
 * Duplicate `src`'s address space into `dst` -- the fork(2) engine.  Every
 * entry is re-entered in dst's map with identical range, prot and flags, and
 * every present 4 KiB leaf in src_pm becomes a leaf at the same VA in dst_pm
 * pointing at the SAME frame, with one more owner recorded against it.
 *
 * Nothing is copied here.  A writable range has the write bit cleared in both
 * page tables -- the parent's as much as the child's, because "who writes
 * first" is not known and the one that does must be the one that faults --
 * and both entries are marked VME_F_COW so vm_fault knows the fault is a
 * request for a private copy rather than a violation.  A read-only range
 * needs none of that: it is shared outright and its frames simply have two
 * owners until one of the maps goes away.
 *
 * Returns false on allocation failure with dst partially populated -- the
 * caller derefs the child task, whose normal teardown (vm_map_release_anon +
 * vm_map_destroy) drops the references taken so far.  Caller guarantees src
 * is quiescent (its one thread is parked in the fork syscall), which is what
 * makes it safe both to walk src's entries unlocked and to change their flags.
 */
bool			 vm_map_fork_share(struct vm_map *src,
			    struct pmap *src_pm, struct vm_map *dst,
			    struct pmap *dst_pm);

/*
 * Outcomes of vm_map_image.  Two failures rather than one because the
 * loaders above distinguish them: out of memory is a machine that is full,
 * a mapping failure is an image asking for something impossible.
 */
#define	VM_IMAGE_OK		0
#define	VM_IMAGE_NOMEM		1
#define	VM_IMAGE_MAP		2

/*
 * Bring one segment of an image that is ALREADY RESIDENT IN THE KERNEL into
 * a task: `vmsize` bytes at `seg_va`, of which the first `filesize` come from
 * `bytes` and the rest are zero.  Both program loaders reduce to exactly this
 * -- kern/elf.c calls it with a PT_LOAD, kern/macho.c with an LC_SEGMENT_64 --
 * and they call the same function rather than each keeping their own copy,
 * because what it decides is subtle enough that two copies would drift.
 *
 * BORROWING INSTEAD OF COPYING
 *
 * These images are not files being read off a disk.  They are part of the
 * kernel image: already resident, already read-only, at a physical address
 * that can be computed.  Allocating a frame to hold a second copy of
 * something that will never change is work with no product -- and it is done
 * once PER TASK, which is how thirty tasks came to hold thirty private copies
 * of the same libSystem.
 *
 * So a page is not copied at all when all three of these hold.  Its
 * page-table entry points straight at the image's own frame and the task
 * reads the kernel's only copy:
 *
 *	read-only    -- a writable page must be private, or one task's store
 *	  would rewrite the image every other task is running.
 *
 *	wholly file-backed -- a page that runs past `filesize` has a zero-fill
 *	  tail, and the image's frame holds whatever the linker put next in
 *	  the kernel rather than zeroes.  Those pages stay private, which is
 *	  also what keeps anything past the end of an image out of sight.
 *
 *	page-aligned -- a frame can only be mapped whole, so the image's
 *	  physical address and the segment's virtual address must agree modulo
 *	  the page size.  The build aligns the embedded images to make this
 *	  true; if it ever stops, this quietly falls back to copying, which is
 *	  what the counters in vm_image_stats are for.
 *
 * The borrowed pages get their own vm_map_entry WITHOUT VME_F_ANON, so a
 * segment can end up as up to three entries.  That is not tidiness: teardown
 * reads that flag and frees every present frame under an entry carrying it,
 * and some of these frames are the kernel's own.
 */
int			 vm_map_image(struct vm_map *map, struct pmap *pm,
			    uint64_t seg_va, uint64_t vmsize,
			    uint64_t filesize, const void *bytes,
			    uint8_t prot);

/*
 * How the loaders have been paying for program pages: borrowed from the
 * kernel image, or allocated and filled.  Every borrowed page is one that
 * used to be a fresh frame plus a 4 KiB copy, once per task.
 */
void			 vm_image_stats(void);

/*
 * Outcomes of a fault.  Only VM_FAULT_OK means "resume the instruction";
 * every other value means the faulting thread should be retired the way an
 * unhandled fault always was.
 */
#define	VM_FAULT_OK		0
#define	VM_FAULT_NOMAP		1	/* no entry, or not a lazy one  */
#define	VM_FAULT_PROT		2	/* mapped, but not like that    */
#define	VM_FAULT_IO		3	/* the pager could not read it  */
#define	VM_FAULT_NOMEM		4	/* out of frames                */

/*
 * Resolve the fault on the page containing `va` in `map`/`pm`.  Called from
 * the #PF handler, and it answers two different faults:
 *
 *	nothing mapped -- turn a VME_F_LAZY promise into memory.  An
 *	  anonymous range gets a zeroed frame, a file-backed range gets a
 *	  frame holding that file's bytes.
 *
 *	mapped, but written to without the write bit -- on a VME_F_COW range
 *	  this is the copy-on-write fault: the writer gets a private copy of
 *	  the frame, or the original outright if it turns out to be the only
 *	  owner left.
 *
 * `write` is the fault's access type, so a store to a range that is not
 * writable at all is refused here rather than papered over with a writable
 * page.  vme_prot is what the range PERMITS; the page tables under it may
 * grant less while copy-on-write is pending, and that difference is exactly
 * what makes the second fault above possible.
 *
 * MUST be called with no locks held: filling a file-backed page reads the
 * disk, and that sleeps.
 */
int			 vm_fault(struct vm_map *map, struct pmap *pm,
			    uint64_t va, bool write);

/* Fault counters, for the shell and the boot banner. */
void			 vm_fault_stats(void);

#endif /* !_SYS_VM_H_ */
