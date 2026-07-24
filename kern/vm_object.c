/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs.h"
#include "kmem.h"
#include "vm_object.h"

/* See vm_object.h for what this is and why it has no page list. */

#define	VM_OBJ_PAGE_BYTES	4096u

static void
path_copy(char *dst, const char *src, size_t cap)
{
	size_t	i;

	for (i = 0; i + 1 < cap && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

struct vm_object *
vm_object_file(const char *path, uint64_t size)
{
	struct vm_object	*obj;

	if (path == NULL)
		return (NULL);

	obj = kmalloc(sizeof(*obj));
	if (obj == NULL)
		return (NULL);

	spin_init(&obj->vo_lock, "vm_object");
	obj->vo_size   = size;
	obj->vo_n_page = 0;
	obj->vo_refs   = 1;
	path_copy(obj->vo_path, path, sizeof(obj->vo_path));
	return (obj);
}

void
vm_object_ref(struct vm_object *obj)
{

	if (obj == NULL)
		return;
	spin_lock(&obj->vo_lock);
	obj->vo_refs++;
	spin_unlock(&obj->vo_lock);
}

void
vm_object_deref(struct vm_object *obj)
{
	bool	last;

	if (obj == NULL)
		return;
	spin_lock(&obj->vo_lock);
	last = (--obj->vo_refs == 0);
	spin_unlock(&obj->vo_lock);
	if (last)
		kfree(obj);
}

int
vm_object_page(struct vm_object *obj, uint64_t off, uint8_t *page)
{
	uint32_t	got;
	uint32_t	want;
	int		rv;

	if (obj == NULL || page == NULL)
		return (-1);

	/*
	 * Wholly past end-of-file.  Not an error: a mapping is allowed to be
	 * longer than the file it names, and those pages read as zero -- which
	 * the caller has already put there.
	 */
	if (off >= obj->vo_size)
		return (0);

	want = VM_OBJ_PAGE_BYTES;
	if (obj->vo_size - off < (uint64_t)want)
		want = (uint32_t)(obj->vo_size - off);

	got = 0;
	rv  = fs_pread(obj->vo_path, off, page, want, &got);
	if (rv != FS_E_OK || got != want)
		return (-1);

	spin_lock(&obj->vo_lock);
	obj->vo_n_page++;
	spin_unlock(&obj->vo_lock);
	return (0);
}
