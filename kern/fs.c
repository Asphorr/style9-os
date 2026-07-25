/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "fs.h"
#include "fs_apfs.h"
#include "fs_fat.h"

/*
 * Picking a backend.  See fs.h for what this is and is not.
 *
 * APFS goes first because recognising a container is a far more specific
 * claim than recognising a FAT BPB: a Fletcher-64 over a checkpoint
 * superblock does not pass by accident, whereas plausible-looking BPB fields
 * turn up in all sorts of blocks.  In practice only one of the two ever
 * mounts, since there is one disk.
 */

static void
name_copy(char *dst, const char *src, size_t cap)
{
	size_t	i;

	for (i = 0; i + 1 < cap && src[i] != '\0'; i++)
		dst[i] = src[i];
	dst[i] = '\0';
}

/*
 * Clear a statbuf before either backend fills it.  This is not tidiness: the
 * struct is copied out to userspace whole, so a field neither filesystem sets
 * would otherwise hand a Darwin binary whatever was on the kernel stack.
 */
static void
zero(void *p, size_t n)
{
	uint8_t	*b;
	size_t	 i;

	b = p;
	for (i = 0; i < n; i++)
		b[i] = 0;
}

/* Both readers number their errors privately; neither numbering escapes. */
static int
apfs_err(int rv)
{

	switch (rv) {
	case FS_APFS_E_OK:		return (FS_E_OK);
	case FS_APFS_E_NOMOUNT:		return (FS_E_NOMOUNT);
	case FS_APFS_E_NOTFOUND:	return (FS_E_NOTFOUND);
	case FS_APFS_E_NOMEM:		return (FS_E_NOMEM);
	case FS_APFS_E_TOOBIG:		return (FS_E_TOOBIG);
	default:			return (FS_E_IO);
	}
}

static int
fat_err(int rv)
{

	switch (rv) {
	case FS_FAT_E_OK:		return (FS_E_OK);
	case FS_FAT_E_NOMOUNT:		return (FS_E_NOMOUNT);
	case FS_FAT_E_NOTFOUND:		return (FS_E_NOTFOUND);
	case FS_FAT_E_NOMEM:		return (FS_E_NOMEM);
	case FS_FAT_E_TOOBIG:		return (FS_E_TOOBIG);
	default:			return (FS_E_IO);
	}
}

int
fs_ready(void)
{

	return (fs_apfs_ready() || fs_fat_ready());
}

const char *
fs_kind(void)
{

	if (fs_apfs_ready())
		return ("apfs");
	if (fs_fat_ready())
		return ("fat");
	return ("none");
}

int
fs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size)
{

	if (fs_apfs_ready())
		return (apfs_err(fs_apfs_slurp(path, out_buf, out_size)));
	if (fs_fat_ready())
		return (fat_err(fs_fat_slurp(path, out_buf, out_size)));
	return (FS_E_NOMOUNT);
}

int
fs_open(const char *path, struct fs_handle *out)
{
	uint64_t	id;
	uint64_t	size;
	int		rv;

	if (out == NULL)
		return (FS_E_NOTFOUND);
	out->fh_kind = FS_HANDLE_NONE;
	out->fh_id   = 0;
	out->fh_size = 0;

	if (fs_apfs_ready()) {
		rv = fs_apfs_open(path, &id, &size);
		if (rv != FS_APFS_E_OK)
			return (apfs_err(rv));
		out->fh_kind = FS_HANDLE_APFS;
	} else if (fs_fat_ready()) {
		rv = fs_fat_open(path, &id, &size);
		if (rv != FS_FAT_E_OK)
			return (fat_err(rv));
		out->fh_kind = FS_HANDLE_FAT;
	} else
		return (FS_E_NOMOUNT);

	out->fh_id   = id;
	out->fh_size = size;
	return (FS_E_OK);
}

int
fs_pread(const struct fs_handle *h, uint64_t off, uint8_t *buf, uint32_t len,
    uint32_t *out_got)
{

	if (h == NULL || out_got == NULL)
		return (FS_E_NOTFOUND);
	switch (h->fh_kind) {
	case FS_HANDLE_APFS:
		return (apfs_err(fs_apfs_pread(h->fh_id, h->fh_size, off, buf,
		    len, out_got)));
	case FS_HANDLE_FAT:
		return (fat_err(fs_fat_pread(h->fh_id, h->fh_size, off, buf,
		    len, out_got)));
	default:
		return (FS_E_NOMOUNT);
	}
}

int
fs_stat(const char *path, struct fs_statbuf *out)
{
	struct fs_apfs_statbuf	asb;
	struct fs_fat_statbuf	fsb;
	int			rv;

	zero(out, sizeof(*out));
	if (fs_apfs_ready()) {
		rv = fs_apfs_stat(path, &asb);
		if (rv != FS_APFS_E_OK)
			return (apfs_err(rv));
		out->fs_size     = asb.afs_size;
		out->fs_ino      = asb.afs_ino;
		out->fs_alloced  = asb.afs_alloced;
		out->fs_mtime_ns = asb.afs_mtime_ns;
		out->fs_atime_ns = asb.afs_atime_ns;
		out->fs_ctime_ns = asb.afs_ctime_ns;
		out->fs_btime_ns = asb.afs_btime_ns;
		out->fs_nlink    = asb.afs_nlink;
		out->fs_uid      = asb.afs_uid;
		out->fs_gid      = asb.afs_gid;
		out->fs_mode     = asb.afs_mode;
		out->fs_is_dir   = asb.afs_is_dir;
		return (FS_E_OK);
	}
	if (fs_fat_ready()) {
		rv = fs_fat_stat2(path, &fsb);
		if (rv != FS_FAT_E_OK)
			return (fat_err(rv));
		out->fs_size     = fsb.fs_size;
		out->fs_ino      = fsb.fs_ino;
		out->fs_alloced  = fsb.fs_alloced;
		out->fs_mtime_ns = fsb.fs_mtime_ns;
		out->fs_atime_ns = fsb.fs_atime_ns;
		/*
		 * FAT records no inode-change time; the write time is the
		 * closest true statement about when this entry last changed.
		 */
		out->fs_ctime_ns = fsb.fs_mtime_ns;
		out->fs_btime_ns = fsb.fs_btime_ns;
		out->fs_nlink    = 1;		/* FAT has no hard links */
		out->fs_uid      = 0;
		out->fs_gid      = 0;
		out->fs_mode     = fsb.fs_mode;
		out->fs_is_dir   = fsb.fs_is_dir;
		return (FS_E_OK);
	}
	return (FS_E_NOMOUNT);
}

int
fs_readdir(const char *path, uint32_t index, struct fs_dirent *out)
{
	struct fs_apfs_dirent	ade;
	struct fs_fat_dirent	fde;
	int			rv;

	if (fs_apfs_ready()) {
		rv = fs_apfs_readdir(path, index, &ade);
		if (rv != 1)
			return (rv < 0 ? apfs_err(rv) : 0);
		out->fde_ino    = ade.ade_ino;
		out->fde_size   = ade.ade_size;
		out->fde_is_dir = ade.ade_is_dir;
		name_copy(out->fde_name, ade.ade_name, sizeof(out->fde_name));
		return (1);
	}
	if (fs_fat_ready()) {
		rv = fs_fat_readdir(path, index, &fde);
		if (rv != 1)
			return (rv < 0 ? fat_err(rv) : 0);
		out->fde_ino    = fde.fde_ino;
		out->fde_size   = fde.fde_size;
		out->fde_is_dir = fde.fde_is_dir;
		name_copy(out->fde_name, fde.fde_name, sizeof(out->fde_name));
		return (1);
	}
	return (FS_E_NOMOUNT);
}
