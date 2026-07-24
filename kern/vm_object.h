/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_VM_OBJECT_H_
#define	_SYS_VM_OBJECT_H_

#include <stdint.h>

#include "spinlock.h"

/*
 * Where a mapping's bytes come from.
 *
 * vm_map answers "which virtual ranges are live".  This answers the other
 * half of the question -- what should be in a page the first time someone
 * touches it.  A range with no object is zero-fill, which is all anonymous
 * memory ever needs; a range with a file object gets that file's bytes at
 * the matching offset, and zeroes past the end of the file, which is
 * exactly what mmap(2) promises for the tail of the last page.
 *
 * There is deliberately no page list here and no sharing.  Every frame a
 * fault installs belongs to the single map entry that faulted it, so an
 * object is a *source of initial content*, not a cache of pages: two tasks
 * mapping the same file get their own frames with the same bytes in them.
 * That is MAP_PRIVATE, which is the only thing a read-only filesystem can
 * meaningfully offer anyway, and it keeps the whole layer clear of the
 * aliasing questions a shared page cache would raise.  The repeated reads
 * that a private-per-task rule implies are absorbed one level down, by the
 * block cache (kern/bio.c) -- which is the layer that should be absorbing
 * them.  When a writable shared mapping is wanted, this is the struct that
 * grows a page list.
 *
 * A file object names its file by path rather than by any handle, because
 * there are no vnodes here: kern/fs.h is a path-addressed interface, so a
 * path is the only durable name a file has.  The pager therefore re-walks
 * the directory tree once per page fault.  That is the honest cost of not
 * having a vnode layer, it is bounded (the walk hits the block cache), and
 * it is the first thing to fix if the pager ever shows up in a profile.
 */

#define	VM_OBJ_PATH_MAX		256

/*
 * Lock key:
 *	(c) const after vm_object_file
 *	(o) protected by vo_lock
 */
struct vm_object {
	struct spinlock	 vo_lock;
	uint64_t	 vo_size;	/* (c) bytes of real content     */
	uint64_t	 vo_n_page;	/* (o) pages served so far       */
	uint32_t	 vo_refs;	/* (o) map entries naming it     */
	char		 vo_path[VM_OBJ_PATH_MAX];	/* (c)           */
};

/*
 * Create a file-backed object naming `path`, whose content is `size` bytes
 * long.  Returns it with one reference, which the caller hands to the
 * vm_map_entry it is about to create.  NULL on allocation failure.
 */
struct vm_object	*vm_object_file(const char *path, uint64_t size);

void			 vm_object_ref(struct vm_object *);
void			 vm_object_deref(struct vm_object *);

/*
 * Fill one 4 KiB page of `obj` at byte offset `off` into `page`, which is a
 * kernel-VA alias of the frame and must already be zeroed -- everything this
 * does not write (the tail past end-of-file, a hole) stays zero, which is
 * what both mmap(2) and a sparse file require.
 *
 * MUST NOT be called with any spinlock held.  It reads the disk, the disk
 * read sleeps waiting for the drive's interrupt, and a thread that blocks
 * while holding a spinlock in this kernel is never woken again (the preempt
 * count is global; see kern/bio.c for the full account of that rule).
 *
 * Returns 0 on success, -1 if the file could not be read.
 */
int			 vm_object_page(struct vm_object *obj, uint64_t off,
			    uint8_t *page);

#endif /* !_SYS_VM_OBJECT_H_ */
