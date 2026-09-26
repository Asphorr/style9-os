/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_VM_OBJECT_H_
#define	_SYS_VM_OBJECT_H_

#include <stdint.h>

#include "fs.h"
#include "spinlock.h"

/*
 * Where a mapping's bytes come from: what a page should hold the first
 * time it is touched.  A range with no object is zero-fill; a range with
 * a file object gets the file's bytes at the matching offset, and zeroes
 * past end-of-file, as mmap(2) promises for the last page.
 *
 * No page list and no sharing: every frame a fault installs belongs to
 * the map entry that faulted it, so an object is a source of initial
 * content, not a page cache.  Two tasks mapping one file get their own
 * frames -- MAP_PRIVATE -- and the repeated reads are absorbed by the
 * block cache (fs/bio.c).  A writable shared mapping would need a page
 * list here.
 *
 * The object holds the file resolved (a struct fs_handle), not its path:
 * a pager that re-resolved the path on every fault spent 936 us per page.
 * The path is kept only for diagnostics.
 */

#define	VM_OBJ_PATH_MAX		256

/*
 * Lock key:
 *	(c) const after vm_object_file
 *	(f) owned by the filesystem, mutated only under its volume lock
 *	(o) protected by vo_lock
 */
struct vm_object {
	struct spinlock	 vo_lock;
	/*
	 * (f) the file, resolved.  Not (c): the filesystem refreshes its
	 * cached length in place when the volume changes, so fs_pread gets
	 * it mutable.  Only fs/ writes it, under the volume lock.
	 */
	struct fs_handle vo_handle;
	uint64_t	 vo_size;	/* (c) bytes of real content     */
	uint64_t	 vo_n_page;	/* (o) pages served so far       */
	uint32_t	 vo_refs;	/* (o) map entries naming it     */
	char		 vo_path[VM_OBJ_PATH_MAX];	/* (c) for humans */
};

/*
 * Create a file-backed object over a resolved file, keeping `path` for
 * diagnostics, and take a hold on the file (fs_hold) that the last
 * vm_object_deref gives back.  Returns it with one reference, for the
 * vm_map_entry the caller is about to create; NULL on failure.
 */
struct vm_object	*vm_object_file(const struct fs_handle *h,
			    const char *path);

void			 vm_object_ref(struct vm_object *);
void			 vm_object_deref(struct vm_object *);

/*
 * Fill the 4 KiB page of `obj` at byte offset `off` into `page`, a kernel
 * alias of the frame that must already be zeroed: whatever this does not
 * write (past end-of-file, a hole) stays zero, as mmap(2) and sparse
 * files require.
 *
 * Must not be called with a spinlock held: it reads the disk, which
 * sleeps (fs/bio.c).
 *
 * Returns 0 on success, -1 if the file could not be read.
 */
int			 vm_object_page(struct vm_object *obj, uint64_t off,
			    uint8_t *page);

#endif /* !_SYS_VM_OBJECT_H_ */
