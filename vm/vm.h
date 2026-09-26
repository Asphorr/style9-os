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
 * Machine-independent VM: per task, the sorted record of which virtual
 * ranges are live, what backs them and with what protection.  The pmap
 * below it stays authoritative for the hardware page tables.
 *
 * vm_map_enter records a range, whose pages the caller installs with
 * pmap_enter; vm_map_find_space places a fresh range (OOL receive,
 * vm_allocate, mmap); vm_map_release_anon drops the owned frames at task
 * teardown, before pmap_destroy frees the page tables.
 *
 *	- Entries are a singly-linked list sorted by vme_start, with no
 *	  overlaps.  Lookup is O(N), and N is a handful per task.
 *
 *	- Adjacent entries are allowed and are not coalesced.
 *
 *	- vme_object is the pager for a lazy range: NULL for zero-fill, a
 *	  file object for a mapped file (vm/vm_object.h).
 *
 *	- Protections are the VM_PROT_* bits of arch/amd64/pmap.h.
 */

struct vm_object;	/* vm/vm_object.h -- what the pages are made of */
struct task;

/*
 * What an entry says about its pages; three separate questions.
 *
 *	VME_F_ANON  -- the frames under the range are owned, not borrowed.
 *	  Teardown drops the entry's reference on each, which frees a frame
 *	  only if no one else holds one (vm/pmm.h).  Set on every range but
 *	  borrowed image pages (vm_map_image), private file mappings
 *	  included: their frames hold the file's bytes but belong to the
 *	  task (MAP_PRIVATE).  The initial content is vme_object's business.
 *
 *	VME_F_COW   -- pages may be shared with another map, and their
 *	  page-table entries have the write bit cleared so no store goes
 *	  unnoticed.  The only flag that makes a protection fault fixable:
 *	  vm_fault gives the writer a private copy, or, if it is the last
 *	  owner, just the write bit back.  It means "may be shared" and can
 *	  stay set after every sharer is gone; the pmm count is the
 *	  authority, the flag only says whether to ask.
 *
 *	VME_F_LAZY  -- the range is promised but not populated; each page's
 *	  first touch faults and vm_fault installs a frame.  Without it an
 *	  entry is fully populated at creation, a missing leaf is a bug, and
 *	  vm_fault refuses to fill it.
 */
#define	VME_F_ANON		0x01	/* anonymous (pmm) backing       */
#define	VME_F_COW		0x02	/* may be shared; copy on write  */
#define	VME_F_LAZY		0x04	/* populate on first touch       */

struct vm_map_entry {
	uint64_t		 vme_start;	/* (m) inclusive       */
	uint64_t		 vme_end;	/* (m) exclusive       */
	uint64_t		 vme_offset;	/* (m) into vme_object */
	struct vm_object	*vme_object;	/* (m) pager; NULL: zeroes */
	uint8_t			 vme_prot;	/* (c) VM_PROT_*       */
	/*
	 * (m) VME_F_*.  Not (c): VME_F_COW is added to live entries, by fork
	 * in the quiescent parent and by vm_pages_capture_user under vm_lock.
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

/* The user-VA window a task's map is created with. */
#define	VM_USER_VA_LO		0x40000000ULL
#define	VM_USER_VA_HI		0x80000000ULL

void			 vm_init(void);

struct vm_map		*vm_map_create(uint64_t lo, uint64_t hi);
void			 vm_map_destroy(struct vm_map *);

/*
 * Record a mapping in `map`; false if [va, va+size) is unaligned,
 * overlaps an entry or leaves [vm_lo, vm_hi).  The pmap is not touched:
 * installing the pages is the caller's job.
 */
bool			 vm_map_enter(struct vm_map *,
			    uint64_t va, uint64_t size,
			    uint8_t prot, uint8_t flags);

/*
 * vm_map_enter with a pager: the range's pages start out holding `obj`'s
 * bytes from `offset` on (NULL `obj`, as vm_map_enter passes, means
 * zeroes).  On success the entry takes over the caller's reference on
 * `obj`; on failure the caller keeps it.  Only VME_F_LAZY entries ever
 * consult their object.
 */
bool			 vm_map_enter_backed(struct vm_map *,
			    uint64_t va, uint64_t size,
			    uint8_t prot, uint8_t flags,
			    struct vm_object *obj, uint64_t offset);

/*
 * Remove every entry lying wholly inside [va, va+size); one that only
 * overlaps is left alone.  Cutting the request's edges into entry
 * boundaries is the caller's job (vm_map_release does it).  The pmap is
 * not touched.  Returns the number of entries removed.
 */
size_t			 vm_map_remove(struct vm_map *,
			    uint64_t va, uint64_t size);

/*
 * Find a hole of `size` bytes (page-rounded) in [vm_lo, vm_hi), next-fit
 * from vm_hint and then once more from the floor.  Writes the start to
 * *va_out and returns true; nothing is reserved.
 */
bool			 vm_map_find_space(struct vm_map *,
			    uint64_t size, uint64_t *va_out);

/*
 * The entry covering `va`, or NULL in a hole.  Caller holds vm_lock, or
 * knows no one can change the map (single-threaded teardown).
 */
struct vm_map_entry	*vm_map_lookup(struct vm_map *, uint64_t va);

void			 vm_map_print(struct vm_map *);

/*
 * One vm_map entry as SYS_TASK_GET_VM_REGIONS returns it: the entry's
 * public fields, without its pointers.  ABI-stable: fields may be
 * appended, never reordered.
 */
#define	MACH_VM_REGION_MAX		64

/* WIRE FORMAT.  ABI-stable. */
struct mach_vm_region_entry {
	uint64_t	mvr_start;	/* inclusive base VA              */
	uint64_t	mvr_end;	/* exclusive top VA               */
	uint64_t	mvr_offset;	/* into vm_object                 */
	uint8_t		mvr_prot;	/* VM_PROT_*                      */
	uint8_t		mvr_flags;	/* VME_F_*                        */
	uint8_t		mvr_pad[6];
};

/*
 * Copy up to `max_entries` of `map`'s entries into `out`, under vm_lock,
 * and return how many.  Backs the userspace `vmmap` tool.
 */
size_t			 vm_map_snapshot(struct vm_map *map,
			    struct mach_vm_region_entry *out,
			    size_t max_entries);

/*
 * The map's entry count (vm_count, under vm_lock), for the "tasks"
 * service's region column.
 */
size_t			 vm_map_region_count(struct vm_map *map);

struct pmap;

/*
 * Unmap every present page of every VME_F_ANON entry and drop the
 * reference on its frame (pmm frees it with its last owner).  Called at
 * task teardown once the task's threads have stopped, before
 * pmap_destroy, and by execve before vm_map_reset; the entries stay.
 */
void			 vm_map_release_anon(struct vm_map *,
			    struct pmap *pm);

/*
 * Tear down an anonymous range: unmap each present page, drop the
 * reference on its frame, and remove the entries.  For
 * SYS_VM_DEALLOCATE, OOL deallocate-on-send and the Darwin munmap.
 * `va` and `size` must be page-aligned.
 *
 * The range need not match how the memory was obtained: entries
 * sticking out of it are cut at its edges first, as munmap(2) expects.
 * Refused, leaving the map as it was:
 *
 *	a hole -- some page of the range is not mapped.  POSIX munmap
 *	  would allow it; here naming memory you do not have is an error.
 *
 *	a non-anonymous entry -- its frames are borrowed from the kernel
 *	  image and shared by every task running that program.
 *
 * Also false if a cut runs out of memory.  Returns true on success.
 */
bool			 vm_map_release(struct vm_map *,
			    struct pmap *pm, uint64_t va, uint64_t size);

/*
 * Drop every entry but keep the map, for execve(2).  The caller has
 * already released the frames (vm_map_release_anon); this frees the
 * entries and rewinds the hint.  No lock: the task is between images and
 * its only thread is the one running this.
 */
void			 vm_map_reset(struct vm_map *map);

/*
 * Duplicate `src`'s address space into `dst`, for fork(2).  Each entry
 * is re-entered in dst with the same range, prot, pager and flags, and
 * each present leaf in src_pm is mapped at the same VA in dst_pm to the
 * same frame -- with one more owner recorded, unless the frame is a
 * borrowed image page.
 *
 * Nothing is copied.  A writable range has the write bit cleared in both
 * page tables, since either side may write first, and both entries get
 * VME_F_COW so vm_fault treats the fault as a request for a copy.  A
 * read-only range is simply shared.
 *
 * Returns false on allocation failure, with dst partly populated; the
 * caller drops the child task, whose teardown releases what was taken.
 * src must be quiescent (its one thread parked in fork): its entries are
 * walked unlocked and their flags changed.
 */
bool			 vm_map_fork_share(struct vm_map *src,
			    struct pmap *src_pm, struct vm_map *dst,
			    struct pmap *dst_pm);

/*
 * Payloads of pages carried between address spaces (Mach OOL data).  The
 * sender names a range of its memory and the receiver finds it mapped in
 * its own; mach/ moves only an array of physical addresses.
 *
 * While in flight the captured frames are owned by the message: whoever
 * holds the array either installs it in a receiver, handing ownership
 * over, or releases it.  Returns the page count, or 0 on failure with
 * nothing held.
 *
 * Capture from a user range shares a page rather than copying it when
 * the payload starts page-aligned, one anonymous entry covers all of it,
 * the page is already present, and it lies wholly inside the payload --
 * so a partial last page, which would expose the sender's bytes past its
 * buffer, is always copied.  Shared or copied, the receiver cannot tell.
 *
 * A shared page is write-protected in the sender too, and the sender's
 * range marked copy-on-write: the receiver gets the bytes as they were
 * at the send.
 */
size_t			 vm_pages_capture_user(struct vm_map *map,
			    struct pmap *pm, uint64_t addr, uint64_t size,
			    uint64_t *pa_out, size_t max_pages);

/*
 * The same for a payload from kernel memory (a rendered man page).
 * Always copies: a kernel address is in no task's map, and .rodata must
 * not become a task's writable page.
 */
size_t			 vm_pages_capture_kernel(const void *src,
			    uint64_t size, uint64_t *pa_out,
			    size_t max_pages);

/*
 * Map a captured payload into `map` at a fresh address, returned in
 * *va_out.  The entry is writable and copy-on-write, the page-table
 * entries read-only: a receiver that only reads goes on sharing, one
 * that writes faults and gets its own copy.
 *
 * On success the frames belong to the receiver and the caller must not
 * release them; on failure nothing is installed and the caller keeps them.
 */
bool			 vm_pages_install(struct vm_map *map, struct pmap *pm,
			    const uint64_t *pas, size_t npages,
			    uint64_t *va_out);

/* Give up a captured payload that will never be delivered. */
void			 vm_pages_release(const uint64_t *pas, size_t npages);

/* How many payload pages were shared with their sender, and how many copied. */
void			 vm_pages_stats(void);

/*
 * Outcomes of vm_map_image.  The loaders tell the failures apart: out of
 * memory, or an image asking for an impossible mapping.
 */
#define	VM_IMAGE_OK		0
#define	VM_IMAGE_NOMEM		1
#define	VM_IMAGE_MAP		2

/*
 * Map one segment of an image resident in the kernel into a task:
 * `vmsize` bytes at `seg_va`, the first `filesize` from `bytes`, the rest
 * zero.  Both loaders use it -- kern/elf.c for a PT_LOAD, kern/macho.c for
 * an LC_SEGMENT_64 -- so its subtle decisions live in one place.
 *
 * The images are part of the kernel image: resident, read-only, at a
 * computable physical address.  So instead of a private copy per task, a
 * page is borrowed -- its leaf points at the image's own frame -- when
 * all three hold:
 *
 *	read-only    -- a writable page must be private, or one task's store
 *	  would rewrite the image every task runs.
 *
 *	wholly file-backed -- a page reaching past `filesize` needs a zero
 *	  tail, but the image's frame holds whatever the linker put next.
 *
 *	page-aligned -- the image's physical address and `seg_va` must
 *	  agree modulo the page size.  The build aligns the images; if that
 *	  ever stops, this silently copies instead (see vm_image_stats).
 *
 * Borrowed pages get their own entry without VME_F_ANON, so a segment
 * can become three entries: teardown frees the frames under every
 * VME_F_ANON entry, and these frames are the kernel's.
 */
int			 vm_map_image(struct vm_map *map, struct pmap *pm,
			    uint64_t seg_va, uint64_t vmsize,
			    uint64_t filesize, const void *bytes,
			    uint8_t prot);

/*
 * Program pages so far: borrowed from the kernel image, or allocated and
 * filled.  Each borrowed page saved a frame and a 4 KiB copy.
 */
void			 vm_image_stats(void);

/*
 * vm_map_release counts: whole, needing a cut, refused, and entries
 * split.  Splits show the mechanism ran; releases needing a cut show it
 * changed an outcome.
 */
void			 vm_map_stats(void);

/*
 * Outcomes of a fault.  VM_FAULT_OK resumes the instruction; anything
 * else retires the thread as for an unhandled fault.
 */
#define	VM_FAULT_OK		0
#define	VM_FAULT_NOMAP		1	/* no entry, or not a lazy one  */
#define	VM_FAULT_PROT		2	/* mapped, but not like that    */
#define	VM_FAULT_IO		3	/* the pager could not read it  */
#define	VM_FAULT_NOMEM		4	/* out of frames                */

/*
 * Resolve a #PF on the page holding `va` in `map`/`pm`.  Two kinds:
 *
 *	nothing mapped -- fill a VME_F_LAZY range: a zeroed frame, or one
 *	  holding the file's bytes.
 *
 *	mapped, written without the write bit -- on a VME_F_COW range, the
 *	  copy-on-write fault: the writer gets a private copy, or the
 *	  original if it is the last owner.
 *
 * `write` is the access type, so a store to a range that is not writable
 * is refused.  vme_prot is what the range permits; the page tables may
 * grant less while copy-on-write is pending, which is what makes the
 * second kind possible.
 *
 * Must be called with no locks held: filling a file-backed page reads
 * the disk, which sleeps.
 */
int			 vm_fault(struct vm_map *map, struct pmap *pm,
			    uint64_t va, bool write);

/* Fault counters, for the shell and the boot banner. */
void			 vm_fault_stats(void);

#endif /* !_SYS_VM_H_ */
