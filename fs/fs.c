/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "apfs.h"
#include "clock.h"
#include "fat.h"
#include "fs.h"
#include "kmem.h"
#include "kprintf.h"
#include "mutex.h"
#include "sched.h"
#include "task.h"
#include "thread.h"

/*
 * One lock for the volume, here rather than in either backend: every
 * caller outside fs/ comes through the functions below, so it covers both
 * backends and lets each stay straight-line code.  Two writers can target
 * the same block, and a reader could see a 4 KiB block (eight sectors)
 * mid-write.
 *
 * A mutex, not a spinlock: every call here reaches the disk, and that
 * sleeps (kern/mutex.h).  Held for a whole operation, a slurp of megabytes
 * included -- correctness over concurrency while there is one disk.
 */
static struct mutex	fs_lock = MUTEX_INIT("fs");

/*
 * Volume generation: bumped under fs_lock whenever metadata changes, so a
 * handle can tell whether the length it copied is still the file's.
 * Starts at 1 so a zeroed handle never matches it.
 */
static uint64_t		fs_gen = 1;

/*
 * fs_n_stale: handles found older than the volume (the check fired).
 * fs_n_resize: those whose length had really moved (it corrected one).
 */
static uint64_t		fs_n_stale;
static uint64_t		fs_n_resize;

/*
 * What is open.  A file lives until its last name and its last descriptor
 * are both gone, so unlink must know whether anything holds the file.  Not
 * a vnode layer: a hold count per object id, which every handle carries.
 * Descriptors onto one file share a row; a file nothing holds has none.
 * Reads and writes never consult it -- only unlink ("is anyone holding
 * this?") and close ("not any more").
 *
 * DARWIN_NOFILE is 16 per task, so 32 rows hold two tasks' entirely
 * distinct sets; shared files take one row each.
 */
#define	FS_OPEN_MAX	32

struct fs_open {
	uint64_t	fo_ino;
	uint32_t	fo_refs;
	/*
	 * Its last name is gone and it waits in the volume's private
	 * directory.  Kept here so close need not descend the tree to ask.
	 */
	bool		fo_nameless;
};

static struct fs_open	fs_opens[FS_OPEN_MAX];	/* (fs_lock) */
static uint64_t		fs_n_orphan;	/* (fs_lock) unlinked open */
static uint64_t		fs_n_reap;	/* (fs_lock) ...and let go */

/* The row for this file, or NULL.  Called with fs_lock held. */
static struct fs_open *
open_row(uint64_t ino)
{
	size_t	i;

	for (i = 0; i < FS_OPEN_MAX; i++)
		if (fs_opens[i].fo_refs != 0 && fs_opens[i].fo_ino == ino)
			return (&fs_opens[i]);
	return (NULL);
}

/*
 * One more holder of this file.  Called with fs_lock held.  A full table
 * is refused (FS_E_NOSPACE), never ignored: an untracked handle is a file
 * unlink could take from under its reader.
 */
static int
open_hold(uint64_t ino)
{
	struct fs_open	*fo;
	size_t		 i;

	fo = open_row(ino);
	if (fo != NULL) {
		fo->fo_refs++;
		return (FS_E_OK);
	}
	for (i = 0; i < FS_OPEN_MAX; i++) {
		if (fs_opens[i].fo_refs != 0)
			continue;
		fs_opens[i].fo_ino      = ino;
		fs_opens[i].fo_refs     = 1;
		fs_opens[i].fo_nameless = false;
		return (FS_E_OK);
	}
	kprintf("fs: %u files are already open and inode %llu would be one "
	    "more -- refused, because a file this kernel is not counting is a "
	    "file unlink would take out from under its own reader\n",
	    (unsigned)FS_OPEN_MAX, (unsigned long long)ino);
	return (FS_E_NOSPACE);
}

/*
 * Bring a handle's cached length up to date if the volume moved on since
 * it was made.  Called with fs_lock held, before a length is clamped
 * against.  FAT cannot be written, so only APFS lengths are re-read.
 */
static void
handle_refresh(struct fs_handle *h)
{
	uint64_t	size;

	if (h->fh_xid != 0)
		return;			/* a checkpoint does not change */
	if (h->fh_gen == fs_gen)
		return;
	fs_n_stale++;
	if (h->fh_kind == FS_HANDLE_APFS &&
	    fs_apfs_size(h->fh_ino, &size) == FS_APFS_E_OK) {
		if (size != h->fh_size)
			fs_n_resize++;
		h->fh_size = size;
	}
	/*
	 * Adopted even when nothing changed or the lookup failed, or the
	 * handle would re-walk the tree on every read after any unrelated
	 * write.
	 */
	h->fh_gen = fs_gen;
}

/*
 * Picking a backend (see fs.h): APFS is asked first, because a checksummed
 * container superblock does not match by accident, while plausible BPB
 * fields turn up anywhere.  With one disk, only one ever mounts.
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
 * Clear a statbuf before a backend fills it: it is copied out to userspace
 * whole, and an unset field would leak kernel stack.
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
	case FS_APFS_E_NOALLOC:		return (FS_E_NOALLOC);
	case FS_APFS_E_EXIST:		return (FS_E_EXIST);
	case FS_APFS_E_ISDIR:		return (FS_E_ISDIR);
	case FS_APFS_E_NOTDIR:		return (FS_E_NOTDIR);
	case FS_APFS_E_NOTEMPTY:	return (FS_E_NOTEMPTY);
	case FS_APFS_E_SPREAD:		return (FS_E_SPREAD);
	case FS_APFS_E_INVAL:		return (FS_E_INVAL);
	case FS_APFS_E_GONE:		return (FS_E_GONE);
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
fs_space(uint32_t *bsize, uint64_t *blocks, uint64_t *bfree)
{

	*bsize  = 0;
	*blocks = 0;
	*bfree  = 0;
	if (!fs_apfs_ready())
		return (FS_E_NOMOUNT);
	*bsize = APFS_BLOCK_SIZE;
	fs_apfs_space(blocks, bfree);
	return (FS_E_OK);
}

/*
 * The published past by name (/.xid, see fs.h): how a path is told from a
 * live one, and how a checkpoint is entered and left around one backend
 * call.  A path is live (not under /.xid), the checkpoint directory
 * itself, a checkpoint (with the path under it, "/" if nothing follows the
 * number), or a non-number under /.xid, which names nothing.
 */
#define	FS_PATH_LIVE	0
#define	FS_PATH_VIEWS	1
#define	FS_PATH_VIEW	2
#define	FS_PATH_NOVIEW	3

static int
view_parse(const char *path, uint64_t *xid_out, const char **rest_out)
{
	const char	*p;
	uint64_t	 x;

	p = path;
	while (*p == '/')
		p++;
	if (p[0] != '.' || p[1] != 'x' || p[2] != 'i' || p[3] != 'd')
		return (FS_PATH_LIVE);
	p += 4;
	if (*p != '\0' && *p != '/')
		return (FS_PATH_LIVE);		/* ".xidfoo" is a live name */
	while (*p == '/')
		p++;
	if (*p == '\0')
		return (FS_PATH_VIEWS);
	if (*p < '0' || *p > '9')
		return (FS_PATH_NOVIEW);
	x = 0;
	while (*p >= '0' && *p <= '9') {
		if (x > (~(uint64_t)0 - 9) / 10)
			return (FS_PATH_NOVIEW);	/* no ring holds that */
		x = x * 10 + (uint64_t)(*p - '0');
		p++;
	}
	if (*p != '\0' && *p != '/')
		return (FS_PATH_NOVIEW);
	*xid_out  = x;
	*rest_out = (*p == '\0') ? "/" : p;
	return (FS_PATH_VIEW);
}

int
fs_readonly(const char *path)
{
	const char	*rest;
	uint64_t	 xid;

	return (view_parse(path, &xid, &rest) != FS_PATH_LIVE);
}

/*
 * Point the backend's readers at checkpoint `xid'.  Called with fs_lock
 * held; the caller calls fs_apfs_view_leave before releasing it.  Only
 * APFS has a past.
 */
static int
view_enter(uint64_t xid, struct fs_apfs_view *v)
{
	int	rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_NOTFOUND : FS_E_NOMOUNT);
	rv = apfs_err(fs_apfs_view_open(xid, v));
	if (rv != FS_E_OK)
		return (rv);
	return (apfs_err(fs_apfs_view_enter(v)));
}

/* The decimal name a checkpoint is listed under. */
static void
view_name(char *dst, size_t cap, uint64_t xid)
{
	char	tmp[24];
	size_t	n;
	size_t	i;

	n = 0;
	do {
		tmp[n++] = (char)('0' + xid % 10);
		xid /= 10;
	} while (xid != 0 && n < sizeof(tmp));
	for (i = 0; i < n && i + 1 < cap; i++)
		dst[i] = tmp[n - 1 - i];
	dst[i] = '\0';
}

/*
 * When the checkpoint happens.  Mutations batch: an edit completes in
 * memory and on fresh blocks -- readable through the writer's own view,
 * reachable from no superblock yet -- and the checkpoint that publishes it
 * is owed.  Three things collect the debt:
 *
 *	- the free queue running low, asked after every mutation
 *	  (fs_apfs_ckpt_due).  The one physical bound: every block a
 *	  batch releases is an entry in a single node;
 *	- the syncer, a kernel thread that publishes a dirty volume every
 *	  FS_SYNC_MS, bounding the gap between return and platter;
 *	- fs_sync, for a caller who means now: fsync(2), and the boot path
 *	  after the self-tests.
 *
 * A crash loses the open transaction and nothing else: every edit in the
 * batch vanishes together, back to the last published checkpoint (the
 * torn-write stand, tools/hosttorn.c, checks exactly that).  write(2)
 * promises order; fsync(2) promises the platter.
 */
#define	FS_SYNC_MS	2000

static uint64_t		fs_n_owed;	/* (fs_lock) mutations that batched  */
static uint64_t		fs_n_room;	/* ...checkpoints the queue forced   */
static uint64_t		fs_n_timer;	/* checkpoints the syncer wrote      */

/*
 * A mutation just succeeded; decide about the checkpoint.  Called with
 * fs_lock held.  The edit stands whatever happens here; a checkpoint
 * failure is returned and the volume stays dirty for the syncer to retry.
 */
static int
ckpt_policy(void)
{

	fs_n_owed++;
	if (!fs_apfs_ckpt_due())
		return (FS_E_OK);
	fs_n_room++;
	return (apfs_err(fs_apfs_checkpoint()));
}

/*
 * Each operation is a _locked body plus a wrapper that takes fs_lock, so
 * no early return can leak the lock.
 */

static int
slurp_locked(const char *path, uint8_t **out_buf, uint32_t *out_size)
{
	struct fs_apfs_view	 v;
	const char		*rest;
	uint64_t		 xid;
	int			 kind;
	int			 rv;

	kind = view_parse(path, &xid, &rest);
	if (kind != FS_PATH_LIVE) {
		if (kind != FS_PATH_VIEW)
			return (FS_E_NOTFOUND);	/* a directory, or nothing */
		rv = view_enter(xid, &v);
		if (rv != FS_E_OK)
			return (rv);
		rv = apfs_err(fs_apfs_slurp(rest, out_buf, out_size));
		fs_apfs_view_leave();
		return (rv);
	}
	if (fs_apfs_ready())
		return (apfs_err(fs_apfs_slurp(path, out_buf, out_size)));
	if (fs_fat_ready())
		return (fat_err(fs_fat_slurp(path, out_buf, out_size)));
	return (FS_E_NOMOUNT);
}

int
fs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = slurp_locked(path, out_buf, out_size);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
open_locked(const char *path, struct fs_handle *out)
{
	struct fs_apfs_view	 v;
	const char		*rest;
	uint64_t		 id;
	uint64_t		 size;
	uint64_t		 ino;
	uint64_t		 xid;
	int			 kind;
	int			 rv;

	if (out == NULL)
		return (FS_E_NOTFOUND);
	out->fh_kind = FS_HANDLE_NONE;
	out->fh_id   = 0;
	out->fh_size = 0;
	out->fh_ino  = 0;
	out->fh_gen  = fs_gen;
	out->fh_xid  = 0;

	kind = view_parse(path, &xid, &rest);
	if (kind != FS_PATH_LIVE) {
		if (kind != FS_PATH_VIEW)
			return (FS_E_NOTFOUND);
		rv = view_enter(xid, &v);
		if (rv != FS_E_OK)
			return (rv);
		rv = apfs_err(fs_apfs_open(rest, &id, &size, &ino));
		fs_apfs_view_leave();
		if (rv != FS_E_OK)
			return (rv);
		/*
		 * Not counted: nothing under a checkpoint can be unlinked.
		 * The free queue's retention keeps the bytes, and every read
		 * asks it again.
		 */
		out->fh_kind = FS_HANDLE_APFS;
		out->fh_ino  = ino;
		out->fh_xid  = xid;
		out->fh_id   = id;
		out->fh_size = size;
		return (FS_E_OK);
	}

	if (fs_apfs_ready()) {
		rv = fs_apfs_open(path, &id, &size, &ino);
		if (rv != FS_APFS_E_OK)
			return (apfs_err(rv));
		/*
		 * Counted before the handle is filled in, so a refusal leaves
		 * it empty rather than usable and uncounted.
		 */
		rv = open_hold(ino);
		if (rv != FS_E_OK)
			return (rv);
		out->fh_kind = FS_HANDLE_APFS;
		out->fh_ino  = ino;
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
fs_open(const char *path, struct fs_handle *out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = open_locked(path, out);
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_hold(const struct fs_handle *h)
{
	int	rv;

	if (h == NULL || h->fh_kind != FS_HANDLE_APFS || h->fh_xid != 0)
		return (FS_E_OK);
	mutex_lock(&fs_lock);
	rv = open_hold(h->fh_ino);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * Drop a hold.  When the last holder of a file whose name is gone lets go,
 * the file is reaped here, in the filesystem, so that close, dup2, task
 * teardown and exec need not each know about the private directory.
 */
int
fs_close(struct fs_handle *h)
{
	struct fs_open	*fo;
	uint64_t	 ino;
	uint64_t	 now_ns;
	int		 rv;

	if (h == NULL || h->fh_kind != FS_HANDLE_APFS)
		return (FS_E_OK);
	if (h->fh_xid != 0) {
		/* Nothing counted it (see open_locked), so nothing is owed. */
		h->fh_kind = FS_HANDLE_NONE;
		h->fh_ino  = 0;
		h->fh_id   = 0;
		h->fh_size = 0;
		h->fh_xid  = 0;
		return (FS_E_OK);
	}
	ino = h->fh_ino;
	rv  = FS_E_OK;

	mutex_lock(&fs_lock);
	fo = open_row(ino);
	if (fo == NULL) {
		/*
		 * A hold was lost somewhere, which lets unlink take bytes from
		 * under a live reader.  Not repairable here; reported.
		 */
		kprintf("fs: closing inode %llu, which was not on the open "
		    "list -- a reference was lost somewhere\n",
		    (unsigned long long)ino);
		mutex_unlock(&fs_lock);
		return (FS_E_NOTFOUND);
	}
	fo->fo_refs--;
	if (fo->fo_refs == 0 && fo->fo_nameless) {
		now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
		rv = apfs_err(fs_apfs_reap(ino, now_ns));
		if (rv == FS_E_OK)
			rv = ckpt_policy();
		fs_gen++;
		fs_n_reap++;
		if (rv != FS_E_OK)
			kprintf("fs: inode %llu was the last hold on a file "
			    "with no name and it will not go (%d) -- it stays "
			    "in the private directory for the next mount\n",
			    (unsigned long long)ino, rv);
	}
	if (fo->fo_refs == 0)
		fo->fo_nameless = false;
	mutex_unlock(&fs_lock);

	h->fh_kind = FS_HANDLE_NONE;
	h->fh_ino  = 0;
	h->fh_id   = 0;
	h->fh_size = 0;
	return (rv);
}

static int
pread_locked(struct fs_handle *h, uint64_t off, uint8_t *buf,
    uint32_t len, uint32_t *out_got)
{
	struct fs_apfs_view	v;
	int			rv;

	if (h == NULL || out_got == NULL)
		return (FS_E_NOTFOUND);
	handle_refresh(h);
	switch (h->fh_kind) {
	case FS_HANDLE_APFS:
		if (h->fh_xid != 0) {
			/*
			 * Resolved again for every read: a handle that fell
			 * off the window gets FS_E_GONE.
			 */
			rv = view_enter(h->fh_xid, &v);
			if (rv != FS_E_OK)
				return (rv);
			rv = apfs_err(fs_apfs_pread(h->fh_id, h->fh_size, off,
			    buf, len, out_got));
			fs_apfs_view_leave();
			return (rv);
		}
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
fs_pread(struct fs_handle *h, uint64_t off, uint8_t *buf, uint32_t len,
    uint32_t *out_got)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = pread_locked(h, off, buf, len, out_got);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
pwrite_locked(struct fs_handle *h, uint64_t off, const uint8_t *buf,
    uint32_t len, uint32_t *out_put)
{
	uint64_t	now_ns;
	int		rv;

	if (h == NULL || out_put == NULL)
		return (FS_E_NOTFOUND);
	if (h->fh_kind != FS_HANDLE_APFS)
		return (FS_E_ROFS);	/* FAT reads here; it does not write */
	if (h->fh_xid != 0)
		return (FS_E_ROFS);	/* and so does the past */

	/* Refresh first: the checks below are against the current length. */
	handle_refresh(h);
	if (off == FS_OFF_APPEND)
		off = h->fh_size;

	/*
	 * A write running past the end grows the file first, so a failure to
	 * grow is a write that did not happen rather than half of one.  One
	 * starting beyond the end leaves a gap that reads as zeros, written
	 * out: there are no sparse files.
	 */
	if (off + (uint64_t)len > h->fh_size) {
		rv = apfs_err(fs_apfs_grow_for_write(h->fh_ino, h->fh_id,
		    off + (uint64_t)len, off));
		if (rv != FS_E_OK)
			return (rv);
		h->fh_size = off + (uint64_t)len;
		fs_gen++;
		h->fh_gen = fs_gen;
	}

	rv = apfs_err(fs_apfs_pwrite(h->fh_id, h->fh_size, off, buf, len,
	    out_put));
	if (rv != FS_E_OK)
		return (rv);

	/*
	 * The bytes are down; stamp the file.  A failed stamp is reported even
	 * though the write succeeded, rather than leave a wrong mtime behind a
	 * success.  The clock is in microseconds, so the stamp is too.
	 */
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_touch(h->fh_ino, now_ns));

	/*
	 * The stamp copies the inode's node and everything up to the
	 * superblock onto fresh blocks, published by a later checkpoint; the
	 * policy decides when (see ckpt_policy).
	 */
	if (rv == FS_E_OK)
		rv = ckpt_policy();

	/*
	 * Metadata moved, so every handle is suspect; this one's generation
	 * moves with it.  Bumped even if the stamp failed: the bytes went
	 * down, and over-reporting change is harmless.
	 */
	fs_gen++;
	h->fh_gen = fs_gen;
	return (rv);
}

int
fs_pwrite(struct fs_handle *h, uint64_t off, const uint8_t *buf,
    uint32_t len, uint32_t *out_put)
{
	int	rv;

	/*
	 * One acquisition covers the bytes and the timestamp, so no reader
	 * sees new contents with the old mtime.
	 */
	mutex_lock(&fs_lock);
	rv = pwrite_locked(h, off, buf, len, out_put);
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_length(struct fs_handle *h, uint64_t *out_len)
{

	if (h == NULL || out_len == NULL)
		return (FS_E_NOTFOUND);
	mutex_lock(&fs_lock);
	handle_refresh(h);
	*out_len = h->fh_size;
	mutex_unlock(&fs_lock);
	return (FS_E_OK);
}

static int
truncate_locked(struct fs_handle *h, uint64_t new_size)
{
	uint64_t	now_ns;
	int		rv;

	if (h == NULL)
		return (FS_E_NOTFOUND);
	if (h->fh_kind != FS_HANDLE_APFS || h->fh_xid != 0)
		return (FS_E_ROFS);

	/*
	 * Against the current length: whether this shortens or lengthens
	 * depends on what is actually there.
	 */
	handle_refresh(h);
	if (new_size == h->fh_size)
		return (FS_E_OK);
	if (new_size > h->fh_size)
		rv = apfs_err(fs_apfs_grow(h->fh_ino, h->fh_id, new_size));
	else
		rv = apfs_err(fs_apfs_truncate(h->fh_ino, h->fh_id, new_size));
	if (rv != FS_E_OK)
		return (rv);
	h->fh_size = new_size;

	/* Stamped and offered to the policy for the same reasons a write is. */
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_touch(h->fh_ino, now_ns));
	if (rv == FS_E_OK)
		rv = ckpt_policy();
	fs_gen++;
	h->fh_gen = fs_gen;
	return (rv);
}

int
fs_truncate(struct fs_handle *h, uint64_t new_size)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = truncate_locked(h, new_size);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * Split a path at its last separator into the parent directory and the
 * last component.  Parsing lives here, not in a backend: the APFS writer
 * takes an object id and one name, which is what its records are keyed on.
 *
 * A trailing separator is refused: "/tmp/x/" is a claim about a directory.
 * The directory calls strip it first (path_undress).
 */
static int
path_split(const char *path, char *dir, size_t dircap, const char **leaf)
{
	size_t	i;
	size_t	cut;

	cut = 0;
	for (i = 0; path[i] != '\0'; i++)
		if (path[i] == '/')
			cut = i + 1;
	if (i == 0 || path[i - 1] == '/')
		return (FS_E_NOTFOUND);
	if (cut >= dircap)
		return (FS_E_NOTFOUND);
	for (i = 0; i + 1 < cut; i++)
		dir[i] = path[i];
	dir[i] = '\0';			/* "" and "/" both name the root */
	*leaf = path + cut;
	return (FS_E_OK);
}

static int
create_locked(const char *path, uint16_t perm, uint64_t *ino_out)
{
	char		 dir[FS_NAME_MAX];
	const char	*leaf;
	uint64_t	 parent;
	uint64_t	 now_ns;
	int		 is_dir;
	int		 rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	if (fs_readonly(path))
		return (FS_E_ROFS);
	rv = path_split(path, dir, sizeof(dir), &leaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_lookup(dir, &parent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTFOUND);

	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_create(parent, leaf, now_ns, perm, ino_out));
	if (rv != FS_E_OK)
		return (rv);
	/*
	 * A crash before the checkpoint takes back the whole create, name
	 * and inode, never leaving a name without its inode.
	 */
	rv = ckpt_policy();
	fs_gen++;
	return (rv);
}

int
fs_create(const char *path, uint16_t perm, uint64_t *ino_out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = create_locked(path, perm, ino_out);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
unlink_locked(const char *path)
{
	struct fs_open	*fo;
	char		 dir[FS_NAME_MAX];
	const char	*leaf;
	uint64_t	 parent;
	uint64_t	 child;
	uint64_t	 now_ns;
	int		 is_dir;
	int		 rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	if (fs_readonly(path))
		return (FS_E_ROFS);
	rv = path_split(path, dir, sizeof(dir), &leaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_lookup(dir, &parent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTFOUND);

	/*
	 * Which file this is, to ask whether anything holds it open.  If so,
	 * the name goes and the file stays (orphaned) until the last close.
	 */
	child = 0;
	rv = apfs_err(fs_apfs_lookup(path, &child, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	fo = is_dir ? NULL : open_row(child);

	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	if (fo != NULL) {
		rv = apfs_err(fs_apfs_orphan(parent, leaf, now_ns, &child));
		if (rv == FS_E_OK) {
			fo->fo_nameless = true;
			fs_n_orphan++;
		}
	} else
		rv = apfs_err(fs_apfs_unlink(parent, leaf, now_ns));
	if (rv != FS_E_OK)
		return (rv);
	rv = ckpt_policy();
	fs_gen++;
	return (rv);
}

/*
 * Reap what an earlier boot left in the private directory: files whose
 * last close never came (fs_apfs_reap_all).  Run once from kmain after the
 * mount and before anything opens a file, so the open list need not be
 * consulted.
 */
int
fs_reap_orphans(void)
{
	uint64_t	now_ns;
	uint32_t	n;
	int		rv;

	if (!fs_apfs_ready())
		return (FS_E_OK);
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	n = 0;
	mutex_lock(&fs_lock);
	rv = apfs_err(fs_apfs_reap_all(now_ns, &n));
	if (rv == FS_E_OK && n != 0) {
		/*
		 * Deferred like any mutation; reaping is idempotent, so a crash
		 * before the publish just reaps them again next boot.
		 */
		rv = ckpt_policy();
		fs_n_reap += n;
		fs_gen++;
	}
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_unlink(const char *path)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = unlink_locked(path);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * Take the trailing separators off a path, keeping "/" itself.  Only the
 * directory calls do this: "mkdir /tmp/x/" can be honoured as written,
 * while "create /tmp/x/" cannot, and path_split refuses it.
 */
static int
path_undress(const char *path, char *out, size_t cap)
{
	size_t	n;

	for (n = 0; path[n] != '\0'; n++) {
		if (n + 1 >= cap)
			return (FS_E_NOTFOUND);
		out[n] = path[n];
	}
	while (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = '\0';
	return (FS_E_OK);
}

/*
 * Make or remove a directory: find the parent, then hand the backend its
 * object id and one component.  The two differ only in the final call.
 */
static int
dir_locked(const char *path, int make, uint16_t perm, uint64_t *ino_out)
{
	char		 norm[FS_NAME_MAX];
	char		 dir[FS_NAME_MAX];
	const char	*leaf;
	uint64_t	 parent;
	uint64_t	 now_ns;
	int		 is_dir;
	int		 rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	if (fs_readonly(path))
		return (FS_E_ROFS);
	rv = path_undress(path, norm, sizeof(norm));
	if (rv != FS_E_OK)
		return (rv);
	rv = path_split(norm, dir, sizeof(dir), &leaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_lookup(dir, &parent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTDIR);

	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = make ? fs_apfs_mkdir(parent, leaf, now_ns, perm, ino_out) :
	    fs_apfs_rmdir(parent, leaf, now_ns);
	rv = apfs_err(rv);
	if (rv != FS_E_OK)
		return (rv);
	rv = ckpt_policy();
	fs_gen++;
	return (rv);
}

int
fs_mkdir(const char *path, uint16_t perm, uint64_t *ino_out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = dir_locked(path, 1, perm, ino_out);
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_rmdir(const char *path)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = dir_locked(path, 0, 0, NULL);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * Move a name: two path splits and one edit.  POSIX wants rename atomic --
 * programs rename a temporary file over the real one so a reader sees one
 * or the other -- and the backend keeps that by changing every leaf it
 * touches in memory before writing.  This layer must not split it into
 * an unlink and a create.
 *
 * A taken destination is taken over in the same edit, and the occupant
 * comes back as an object id for this layer, which knows the descriptors,
 * to dispose of as unlink would: held open, it is marked nameless for the
 * last close; held by nothing, it is reaped now, before the checkpoint, so
 * both reach the platter as one state.  If the reap fails the file waits
 * in the private directory for the next mount; the rename stands.
 *
 * A trailing separator is refused on both paths, as for a create.
 */
static int
rename_locked(const char *opath, const char *npath)
{
	struct fs_open	*fo;
	char		 odir[FS_NAME_MAX];
	char		 ndir[FS_NAME_MAX];
	const char	*oleaf;
	const char	*nleaf;
	uint64_t	 oparent;
	uint64_t	 nparent;
	uint64_t	 victim;
	uint64_t	 now_ns;
	int		 is_dir;
	int		 rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	if (fs_readonly(opath) || fs_readonly(npath))
		return (FS_E_ROFS);
	rv = path_split(opath, odir, sizeof(odir), &oleaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = path_split(npath, ndir, sizeof(ndir), &nleaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_lookup(odir, &oparent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTDIR);
	rv = apfs_err(fs_apfs_lookup(ndir, &nparent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTDIR);

	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	victim = 0;
	rv = apfs_err(fs_apfs_rename(oparent, oleaf, nparent, nleaf, now_ns,
	    &victim));
	if (rv != FS_E_OK)
		return (rv);
	if (victim != 0) {
		fo = open_row(victim);
		if (fo != NULL) {
			fo->fo_nameless = true;
			fs_n_orphan++;
		} else {
			rv = apfs_err(fs_apfs_reap(victim, now_ns));
			if (rv == FS_E_OK)
				fs_n_reap++;
			else
				kprintf("fs: inode %llu lost its name to a "
				    "rename and will not go (%d) -- it stays "
				    "in the private directory for the next "
				    "mount\n", (unsigned long long)victim,
				    rv);
		}
	}
	rv = ckpt_policy();
	fs_gen++;
	return (rv);
}

int
fs_rename(const char *opath, const char *npath)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = rename_locked(opath, npath);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * fs_chmod: the permission bits of an existing name.  FAT is refused
 * (FS_E_ROFS): its entry has no mode word to set, and a chmod that
 * silently did nothing would look like success.
 */
int
fs_chmod(const char *path, uint16_t mode)
{
	struct fs_apfs_statbuf	asb;
	uint64_t		now_ns;
	int			rv;

	mutex_lock(&fs_lock);
	if (!fs_apfs_ready()) {
		mutex_unlock(&fs_lock);
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	}
	if (fs_readonly(path)) {
		mutex_unlock(&fs_lock);
		return (FS_E_ROFS);
	}
	rv = apfs_err(fs_apfs_stat(path, &asb));
	if (rv != FS_E_OK) {
		mutex_unlock(&fs_lock);
		return (rv);
	}
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_chmod(asb.afs_ino, mode, now_ns));
	if (rv == FS_E_OK) {
		rv = ckpt_policy();
		fs_gen++;
	}
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_utimes(const char *path, uint64_t atime_ns, uint64_t mtime_ns)
{
	struct fs_apfs_statbuf	asb;
	uint64_t		now_ns;
	int			rv;

	mutex_lock(&fs_lock);
	if (!fs_apfs_ready()) {
		mutex_unlock(&fs_lock);
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	}
	if (fs_readonly(path)) {
		mutex_unlock(&fs_lock);
		return (FS_E_ROFS);
	}
	rv = apfs_err(fs_apfs_stat(path, &asb));
	if (rv != FS_E_OK) {
		mutex_unlock(&fs_lock);
		return (rv);
	}
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_utimes(asb.afs_ino, atime_ns, mtime_ns, now_ns));
	if (rv == FS_E_OK) {
		rv = ckpt_policy();
		fs_gen++;
	}
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
stat_locked(const char *path, struct fs_statbuf *out)
{
	struct fs_apfs_statbuf	 asb;
	struct fs_fat_statbuf	 fsb;
	struct fs_apfs_view	 v;
	const char		*rest;
	uint64_t		 xid;
	int			 kind;
	int			 rv;

	zero(out, sizeof(*out));
	kind = view_parse(path, &xid, &rest);
	if (kind == FS_PATH_NOVIEW)
		return (FS_E_NOTFOUND);
	if (kind == FS_PATH_VIEWS && !fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_NOTFOUND : FS_E_NOMOUNT);
	if (fs_apfs_ready()) {
		if (kind == FS_PATH_VIEWS) {
			/*
			 * /.xid reports the live root's times and owner under
			 * its own inode, with the write bits off.
			 */
			rv = fs_apfs_stat("/", &asb);
			if (rv != FS_APFS_E_OK)
				return (apfs_err(rv));
			asb.afs_ino  = FS_VIEW_INO(0);
			asb.afs_mode = (uint16_t)(FS_S_IFDIR | 0555);
		} else if (kind == FS_PATH_VIEW) {
			rv = view_enter(xid, &v);
			if (rv != FS_E_OK)
				return (rv);
			rv = fs_apfs_stat(rest, &asb);
			fs_apfs_view_leave();
			if (rv != FS_APFS_E_OK)
				return (apfs_err(rv));
			/* The checkpoint's root, under its listing entry's inode. */
			if (rest[0] == '/' && rest[1] == '\0')
				asb.afs_ino = FS_VIEW_INO(xid);
		} else {
			rv = fs_apfs_stat(path, &asb);
			if (rv != FS_APFS_E_OK)
				return (apfs_err(rv));
		}
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
		/* FAT records no inode-change time; use the write time. */
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
fs_stat(const char *path, struct fs_statbuf *out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = stat_locked(path, out);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * How many checkpoints the /.xid listing can name.  The window is
 * APFS_FQ_KEEP + 1 wide; a window larger than this is listed short, never
 * overrun.
 */
#define	FS_VIEW_LIST_MAX	16

static int
readdir_locked(const char *path, uint32_t index, struct fs_dirent *out)
{
	uint64_t		 xids[FS_VIEW_LIST_MAX];
	struct fs_apfs_dirent	 ade;
	struct fs_fat_dirent	 fde;
	struct fs_apfs_view	 v;
	const char		*rest;
	uint64_t		 xid;
	uint32_t		 n;
	int			 kind;
	int			 rv;

	kind = view_parse(path, &xid, &rest);
	if (kind == FS_PATH_NOVIEW)
		return (FS_E_NOTFOUND);
	if (kind != FS_PATH_LIVE && !fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_NOTFOUND : FS_E_NOMOUNT);
	if (kind == FS_PATH_VIEWS) {
		rv = apfs_err(fs_apfs_view_list(xids, FS_VIEW_LIST_MAX, &n));
		if (rv != FS_E_OK)
			return (rv);
		if (index >= n)
			return (0);
		out->fde_ino    = FS_VIEW_INO(xids[index]);
		out->fde_size   = 0;
		out->fde_is_dir = 1;
		view_name(out->fde_name, sizeof(out->fde_name), xids[index]);
		return (1);
	}
	if (kind == FS_PATH_VIEW) {
		rv = view_enter(xid, &v);
		if (rv != FS_E_OK)
			return (rv);
		rv = fs_apfs_readdir(rest, index, &ade);
		fs_apfs_view_leave();
		if (rv != 1)
			return (rv < 0 ? apfs_err(rv) : 0);
		out->fde_ino    = ade.ade_ino;
		out->fde_size   = ade.ade_size;
		out->fde_is_dir = ade.ade_is_dir;
		name_copy(out->fde_name, ade.ade_name, sizeof(out->fde_name));
		return (1);
	}
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

int
fs_readdir(const char *path, uint32_t index, struct fs_dirent *out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = readdir_locked(path, index, out);
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * Publish now: fsync(2) lands here, as does the boot path after the
 * self-tests.  A clean volume is a no-op; a checkpoint of nothing would
 * spend a ring slot.
 */
int
fs_sync(void)
{
	int	rv;

	mutex_lock(&fs_lock);
	if (fs_apfs_ready())
		rv = fs_apfs_dirty() ? apfs_err(fs_apfs_checkpoint()) :
		    FS_E_OK;
	else if (fs_fat_ready())
		rv = FS_E_OK;	/* FAT has no transaction to close */
	else
		rv = FS_E_NOMOUNT;
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * The syncer, the timer leg of the policy: sleep, and publish the volume
 * if it is dirty.  The dirty test reads without the lock (fs_sync takes
 * it to act).  A failed checkpoint is retried every tick, and reported
 * every time, while the volume stays dirty.
 */
static void
syncer_entry(void *arg)
{
	int	rv;

	(void)arg;
	for (;;) {
		sched_nap_ms(FS_SYNC_MS);
		if (!fs_apfs_ready() || !fs_apfs_dirty())
			continue;
		rv = fs_sync();
		if (rv != FS_E_OK)
			kprintf("fs-sync: the checkpoint would not write (%d) "
			    "-- the volume stays dirty and the next tick "
			    "tries again\n", rv);
		else
			fs_n_timer++;
	}
}

void
fs_syncer_start(void)
{
	struct thread	*th;

	if (!fs_apfs_ready())
		return;
	th = thread_create(kernel_task, syncer_entry, NULL, "syncer");
	if (th == NULL) {
		kprintf("fs-sync: no thread for the syncer -- only fsync and "
		    "the free queue will collect deferred checkpoints\n");
		return;
	}
	thread_start(th);
}

/*
 * Under fs_lock: the test writes checkpoints and checks the state they
 * move, which no reader may see half-done.
 */
void
fs_ckpt_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_ckpt_selftest();
	mutex_unlock(&fs_lock);
}

void
fs_handle_stats(void)
{

	/*
	 * The policy's tally: mutations batched, and which leg published
	 * them (fs_sync's own count is not kept).
	 */
	if (fs_n_owed != 0)
		kprintf("fs-sync: %llu mutation(s) batched, %llu checkpoint(s) "
		    "forced by the free queue's room, %llu written by the "
		    "syncer\n", (unsigned long long)fs_n_owed,
		    (unsigned long long)fs_n_room,
		    (unsigned long long)fs_n_timer);
	if (fs_n_stale == 0 && fs_n_orphan == 0)
		return;
	kprintf("fs: volume generation %llu -- %llu stale handle(s) refreshed, "
	    "%llu had actually changed length\n",
	    (unsigned long long)fs_gen, (unsigned long long)fs_n_stale,
	    (unsigned long long)fs_n_resize);
	/*
	 * Orphans and reaps.  They need not balance within one boot: an
	 * orphan left by a crashed boot is reaped by the next.
	 */
	if (fs_n_orphan != 0 || fs_n_reap != 0)
		kprintf("fs: %llu name(s) taken from files something still had "
		    "open, %llu of those files since let go\n",
		    (unsigned long long)fs_n_orphan,
		    (unsigned long long)fs_n_reap);
}

/* ---- write self-test ------------------------------------------------------ */

/*
 * The multi-extent file on the test image (4096 bytes in one extent, the
 * rest in another).  The probe straddles the block boundary, which is also
 * the extent boundary, so one 12-byte write must find two physical runs
 * and read-modify-write a partial block at each end.
 */
#define	SELFTEST_PATH	"/var/db/big.txt"
#define	SELFTEST_OFF	4090		/* 6 bytes before the boundary */
#define	SELFTEST_LEN	12		/* ...and 6 bytes past it      */
#define	SELFTEST_CTX	32		/* window read back around it  */
#define	SELFTEST_PAD	10		/* SELFTEST_OFF - window start */

/*
 * Left at offset 0 on purpose and looked for on the next boot: finding it
 * after a power cycle proves it reached the platter, which no cache can
 * fake.
 */
#define	SELFTEST_MARK	"style9 moved these bytes to write them.\n"

static int
same(const uint8_t *a, const uint8_t *b, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return (0);
	return (1);
}

static size_t
slen(const char *s)
{
	size_t	n;

	for (n = 0; s[n] != '\0'; n++)
		;
	return (n);
}

void
fs_write_selftest(void)
{
	struct fs_handle	h;
	/* Declared here so the single exit can close it: a handle is a hold. */
	struct fs_handle	stale;
	struct fs_statbuf	st0;
	struct fs_statbuf	st1;
	uint8_t			save[SELFTEST_CTX];
	uint8_t			back[SELFTEST_CTX];
	uint8_t			mark[sizeof(SELFTEST_MARK) - 1];
	uint8_t			pat[SELFTEST_LEN];
	const char		*marker = SELFTEST_MARK;
	uint32_t		got;
	uint32_t		put;
	size_t			i;
	int			rv;

	if (!fs_apfs_ready())
		return;			/* nothing here can be written */

	/* Empty before any jump to the exit, so closing it there is a no-op. */
	stale.fh_kind = FS_HANDLE_NONE;

	rv = fs_open(SELFTEST_PATH, &h);
	if (rv != FS_E_OK) {
		kprintf("apfs-write: %s absent (rv=%d) -- self-test skipped\n",
		    SELFTEST_PATH, rv);
		goto done;
	}
	if (h.fh_size < SELFTEST_OFF + SELFTEST_CTX) {
		kprintf("apfs-write: %s too small -- self-test skipped\n",
		    SELFTEST_PATH);
		goto done;
	}

	if (fs_stat(SELFTEST_PATH, &st0) != FS_E_OK) {
		kprintf("apfs-write: FAIL cannot stat before\n");
		goto done;
	}

	/* The window as it stands, so we can put it back and check neighbours. */
	rv = fs_pread(&h, SELFTEST_OFF - SELFTEST_PAD, save, SELFTEST_CTX, &got);
	if (rv != FS_E_OK || got != SELFTEST_CTX) {
		kprintf("apfs-write: FAIL pre-read (rv=%d got=%u)\n", rv, got);
		goto done;
	}

	for (i = 0; i < SELFTEST_LEN; i++)
		pat[i] = (uint8_t)('A' + i);

	rv = fs_pwrite(&h, SELFTEST_OFF, pat, SELFTEST_LEN, &put);
	if (rv != FS_E_OK || put != SELFTEST_LEN) {
		kprintf("apfs-write: FAIL write (rv=%d put=%u)\n", rv, put);
		goto done;
	}

	rv = fs_pread(&h, SELFTEST_OFF - SELFTEST_PAD, back, SELFTEST_CTX, &got);
	if (rv != FS_E_OK || got != SELFTEST_CTX) {
		kprintf("apfs-write: FAIL read-back (rv=%d got=%u)\n", rv, got);
		goto done;
	}
	if (!same(back + SELFTEST_PAD, pat, SELFTEST_LEN)) {
		kprintf("apfs-write: FAIL written bytes differ\n");
		goto done;
	}
	/*
	 * The neighbours: both ends of the write land in blocks mostly not
	 * ours, kept by the read-modify-write.
	 */
	if (!same(back, save, SELFTEST_PAD) ||
	    !same(back + SELFTEST_PAD + SELFTEST_LEN,
	    save + SELFTEST_PAD + SELFTEST_LEN,
	    SELFTEST_CTX - SELFTEST_PAD - SELFTEST_LEN)) {
		kprintf("apfs-write: FAIL neighbouring bytes clobbered\n");
		goto done;
	}

	/* Put it back, and prove the restore too. */
	rv = fs_pwrite(&h, SELFTEST_OFF, save + SELFTEST_PAD, SELFTEST_LEN,
	    &put);
	if (rv != FS_E_OK) {
		kprintf("apfs-write: FAIL restore (rv=%d)\n", rv);
		goto done;
	}
	rv = fs_pread(&h, SELFTEST_OFF - SELFTEST_PAD, back, SELFTEST_CTX, &got);
	if (rv != FS_E_OK || !same(back, save, SELFTEST_CTX)) {
		kprintf("apfs-write: FAIL restore did not restore\n");
		goto done;
	}

	if (fs_stat(SELFTEST_PATH, &st1) != FS_E_OK) {
		kprintf("apfs-write: FAIL cannot stat after\n");
		goto done;
	}
	if (st1.fs_mtime_ns <= st0.fs_mtime_ns) {
		kprintf("apfs-write: FAIL mtime did not move (%llu -> %llu)\n",
		    (unsigned long long)st0.fs_mtime_ns,
		    (unsigned long long)st1.fs_mtime_ns);
		goto done;
	}

	/*
	 * A second handle, opened before a write, carries a generation the
	 * write leaves behind; reading through it must notice (fs_n_stale
	 * moves).  The length does not change, so only the check is tested.
	 */
	{
		uint64_t		n0;
		uint8_t			one;

		n0 = fs_n_stale;
		if (fs_open(SELFTEST_PATH, &stale) != FS_E_OK) {
			kprintf("apfs-write: FAIL second open\n");
			goto done;
		}
		rv = fs_pwrite(&h, SELFTEST_OFF, save + SELFTEST_PAD,
		    SELFTEST_LEN, &put);
		if (rv != FS_E_OK) {
			kprintf("apfs-write: FAIL write before staleness "
			    "check (rv=%d)\n", rv);
			goto done;
		}
		if (fs_pread(&stale, 0, &one, 1, &got) != FS_E_OK) {
			kprintf("apfs-write: FAIL read through stale handle\n");
			goto done;
		}
		if (fs_n_stale == n0) {
			kprintf("apfs-write: FAIL a handle older than the "
			    "volume was not noticed\n");
			goto done;
		}
	}

	/* The marker, and what it says about a previous boot. */
	rv = fs_pread(&h, 0, mark, (uint32_t)sizeof(mark), &got);
	if (rv != FS_E_OK || got != sizeof(mark)) {
		kprintf("apfs-write: FAIL marker read (rv=%d)\n", rv);
		goto done;
	}
	if (same(mark, (const uint8_t *)marker, sizeof(mark))) {
		kprintf("apfs-write: PASS -- and the marker at %s:0 is still "
		    "there from an earlier boot\n", SELFTEST_PATH);
		goto done;
	}
	rv = fs_pwrite(&h, 0, (const uint8_t *)marker, (uint32_t)sizeof(mark),
	    &put);
	if (rv != FS_E_OK || put != sizeof(mark)) {
		kprintf("apfs-write: FAIL marker write (rv=%d put=%u)\n", rv,
		    put);
		goto done;
	}
	rv = fs_pread(&h, 0, mark, (uint32_t)sizeof(mark), &got);
	if (rv != FS_E_OK || !same(mark, (const uint8_t *)marker, sizeof(mark))) {
		kprintf("apfs-write: FAIL marker read-back\n");
		goto done;
	}
	kprintf("apfs-write: PASS -- marker written at %s:0; a reboot should "
	    "find it\n", SELFTEST_PATH);
done:
	/*
	 * Both handles back on every path: a leaked hold would make the next
	 * unlink of the file orphan it instead of freeing it.
	 */
	(void)fs_close(&stale);
	(void)fs_close(&h);
}

/*
 * A file gets longer and stays longer.  Appends a block at a time -- the
 * smallest append sure to need a new run -- until the file is at least
 * SELFTEST_GROW_TO bytes.  fs_trunc_selftest runs first and cuts the file
 * back to its shipped length, so this appends on every boot.
 */
#define	SELFTEST_GROW_TO	(155648u + 6u * 4096u)
#define	GROW_CHUNK		4096u

void
fs_grow_selftest(void)
{
	struct fs_handle	 h;
	struct fs_statbuf	 st;
	uint8_t			*chunk;
	uint8_t			*back;
	uint64_t		 was;
	uint64_t		 merges;
	uint64_t		 ckpts;
	uint64_t		 at;
	uint32_t		 rounds;
	uint32_t		 got;
	uint32_t		 put;
	uint32_t		 i;
	int			 rv;

	if (!fs_apfs_ready())
		return;
	if (fs_open(SELFTEST_PATH, &h) != FS_E_OK) {
		kprintf("apfs-grow: %s absent -- skipped\n", SELFTEST_PATH);
		return;
	}
	chunk = kmalloc(GROW_CHUNK);
	back  = kmalloc(GROW_CHUNK);
	if (chunk == NULL || back == NULL) {
		kprintf("apfs-grow: no memory -- skipped\n");
		goto out;
	}
	for (i = 0; i < GROW_CHUNK; i++)
		chunk[i] = (uint8_t)('a' + (i % 26));

	was    = h.fh_size;
	merges = fs_apfs_merges();
	ckpts  = fs_apfs_ckpts();
	rounds = 0;

	while (h.fh_size < SELFTEST_GROW_TO) {
		at = h.fh_size;
		rv = fs_pwrite(&h, at, chunk, GROW_CHUNK, &put);
		/*
		 * FS_E_SPREAD is a documented refusal, not a failure: the
		 * file's records are in more leaves than one edit can move,
		 * and nothing was changed.  Reported as a skip.
		 */
		if (rv == FS_E_SPREAD) {
			kprintf("apfs-grow: %s keeps its bytes and its inode "
			    "in different leaves -- appending across two is "
			    "not supported; skipped after %u round(s)\n",
			    SELFTEST_PATH, (unsigned)rounds);
			goto out;
		}
		if (rv != FS_E_OK || put != GROW_CHUNK) {
			kprintf("apfs-grow: FAIL round %u at %llu would not "
			    "grow (rv=%d put=%u)\n", (unsigned)rounds,
			    (unsigned long long)at, rv, (unsigned)put);
			goto out;
		}
		if (h.fh_size != at + put) {
			kprintf("apfs-grow: FAIL the handle says %llu after "
			    "writing %u at %llu\n",
			    (unsigned long long)h.fh_size, (unsigned)put,
			    (unsigned long long)at);
			goto out;
		}
		/*
		 * Read back through the file: the bytes have been through an
		 * allocation, a zeroing and a record insert since.
		 */
		rv = fs_pread(&h, at, back, put, &got);
		if (rv != FS_E_OK || got != put) {
			kprintf("apfs-grow: FAIL round %u cannot be read back "
			    "(rv=%d got=%u)\n", (unsigned)rounds, rv, got);
			goto out;
		}
		for (i = 0; i < put; i++) {
			if (back[i] == chunk[i])
				continue;
			kprintf("apfs-grow: FAIL round %u byte %u is 0x%02x, "
			    "wanted 0x%02x\n", (unsigned)rounds, (unsigned)i,
			    (unsigned)back[i], (unsigned)chunk[i]);
			goto out;
		}
		if (++rounds > 32) {
			kprintf("apfs-grow: FAIL %u rounds and still short of "
			    "%u bytes\n", (unsigned)rounds,
			    (unsigned)SELFTEST_GROW_TO);
			goto out;
		}
	}

	if (rounds == 0) {
		/*
		 * Already long enough, so the truncate test did not run.  A
		 * skip, not a pass.  That the length survives a reboot is
		 * checked by fs_trunc_selftest, against this test's tail.
		 */
		kprintf("apfs-grow: %s is already %llu bytes -- nothing to "
		    "append, skipped\n", SELFTEST_PATH,
		    (unsigned long long)h.fh_size);
		goto out;
	}

	if (fs_stat(SELFTEST_PATH, &st) != FS_E_OK) {
		kprintf("apfs-grow: FAIL cannot stat after growing\n");
		goto out;
	}
	if (st.fs_size != h.fh_size) {
		kprintf("apfs-grow: FAIL the volume says %llu bytes and the "
		    "handle says %llu -- the length did not reach the inode "
		    "record\n", (unsigned long long)st.fs_size,
		    (unsigned long long)h.fh_size);
		goto out;
	}

	/*
	 * Appends merge: the allocator hands back the blocks right after the
	 * file's last run, and touching runs are one run, so no new record
	 * per block.  (Splits are proved by fs_split_selftest.)
	 *
	 * All but the first: the truncate test just freed the blocks past
	 * this file's run, and the free queue holds them for the checkpoints
	 * that still name them, so the first append starts a new run.
	 */
	merges = fs_apfs_merges() - merges;
	ckpts  = fs_apfs_ckpts() - ckpts;
	if (merges + 1 < rounds) {
		/*
		 * Decidable only with no checkpoint in the loop: a checkpoint
		 * makes the free queue release blocks, and first-fit may take
		 * those holes over the block that would continue the run.
		 */
		if (ckpts != 0) {
			kprintf("apfs-grow: %u append(s) merged %llu run(s) "
			    "with %llu checkpoint(s) landing mid-loop -- "
			    "freed blocks came back as holes, so the merge "
			    "claim is not decidable this boot; skipped\n",
			    (unsigned)rounds, (unsigned long long)merges,
			    (unsigned long long)ckpts);
			goto out;
		}
		kprintf("apfs-grow: FAIL %u appends lengthened only %llu runs "
		    "-- the rest were given records of their own, and blocks "
		    "that touch should never need one\n", (unsigned)rounds,
		    (unsigned long long)merges);
		goto out;
	}

	kprintf("apfs-grow: PASS -- %s grew %llu -> %llu bytes over %u "
	    "appends, %llu of them lengthening the run already there rather "
	    "than adding a record, and a reboot should still find it\n",
	    SELFTEST_PATH, (unsigned long long)was,
	    (unsigned long long)h.fh_size, (unsigned)rounds,
	    (unsigned long long)merges);
out:
	/* The handle is a hold; every path releases it. */
	(void)fs_close(&h);
	kfree(chunk);
	kfree(back);
}

/*
 * A file gets shorter, and a record leaves a tree.  Cutting inside a run
 * shortens it in place; cutting a run away entirely takes its record out
 * of two B-trees.  Both are made to happen:
 *
 *	cut to the shipped length	-- undoes the growth test's appends
 *	append two blocks		-- one allocation, one run, one record
 *	cut away one of them		-- inside that run: a shortening
 *	append one block		-- cannot continue the run: the block
 *					   past it was just freed and the free
 *					   queue still holds it, so it gets a
 *					   record of its own
 *	cut it away			-- a whole run past the end: a drop
 *	cut back to the shipped length	-- leaves the file for the growth test
 *
 * On the way in it checks the tail the previous boot's growth test
 * appended: that test's persistence claim, checked while it still holds.
 */
#define	SELFTEST_TRUNC_TO	155648u		/* what big.txt ships at */

void
fs_trunc_selftest(void)
{
	struct fs_handle	 h;
	struct fs_statbuf	 st;
	uint8_t			*edge;
	uint8_t			*back;
	uint8_t			*chunk;
	uint64_t		 was;
	uint64_t		 shortens;
	uint64_t		 drops;
	uint32_t		 got;
	uint32_t		 put;
	uint32_t		 i;
	int			 rv;

	if (!fs_apfs_ready())
		return;
	if (fs_open(SELFTEST_PATH, &h) != FS_E_OK) {
		kprintf("apfs-trunc: %s absent -- skipped\n", SELFTEST_PATH);
		return;
	}
	edge  = kmalloc(SELFTEST_CTX);
	back  = kmalloc(2u * GROW_CHUNK);
	chunk = kmalloc(2u * GROW_CHUNK);
	if (edge == NULL || back == NULL || chunk == NULL) {
		kprintf("apfs-trunc: no memory -- skipped\n");
		goto out;
	}
	for (i = 0; i < 2u * GROW_CHUNK; i++)
		chunk[i] = (uint8_t)('a' + (i % 26));

	was = h.fh_size;
	if (was < SELFTEST_TRUNC_TO) {
		kprintf("apfs-trunc: %s is %llu bytes, shorter than the %u it "
		    "ships at -- skipped\n", SELFTEST_PATH,
		    (unsigned long long)was, (unsigned)SELFTEST_TRUNC_TO);
		goto out;
	}

	if (was > SELFTEST_TRUNC_TO) {
		/*
		 * Longer than shipped: the previous boot's growth test
		 * appended, and the last block must still read as appended.
		 */
		rv = fs_pread(&h, was - GROW_CHUNK, back, GROW_CHUNK, &got);
		if (rv != FS_E_OK || got != GROW_CHUNK) {
			kprintf("apfs-trunc: FAIL cannot read the tail of %s "
			    "(rv=%d got=%u)\n", SELFTEST_PATH, rv,
			    (unsigned)got);
			goto out;
		}
		for (i = 0; i < GROW_CHUNK; i++) {
			if (back[i] == chunk[i])
				continue;
			kprintf("apfs-trunc: FAIL byte %u of the tail is "
			    "0x%02x, wanted 0x%02x -- what an earlier boot "
			    "appended did not survive\n", (unsigned)i,
			    (unsigned)back[i], (unsigned)chunk[i]);
			goto out;
		}
	}

	/* The bytes just below where every cut will fall, to compare after. */
	rv = fs_pread(&h, SELFTEST_TRUNC_TO - SELFTEST_CTX, edge, SELFTEST_CTX,
	    &got);
	if (rv != FS_E_OK || got != SELFTEST_CTX) {
		kprintf("apfs-trunc: FAIL cannot read the bytes below the cut "
		    "(rv=%d got=%u)\n", rv, (unsigned)got);
		goto out;
	}

	rv = fs_truncate(&h, SELFTEST_TRUNC_TO);
	/*
	 * FS_E_SPREAD is a documented refusal, not a failure: a cut edits the
	 * extent and inode records in one node, and after enough splits they
	 * are no longer in the same one.  Nothing changed; reported as a skip.
	 */
	if (rv == FS_E_SPREAD) {
		kprintf("apfs-trunc: %s no longer keeps its runs and its "
		    "inode in one leaf -- cutting across two is not "
		    "supported; skipped\n", SELFTEST_PATH);
		goto out;
	}
	if (rv != FS_E_OK) {
		kprintf("apfs-trunc: FAIL cutting %s to %u bytes (rv=%d)\n",
		    SELFTEST_PATH, (unsigned)SELFTEST_TRUNC_TO, rv);
		goto out;
	}
	if (h.fh_size != SELFTEST_TRUNC_TO) {
		kprintf("apfs-trunc: FAIL the handle says %llu bytes after a "
		    "cut to %u\n", (unsigned long long)h.fh_size,
		    (unsigned)SELFTEST_TRUNC_TO);
		goto out;
	}

	/* Reading at the new end gives zero bytes and no error. */
	rv = fs_pread(&h, SELFTEST_TRUNC_TO, back, GROW_CHUNK, &got);
	if (rv != FS_E_OK || got != 0) {
		kprintf("apfs-trunc: FAIL reading at the new end gave %u "
		    "bytes (rv=%d) -- the file did not stop where it says\n",
		    (unsigned)got, rv);
		goto out;
	}

	/*
	 * The volume agrees on the length and on the blocks: the space past
	 * the cut must have come back.
	 */
	if (fs_stat(SELFTEST_PATH, &st) != FS_E_OK) {
		kprintf("apfs-trunc: FAIL cannot stat after cutting\n");
		goto out;
	}
	if (st.fs_size != SELFTEST_TRUNC_TO ||
	    st.fs_alloced != SELFTEST_TRUNC_TO) {
		kprintf("apfs-trunc: FAIL the volume says %llu bytes in %llu "
		    "allocated, wanted %u in %u -- the length reached the "
		    "inode but the blocks did not come back\n",
		    (unsigned long long)st.fs_size,
		    (unsigned long long)st.fs_alloced,
		    (unsigned)SELFTEST_TRUNC_TO, (unsigned)SELFTEST_TRUNC_TO);
		goto out;
	}

	/* Two blocks, in one allocation, which is therefore one run. */
	rv = fs_pwrite(&h, SELFTEST_TRUNC_TO, chunk, 2u * GROW_CHUNK, &put);
	if (rv != FS_E_OK || put != 2u * GROW_CHUNK) {
		kprintf("apfs-trunc: FAIL cannot append two blocks (rv=%d "
		    "put=%u)\n", rv, (unsigned)put);
		goto out;
	}

	/* Half of it away: the cut lands inside that run, so it is shortened. */
	shortens = fs_apfs_shortens();
	rv = fs_truncate(&h, SELFTEST_TRUNC_TO + GROW_CHUNK);
	if (rv != FS_E_OK) {
		kprintf("apfs-trunc: FAIL cutting inside the appended run "
		    "(rv=%d)\n", rv);
		goto out;
	}
	if (fs_apfs_shortens() != shortens + 1) {
		kprintf("apfs-trunc: FAIL a cut that lands inside a two-block "
		    "run shortened %llu records -- it should shorten exactly "
		    "one\n", (unsigned long long)(fs_apfs_shortens() -
		    shortens));
		goto out;
	}

	/*
	 * One block back.  The block right past the run was just freed and
	 * the free queue holds it, so the allocator goes elsewhere and the
	 * file gets a second record.
	 */
	rv = fs_pwrite(&h, SELFTEST_TRUNC_TO + GROW_CHUNK, chunk, GROW_CHUNK,
	    &put);
	if (rv != FS_E_OK || put != GROW_CHUNK) {
		kprintf("apfs-trunc: FAIL cannot append the block that must "
		    "not merge (rv=%d put=%u)\n", rv, (unsigned)put);
		goto out;
	}

	/* And away, which is a whole run past the end: a record leaves. */
	drops = fs_apfs_drops();
	rv = fs_truncate(&h, SELFTEST_TRUNC_TO + GROW_CHUNK);
	if (rv != FS_E_OK) {
		kprintf("apfs-trunc: FAIL cutting the run away (rv=%d)\n", rv);
		goto out;
	}
	if (fs_apfs_drops() != drops + 1) {
		kprintf("apfs-trunc: FAIL cutting away a whole run dropped "
		    "%llu records -- either it was folded into the run before "
		    "it, which the free queue should have prevented, or a run "
		    "past the end was left describing nothing\n",
		    (unsigned long long)(fs_apfs_drops() - drops));
		goto out;
	}

	/* Back to what the file ships at, for the growth test to undo. */
	rv = fs_truncate(&h, SELFTEST_TRUNC_TO);
	if (rv != FS_E_OK) {
		kprintf("apfs-trunc: FAIL cutting back to %u (rv=%d)\n",
		    (unsigned)SELFTEST_TRUNC_TO, rv);
		goto out;
	}

	/* Through all of that, the bytes below the cut never moved. */
	rv = fs_pread(&h, SELFTEST_TRUNC_TO - SELFTEST_CTX, back, SELFTEST_CTX,
	    &got);
	if (rv != FS_E_OK || got != SELFTEST_CTX) {
		kprintf("apfs-trunc: FAIL cannot read below the cut afterwards "
		    "(rv=%d got=%u)\n", rv, (unsigned)got);
		goto out;
	}
	for (i = 0; i < SELFTEST_CTX; i++) {
		if (back[i] == edge[i])
			continue;
		kprintf("apfs-trunc: FAIL byte %u below the cut is 0x%02x and "
		    "was 0x%02x -- cutting the tail off disturbed what was "
		    "kept\n", (unsigned)i, (unsigned)back[i],
		    (unsigned)edge[i]);
		goto out;
	}

	if (fs_stat(SELFTEST_PATH, &st) != FS_E_OK ||
	    st.fs_size != SELFTEST_TRUNC_TO ||
	    st.fs_alloced != SELFTEST_TRUNC_TO) {
		kprintf("apfs-trunc: FAIL %s did not end at %u bytes in %u "
		    "allocated\n", SELFTEST_PATH, (unsigned)SELFTEST_TRUNC_TO,
		    (unsigned)SELFTEST_TRUNC_TO);
		goto out;
	}

	kprintf("apfs-trunc: PASS -- %s cut %llu -> %u bytes, a run shortened "
	    "in place and a run dropped out of both trees, the %u bytes below "
	    "the cut untouched\n", SELFTEST_PATH, (unsigned long long)was,
	    (unsigned)SELFTEST_TRUNC_TO, (unsigned)SELFTEST_CTX);
out:
	/* As above: the hold is released on every path. */
	(void)fs_close(&h);
	kfree(edge);
	kfree(back);
	kfree(chunk);
}

/*
 * The file this test makes, in a directory small enough to count.  Left
 * behind on purpose: only the next boot finding it shows the create
 * reached the platter.
 */
#define	SELFTEST_MADE_DIR	"/etc"
#define	SELFTEST_MADE		"/etc/made.txt"
#define	SELFTEST_MADE_MARK	"style9 made this file from nothing.\n"

/*
 * Take every name out of a directory someone else has written into.
 * Always removes entry zero: the listing is live, and indices shift as
 * names go.  Returns 0, having said why, if it will not empty.
 */
static int
dir_clear(const char *path, int held)
{
	struct fs_dirent	de;
	char			full[FS_NAME_MAX];
	size_t			dlen;
	size_t			i;
	int			gone;
	int			rv;

	dlen = slen(path);
	for (gone = 0; gone < held + 1; gone++) {
		rv = fs_readdir(path, 0, &de);
		if (rv == 0)
			break;			/* empty now */
		if (rv < 0) {
			kprintf("apfs-dirs: FAIL cannot list %s while "
			    "clearing it\n", path);
			return (0);
		}
		if (dlen + 1 + slen(de.fde_name) + 1 > sizeof(full)) {
			kprintf("apfs-dirs: FAIL %s/%s is too long a name to "
			    "remove\n", path, de.fde_name);
			return (0);
		}
		for (i = 0; i < dlen; i++)
			full[i] = path[i];
		full[dlen] = '/';
		for (i = 0; de.fde_name[i] != '\0'; i++)
			full[dlen + 1 + i] = de.fde_name[i];
		full[dlen + 1 + i] = '\0';
		rv = de.fde_is_dir ? fs_rmdir(full) : fs_unlink(full);
		if (rv != FS_E_OK) {
			kprintf("apfs-dirs: FAIL cannot take %s out of %s "
			    "(rv=%d)\n", de.fde_name, path, rv);
			return (0);
		}
	}
	kprintf("apfs-dirs: %s came back holding %d name(s) this test did not "
	    "make -- somebody has been using this volume, which is what it is "
	    "for; cleared\n", path, held);
	return (1);
}

/* How many names a directory holds, and whether one of them is `want`. */
static int
dir_count(const char *path, const char *want, int *saw_want)
{
	struct fs_dirent	de;
	uint32_t		i;
	int			n;
	int			rv;

	n = 0;
	if (saw_want != NULL)
		*saw_want = 0;
	for (i = 0; i < 4096u; i++) {
		rv = fs_readdir(path, i, &de);
		if (rv <= 0)
			return (rv == 0 ? n : -1);
		n++;
		if (saw_want != NULL && same((const uint8_t *)de.fde_name,
		    (const uint8_t *)want, slen(want) + 1))
			*saw_want = 1;
	}
	return (-1);
}

void
fs_make_selftest(void)
{
	struct fs_handle	 h;
	struct fs_statbuf	 st;
	uint8_t			 back[sizeof(SELFTEST_MADE_MARK) - 1];
	const char		*mark = SELFTEST_MADE_MARK;
	const uint32_t		 marklen = sizeof(SELFTEST_MADE_MARK) - 1;
	uint64_t		 ino;
	uint64_t		 holes;
	uint32_t		 got;
	uint32_t		 put;
	uint32_t		 i;
	int			 before;
	int			 after;
	int			 saw;
	int			 rv;
	int			 had;

	if (!fs_apfs_ready())
		return;

	before = dir_count(SELFTEST_MADE_DIR, "made.txt", &saw);
	if (before < 0) {
		kprintf("apfs-make: FAIL cannot list %s\n", SELFTEST_MADE_DIR);
		return;
	}
	had = fs_stat(SELFTEST_MADE, &st) == FS_E_OK;
	if (had != saw) {
		kprintf("apfs-make: FAIL %s %s by name and %s in the "
		    "directory listing\n", SELFTEST_MADE,
		    had ? "exists" : "does not exist",
		    saw ? "appears" : "does not appear");
		return;
	}

	if (had) {
		/*
		 * What the previous boot made and left: the only proof that a
		 * create reached the disk.
		 */
		if (st.fs_size != marklen) {
			kprintf("apfs-make: FAIL %s is %llu bytes and the boot "
			    "that made it wrote %u\n", SELFTEST_MADE,
			    (unsigned long long)st.fs_size, (unsigned)marklen);
			return;
		}
		if (fs_open(SELFTEST_MADE, &h) != FS_E_OK ||
		    fs_pread(&h, 0, back, marklen, &got) != FS_E_OK ||
		    got != marklen) {
			kprintf("apfs-make: FAIL cannot read %s back\n",
			    SELFTEST_MADE);
			(void)fs_close(&h);
			return;
		}
		/*
		 * Closed before the unlink below: with the hold still here the
		 * unlink would take only the name and leave an orphan.
		 */
		(void)fs_close(&h);
		for (i = 0; i < marklen; i++) {
			if (back[i] == (uint8_t)mark[i])
				continue;
			kprintf("apfs-make: FAIL byte %u of %s is 0x%02x, "
			    "wanted 0x%02x -- a file made by an earlier boot "
			    "did not survive it\n", (unsigned)i, SELFTEST_MADE,
			    (unsigned)back[i], (unsigned)mark[i]);
			return;
		}

		rv = fs_unlink(SELFTEST_MADE);
		if (rv != FS_E_OK) {
			kprintf("apfs-make: FAIL cannot unlink %s (rv=%d)\n",
			    SELFTEST_MADE, rv);
			return;
		}
		if (fs_stat(SELFTEST_MADE, &st) != FS_E_NOTFOUND) {
			kprintf("apfs-make: FAIL %s still resolves after "
			    "being unlinked\n", SELFTEST_MADE);
			return;
		}
		after = dir_count(SELFTEST_MADE_DIR, "made.txt", &saw);
		if (after != before - 1 || saw) {
			kprintf("apfs-make: FAIL %s held %d names and holds "
			    "%d after one was removed%s\n", SELFTEST_MADE_DIR,
			    before, after, saw ? ", and still lists it" : "");
			return;
		}
		before = after;
	} else {
		kprintf("apfs-make: %s is absent -- this is the first boot on "
		    "this image, so there is nothing to have survived yet\n",
		    SELFTEST_MADE);
	}

	/*
	 * And make it.  The hole counter is read across the create alone:
	 * after the unlink above, the node has holes exactly the size this
	 * create wants, and it must reuse them rather than take the room from
	 * a span that never grows back.
	 */
	holes = fs_apfs_holes();
	ino   = 0;
	rv = fs_create(SELFTEST_MADE, 0644, &ino);
	/* The writer splits a full leaf rather than refuse (apfs-room). */
	if (rv != FS_E_OK || ino == 0) {
		kprintf("apfs-make: FAIL cannot make %s (rv=%d ino=%llu)\n",
		    SELFTEST_MADE, rv, (unsigned long long)ino);
		return;
	}
	if (had && fs_apfs_holes() == holes) {
		kprintf("apfs-make: FAIL making a file straight after "
		    "unlinking one of the same shape took no room from the "
		    "free lists -- the node loses a record's worth of span "
		    "every boot and will refuse a name after about fifteen\n");
		return;
	}

	/* The volume agrees, by name, by number and in its directory. */
	if (fs_stat(SELFTEST_MADE, &st) != FS_E_OK) {
		kprintf("apfs-make: FAIL %s does not resolve after being "
		    "made\n", SELFTEST_MADE);
		return;
	}
	if (st.fs_ino != ino || st.fs_size != 0 || st.fs_is_dir ||
	    !FS_ISREG(st.fs_mode)) {
		kprintf("apfs-make: FAIL %s is inode %llu of %llu bytes, mode "
		    "%#o -- wanted inode %llu, empty, a regular file\n",
		    SELFTEST_MADE, (unsigned long long)st.fs_ino,
		    (unsigned long long)st.fs_size, (unsigned)st.fs_mode,
		    (unsigned long long)ino);
		return;
	}
	after = dir_count(SELFTEST_MADE_DIR, "made.txt", &saw);
	if (after != before + 1 || !saw) {
		kprintf("apfs-make: FAIL %s held %d names and holds %d after "
		    "one was made%s\n", SELFTEST_MADE_DIR, before, after,
		    saw ? "" : ", and does not list it");
		return;
	}

	/* A name is taken once.  Asking again is an error, not a truncation. */
	rv = fs_create(SELFTEST_MADE, 0644, NULL);
	if (rv != FS_E_EXIST) {
		kprintf("apfs-make: FAIL making %s a second time answered %d, "
		    "wanted %d -- a create that quietly replaces a file is a "
		    "different call\n", SELFTEST_MADE, rv, FS_E_EXIST);
		return;
	}
	rv = fs_unlink(SELFTEST_MADE_DIR);
	if (rv != FS_E_ISDIR) {
		kprintf("apfs-make: FAIL unlinking the directory %s answered "
		    "%d, wanted %d\n", SELFTEST_MADE_DIR, rv, FS_E_ISDIR);
		return;
	}

	/*
	 * Bytes into a file that has none, through the growth path: its first
	 * block, first extent record and first owner.
	 */
	if (fs_open(SELFTEST_MADE, &h) != FS_E_OK) {
		kprintf("apfs-make: FAIL cannot open %s\n", SELFTEST_MADE);
		return;
	}
	rv = fs_pwrite(&h, 0, (const uint8_t *)mark, marklen, &put);
	if (rv != FS_E_OK || put != marklen) {
		kprintf("apfs-make: FAIL writing %u bytes into a file with "
		    "none (rv=%d put=%u)\n", (unsigned)marklen, rv,
		    (unsigned)put);
		goto done;
	}
	if (fs_pread(&h, 0, back, marklen, &got) != FS_E_OK ||
	    got != marklen || !same(back, (const uint8_t *)mark, marklen)) {
		kprintf("apfs-make: FAIL %s does not read back what was just "
		    "written into it\n", SELFTEST_MADE);
		goto done;
	}
	if (fs_stat(SELFTEST_MADE, &st) != FS_E_OK || st.fs_size != marklen) {
		kprintf("apfs-make: FAIL %s does not say it is %u bytes\n",
		    SELFTEST_MADE, (unsigned)marklen);
		goto done;
	}

	kprintf("apfs-make: PASS -- %s made as inode %llu, %u bytes written "
	    "into a file that had none, %d names in %s%s\n", SELFTEST_MADE,
	    (unsigned long long)ino, (unsigned)marklen, after,
	    SELFTEST_MADE_DIR,
	    had ? ", the one the boot before left having been read and "
	    "removed first" : "");
done:
	/* One exit after the open, so the hold is always released. */
	(void)fs_close(&h);
}

/*
 * The directory this test makes, left on the volume for the next boot to
 * find, as with the file above.  It also checks that the new directory is
 * a directory to the rest of the kernel: a name is made inside it (the
 * lookup descends into it, the writer keys an entry under it), and its
 * removal is then refused while the name is there.  Outcomes are checked
 * against the writer's own counters, not just the error codes, since a
 * half-done edit also returns an error.
 */
#define	SELFTEST_DIRS		"/etc/madedir"
#define	SELFTEST_DIRS_SLASH	"/etc/madedir/"
#define	SELFTEST_DIRS_FILE	"/etc/madedir/inside.txt"
/*
 * The mode this test leaves, which nothing else produces (mkdir here makes
 * 0755): found next boot, it proves the chmod reached the platter.
 */
#define	SELFTEST_DIRS_MODE	0711

void
fs_dirs_selftest(void)
{
	struct fs_statbuf	 st;
	uint64_t		 ino;
	uint64_t		 count;
	int			 before;
	int			 after;
	int			 held;
	int			 saw;
	int			 rv;
	int			 had;

	if (!fs_apfs_ready())
		return;

	before = dir_count(SELFTEST_MADE_DIR, "madedir", &saw);
	if (before < 0) {
		kprintf("apfs-dirs: FAIL cannot list %s\n", SELFTEST_MADE_DIR);
		return;
	}
	had = fs_stat(SELFTEST_DIRS, &st) == FS_E_OK;
	if (had != saw) {
		kprintf("apfs-dirs: FAIL %s %s by name and %s in the "
		    "directory listing\n", SELFTEST_DIRS,
		    had ? "exists" : "does not exist",
		    saw ? "appears" : "does not appear");
		return;
	}

	if (had) {
		/* What the previous boot made and left. */
		if (!st.fs_is_dir || st.fs_size != 0) {
			kprintf("apfs-dirs: FAIL %s came back as %s of %llu "
			    "bytes -- wanted a directory of none\n",
			    SELFTEST_DIRS, st.fs_is_dir ? "a directory" :
			    "a file", (unsigned long long)st.fs_size);
			return;
		}
		/*
		 * With the mode the previous boot set.  0755 is accepted too:
		 * an image made before chmod existed.  Anything else fails.
		 */
		if ((st.fs_mode & 07777) == SELFTEST_DIRS_MODE)
			kprintf("apfs-dirs: %s came back wearing %04o -- a "
			    "chmod from the boot before survived the machine "
			    "being switched off\n", SELFTEST_DIRS,
			    (unsigned)SELFTEST_DIRS_MODE);
		else if ((st.fs_mode & 07777) == 0755)
			kprintf("apfs-dirs: %s came back at 0755, which is "
			    "what a boot older than chmod would have left\n",
			    SELFTEST_DIRS);
		else {
			kprintf("apfs-dirs: FAIL %s came back wearing %04o, "
			    "which nothing here writes\n", SELFTEST_DIRS,
			    (unsigned)(st.fs_mode & 07777));
			return;
		}
		/*
		 * It need not come back empty: a user may have written into it
		 * from the shell.  The claim is that it still lists and can be
		 * emptied; the names are cleared and reported, and failing to
		 * empty it is a failure.
		 */
		held = dir_count(SELFTEST_DIRS, NULL, NULL);
		if (held < 0) {
			kprintf("apfs-dirs: FAIL cannot list %s\n",
			    SELFTEST_DIRS);
			return;
		}
		if (held > 0 && !dir_clear(SELFTEST_DIRS, held))
			return;

		count = fs_apfs_dirkills();
		rv = fs_rmdir(SELFTEST_DIRS);
		if (rv != FS_E_OK) {
			kprintf("apfs-dirs: FAIL cannot remove %s (rv=%d)\n",
			    SELFTEST_DIRS, rv);
			return;
		}
		if (fs_apfs_dirkills() != count + 1) {
			kprintf("apfs-dirs: FAIL removing %s answered success "
			    "without the writer having removed one\n",
			    SELFTEST_DIRS);
			return;
		}
		if (fs_stat(SELFTEST_DIRS, &st) != FS_E_NOTFOUND) {
			kprintf("apfs-dirs: FAIL %s still resolves after being "
			    "removed\n", SELFTEST_DIRS);
			return;
		}
		after = dir_count(SELFTEST_MADE_DIR, "madedir", &saw);
		if (after != before - 1 || saw) {
			kprintf("apfs-dirs: FAIL %s held %d names and holds %d "
			    "after one was removed%s\n", SELFTEST_MADE_DIR,
			    before, after, saw ? ", and still lists it" : "");
			return;
		}
		before = after;
	} else {
		kprintf("apfs-dirs: %s is absent -- this is the first boot on "
		    "this image, so there is nothing to have survived yet\n",
		    SELFTEST_DIRS);
	}

	/* And make one.  A full leaf is the writer's business here as well. */
	count = fs_apfs_dirmakes();
	ino   = 0;
	rv = fs_mkdir(SELFTEST_DIRS, 0755, &ino);
	if (rv != FS_E_OK || ino == 0) {
		kprintf("apfs-dirs: FAIL cannot make %s (rv=%d ino=%llu)\n",
		    SELFTEST_DIRS, rv, (unsigned long long)ino);
		return;
	}
	if (fs_apfs_dirmakes() != count + 1) {
		kprintf("apfs-dirs: FAIL making %s answered success without "
		    "the writer having made one\n", SELFTEST_DIRS);
		return;
	}

	/* The volume agrees, by name, by number, by mode and in its parent. */
	if (fs_stat(SELFTEST_DIRS, &st) != FS_E_OK) {
		kprintf("apfs-dirs: FAIL %s does not resolve after being "
		    "made\n", SELFTEST_DIRS);
		return;
	}
	if (st.fs_ino != ino || st.fs_size != 0 || !st.fs_is_dir ||
	    !FS_ISDIR(st.fs_mode)) {
		kprintf("apfs-dirs: FAIL %s is inode %llu of %llu bytes, mode "
		    "%#o -- wanted inode %llu, empty, a directory\n",
		    SELFTEST_DIRS, (unsigned long long)st.fs_ino,
		    (unsigned long long)st.fs_size, (unsigned)st.fs_mode,
		    (unsigned long long)ino);
		return;
	}
	held = dir_count(SELFTEST_DIRS, NULL, NULL);
	if (held != 0) {
		kprintf("apfs-dirs: FAIL a directory made an instant ago holds "
		    "%d name(s)\n", held);
		return;
	}
	after = dir_count(SELFTEST_MADE_DIR, "madedir", &saw);
	if (after != before + 1 || !saw) {
		kprintf("apfs-dirs: FAIL %s held %d names and holds %d after "
		    "one was made%s\n", SELFTEST_MADE_DIR, before, after,
		    saw ? "" : ", and does not list it");
		return;
	}

	/*
	 * A name is taken once, whatever took it; with a trailing separator
	 * it is still the same name.
	 */
	rv = fs_mkdir(SELFTEST_DIRS, 0755, NULL);
	if (rv != FS_E_EXIST) {
		kprintf("apfs-dirs: FAIL making %s a second time answered %d, "
		    "wanted %d\n", SELFTEST_DIRS, rv, FS_E_EXIST);
		return;
	}
	rv = fs_mkdir(SELFTEST_DIRS_SLASH, 0755, NULL);
	if (rv != FS_E_EXIST) {
		kprintf("apfs-dirs: FAIL making %s answered %d, wanted %d -- "
		    "the trailing separator named something else\n",
		    SELFTEST_DIRS_SLASH, rv, FS_E_EXIST);
		return;
	}
	rv = fs_unlink(SELFTEST_DIRS);
	if (rv != FS_E_ISDIR) {
		kprintf("apfs-dirs: FAIL unlinking the directory %s answered "
		    "%d, wanted %d\n", SELFTEST_DIRS, rv, FS_E_ISDIR);
		return;
	}
	rv = fs_rmdir(SELFTEST_PATH);
	if (rv != FS_E_NOTDIR) {
		kprintf("apfs-dirs: FAIL removing the file %s as a directory "
		    "answered %d, wanted %d\n", SELFTEST_PATH, rv,
		    FS_E_NOTDIR);
		return;
	}
	rv = fs_rmdir(SELFTEST_MADE_DIR);
	if (rv != FS_E_NOTEMPTY) {
		kprintf("apfs-dirs: FAIL removing %s, which holds %d names, "
		    "answered %d, wanted %d\n", SELFTEST_MADE_DIR, after, rv,
		    FS_E_NOTEMPTY);
		return;
	}

	/*
	 * A name inside it: the lookup descends into the new directory, the
	 * create keys an entry under its object id, and removal must then be
	 * refused.
	 */
	rv = fs_create(SELFTEST_DIRS_FILE, 0644, NULL);
	if (rv == FS_E_NOALLOC) {
		kprintf("apfs-dirs: PASS -- %s made as inode %llu, %d names in "
		    "%s; the leaf that would hold a name INSIDE it is full, so "
		    "that half is skipped%s\n", SELFTEST_DIRS,
		    (unsigned long long)ino, after, SELFTEST_MADE_DIR,
		    had ? ", the one the boot before left having been removed "
		    "first" : "");
		return;
	}
	if (rv != FS_E_OK) {
		kprintf("apfs-dirs: FAIL cannot make %s inside a directory "
		    "this kernel just made (rv=%d)\n", SELFTEST_DIRS_FILE, rv);
		return;
	}
	held = dir_count(SELFTEST_DIRS, "inside.txt", &saw);
	if (held != 1 || !saw) {
		kprintf("apfs-dirs: FAIL %s holds %d name(s)%s after one was "
		    "made in it\n", SELFTEST_DIRS, held,
		    saw ? "" : " and does not list it");
		return;
	}

	count = fs_apfs_dirkills();
	rv = fs_rmdir(SELFTEST_DIRS);
	if (rv != FS_E_NOTEMPTY) {
		kprintf("apfs-dirs: FAIL removing %s while it holds a name "
		    "answered %d, wanted %d\n", SELFTEST_DIRS, rv,
		    FS_E_NOTEMPTY);
		return;
	}
	if (fs_apfs_dirkills() != count) {
		kprintf("apfs-dirs: FAIL removing %s was refused and the "
		    "writer removed one anyway\n", SELFTEST_DIRS);
		return;
	}
	if (fs_stat(SELFTEST_DIRS_FILE, &st) != FS_E_OK) {
		kprintf("apfs-dirs: FAIL %s did not survive a refused "
		    "removal of the directory holding it\n",
		    SELFTEST_DIRS_FILE);
		return;
	}

	/* Emptied again, and left that way for the boot after this one. */
	rv = fs_unlink(SELFTEST_DIRS_FILE);
	if (rv != FS_E_OK) {
		kprintf("apfs-dirs: FAIL cannot unlink %s (rv=%d)\n",
		    SELFTEST_DIRS_FILE, rv);
		return;
	}
	held = dir_count(SELFTEST_DIRS, NULL, NULL);
	if (held != 0) {
		kprintf("apfs-dirs: FAIL %s holds %d name(s) after the only "
		    "one was removed\n", SELFTEST_DIRS, held);
		return;
	}

	/* Left with SELFTEST_DIRS_MODE for the next boot to find. */
	rv = fs_chmod(SELFTEST_DIRS, SELFTEST_DIRS_MODE);
	if (rv != FS_E_OK) {
		kprintf("apfs-dirs: FAIL chmod of %s answered %d\n",
		    SELFTEST_DIRS, rv);
		return;
	}
	if (fs_stat(SELFTEST_DIRS, &st) != FS_E_OK ||
	    (st.fs_mode & 07777) != SELFTEST_DIRS_MODE) {
		kprintf("apfs-dirs: FAIL %s reads back as %04o after being "
		    "chmodded to %04o\n", SELFTEST_DIRS,
		    (unsigned)(st.fs_mode & 07777),
		    (unsigned)SELFTEST_DIRS_MODE);
		return;
	}
	if (!FS_ISDIR(st.fs_mode)) {
		kprintf("apfs-dirs: FAIL a chmod took the directory bit off "
		    "%s\n", SELFTEST_DIRS);
		return;
	}

	kprintf("apfs-dirs: PASS -- %s made as inode %llu, a name made inside "
	    "it and taken back out, removal refused while it held one, %d "
	    "names in %s%s\n", SELFTEST_DIRS, (unsigned long long)ino, after,
	    SELFTEST_MADE_DIR,
	    had ? ", the directory the boot before left having been read and "
	    "removed first" : "");
}

/*
 * What a shell left on the disk one boot ago.  The file is written by dash
 * redirecting into it (open(2) with O_CREAT, dup2 onto fd 1, echo) during
 * the previous boot, after this test has run; so the first boot skips, and
 * every later one checks that bytes a ring-3 program wrote survived a
 * power cycle.
 *
 * The text is duplicated in user/hello.c on purpose: if the demo changes,
 * this fails rather than checking nothing.
 */
#define	SELFTEST_SHELL		"/etc/notes.txt"
#define	SELFTEST_SHELL_TEXT	"a line from a real Apple shell\n" \
				"appended by the same shell\n"

void
fs_shell_selftest(void)
{
	struct fs_handle	 h;
	uint8_t			 back[sizeof(SELFTEST_SHELL_TEXT)];
	const char		*want = SELFTEST_SHELL_TEXT;
	const uint32_t		 wantlen = sizeof(SELFTEST_SHELL_TEXT) - 1;
	uint32_t		 got;
	int			 rv;

	if (!fs_apfs_ready())
		return;

	rv = fs_open(SELFTEST_SHELL, &h);
	if (rv == FS_E_NOTFOUND) {
		kprintf("apfs-shell: %s is not there -- no shell has written "
		    "to this volume yet; skipped\n", SELFTEST_SHELL);
		goto done;
	}
	if (rv != FS_E_OK) {
		kprintf("apfs-shell: FAIL cannot open %s (rv=%d)\n",
		    SELFTEST_SHELL, rv);
		goto done;
	}
	/*
	 * Empty is a legal state: a redirection creates the file, then
	 * writes it, and a checkpoint may land in between; power off there
	 * leaves the name with nothing in it.  (It can also be missing
	 * entirely, handled above.)  Any other content is a failure.
	 */
	if (h.fh_size == 0) {
		kprintf("apfs-shell: %s is there but empty -- a boot was "
		    "interrupted between the shell creating it and writing "
		    "into it, which is a state this volume is allowed to be "
		    "in; skipped\n", SELFTEST_SHELL);
		goto done;
	}
	if (h.fh_size != wantlen) {
		kprintf("apfs-shell: FAIL %s is %llu bytes and a shell wrote "
		    "%u\n", SELFTEST_SHELL, (unsigned long long)h.fh_size,
		    (unsigned)wantlen);
		goto done;
	}
	if (fs_pread(&h, 0, back, wantlen, &got) != FS_E_OK || got != wantlen) {
		kprintf("apfs-shell: FAIL %s will not read back (got %u of "
		    "%u)\n", SELFTEST_SHELL, (unsigned)got, (unsigned)wantlen);
		goto done;
	}
	if (!same(back, (const uint8_t *)want, wantlen)) {
		kprintf("apfs-shell: FAIL %s holds something other than what "
		    "a shell wrote into it\n", SELFTEST_SHELL);
		goto done;
	}
	kprintf("apfs-shell: PASS -- %u bytes a REAL Apple shell redirected "
	    "into %s in an earlier boot are still there, byte for byte\n",
	    (unsigned)wantlen, SELFTEST_SHELL);
done:
	/* As in the tests above: the hold is released on every path. */
	(void)fs_close(&h);
}

/*
 * Backend self-tests, each run under fs_lock as fs_ckpt_selftest is.  This
 * one: a node asked to run out of room.
 */
void
fs_split_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_split_selftest();
	mutex_unlock(&fs_lock);
}

/* And about the same file every other write test uses. */
void
fs_data_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_data_selftest(SELFTEST_PATH);
	mutex_unlock(&fs_lock);
}

/*
 * And about a node that stops starting where its parent says.  The time
 * is passed in: fs/apfs/apfs.c reads no clock, which is also why create
 * and unlink take a timestamp.
 */
void
fs_index_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_index_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/* And about a node that has nothing left in it. */
void
fs_drop_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_drop_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/* And about a file whose two records a split has put in different nodes. */
void
fs_stream_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_stream_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/* And about the tree that counts the volume's runs outgrowing its root. */
void
fs_extref_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_extref_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/* And about a node a create is asked to make room in for itself. */
void
fs_room_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_room_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/* And about a name that moves without anything being made or destroyed. */
void
fs_move_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_move_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/*
 * Is anything still held?  Asked after the self-tests and before anything
 * a user runs, the one moment the answer must be "nothing".  A leaked hold
 * is otherwise invisible: the next unlink of that file correctly takes
 * only the name, and the bytes wait for a close that never comes.
 */
void
fs_open_check(void)
{
	size_t		i;
	uint32_t	held;

	if (!fs_apfs_ready())
		return;
	held = 0;
	mutex_lock(&fs_lock);
	for (i = 0; i < FS_OPEN_MAX; i++) {
		if (fs_opens[i].fo_refs == 0)
			continue;
		held++;
		kprintf("fs-open: FAIL inode %llu is still held %u time(s)%s "
		    "-- a self-test opened a file and did not give it back\n",
		    (unsigned long long)fs_opens[i].fo_ino,
		    (unsigned)fs_opens[i].fo_refs,
		    fs_opens[i].fo_nameless ? " and has no name left" : "");
	}
	mutex_unlock(&fs_lock);
	if (held == 0)
		kprintf("fs-open: PASS -- of %u rows, every file the "
		    "self-tests opened was given back\n",
		    (unsigned)FS_OPEN_MAX);
}

/* And about a file that outlives its own name. */
void
fs_orphan_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_orphan_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
}

/*
 * A rename lands on a name something is reading.  The writer's own test
 * (apfs-clobber, run first) proves the records.  This proves the promise
 * about descriptors: a holder of the old file goes on reading it, from
 * the platter, while every open of the name gets the new one.
 *
 * The victim's row is checked too: nameless after the rename, gone after
 * the close, since the close is where the file is reaped.
 */
#define	FS_CLOB_NAME	"/etc/usurped.txt"
#define	FS_CLOB_TEMP	"/etc/usurper.txt"
#define	FS_CLOB_OLD	"the file that stood at this name first\n"
#define	FS_CLOB_NEW	"the newcomer the name answers with now\n"

static void
fs_clobber_scene(void)
{
	struct fs_handle	 hold;
	struct fs_handle	 h;
	struct fs_open		*fo;
	uint8_t			 buf[64];
	const char		*fail;
	uint64_t		 vino, nino;
	uint64_t		 orphan0, reap0;
	const uint32_t		 oldn = sizeof(FS_CLOB_OLD) - 1;
	const uint32_t		 newn = sizeof(FS_CLOB_NEW) - 1;
	uint32_t		 got, put;
	bool			 nameless, still;

	fail = NULL;
	vino = 0;
	nino = 0;
	(void)fs_unlink(FS_CLOB_NAME);
	(void)fs_unlink(FS_CLOB_TEMP);

	mutex_lock(&fs_lock);
	orphan0 = fs_n_orphan;
	reap0   = fs_n_reap;
	mutex_unlock(&fs_lock);

	/* The occupant, and a descriptor held on it across everything. */
	if (fs_create(FS_CLOB_NAME, 0644, &vino) != FS_E_OK ||
	    fs_open(FS_CLOB_NAME, &hold) != FS_E_OK) {
		kprintf("fs-clobber: FAIL cannot make and hold the occupant\n");
		return;
	}
	if (fs_pwrite(&hold, 0, (const uint8_t *)FS_CLOB_OLD, oldn,
	    &put) != FS_E_OK || put != oldn)
		fail = "cannot write the occupant's bytes";

	/* The newcomer, under its own name for now. */
	if (fail == NULL &&
	    (fs_create(FS_CLOB_TEMP, 0644, &nino) != FS_E_OK ||
	    fs_open(FS_CLOB_TEMP, &h) != FS_E_OK))
		fail = "cannot make the newcomer";
	else if (fail == NULL) {
		if (fs_pwrite(&h, 0, (const uint8_t *)FS_CLOB_NEW,
		    newn, &put) != FS_E_OK || put != newn)
			fail = "cannot write the newcomer's bytes";
		(void)fs_close(&h);
	}

	/* The takeover. */
	if (fail == NULL && fs_rename(FS_CLOB_TEMP, FS_CLOB_NAME) != FS_E_OK)
		fail = "the rename onto the held name was refused";

	/* The rename must have marked the occupant's row nameless. */
	if (fail == NULL) {
		mutex_lock(&fs_lock);
		fo = open_row(vino);
		nameless = fo != NULL && fo->fo_nameless;
		mutex_unlock(&fs_lock);
		if (!nameless)
			fail = "the occupant's row does not say nameless";
	}

	/* The name answers with the newcomer... */
	if (fail == NULL) {
		if (fs_open(FS_CLOB_NAME, &h) != FS_E_OK)
			fail = "the taken name does not open";
		else {
			got = 0;
			if (h.fh_ino != nino)
				fail = "the taken name is not the newcomer's "
				    "inode";
			else if (fs_pread(&h, 0, buf, sizeof(buf), &got) !=
			    FS_E_OK || got != newn ||
			    !same(buf, (const uint8_t *)FS_CLOB_NEW, newn))
				fail = "the name did not answer with the "
				    "newcomer's bytes";
			(void)fs_close(&h);
		}
	}
	/* ...while the held descriptor still reads the old file whole. */
	if (fail == NULL) {
		got = 0;
		if (fs_pread(&hold, 0, buf, sizeof(buf), &got) != FS_E_OK ||
		    got != oldn ||
		    !same(buf, (const uint8_t *)FS_CLOB_OLD, oldn))
			fail = "the holder did not get the old bytes back";
	}

	/* The last holder lets go, and the file goes with it. */
	(void)fs_close(&hold);
	if (fail == NULL) {
		mutex_lock(&fs_lock);
		still = open_row(vino) != NULL;
		mutex_unlock(&fs_lock);
		if (still)
			fail = "the occupant's row outlived the last close";
	}
	if (fail == NULL) {
		mutex_lock(&fs_lock);
		still = fs_n_orphan != orphan0 + 1 || fs_n_reap != reap0 + 1;
		mutex_unlock(&fs_lock);
		if (still)
			fail = "the orphan and reap counts did not move by "
			    "one each";
	}

	(void)fs_unlink(FS_CLOB_NAME);
	if (fail != NULL) {
		kprintf("fs-clobber: FAIL %s\n", fail);
		return;
	}
	kprintf("fs-clobber: PASS -- the name answered with the newcomer's "
	    "%u bytes at once, the holder read its own file's %u to the last "
	    "one, and the file left when the holder let go\n",
	    (unsigned)newn, (unsigned)oldn);
}

/* The writer's half first, then the descriptor's half. */
void
fs_clobber_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_clobber_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);
	fs_clobber_scene();
}

/* And that a lookup by key agrees with reading the whole tree. */
void
fs_seek_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_seek_selftest();
	mutex_unlock(&fs_lock);
}

/*
 * A kill landing on a task that is writing to the disk: mutex_kill_selftest
 * (kern/mutex.c) with the driver in.  The victim hammers pwrites at a
 * scratch file, so it is probably parked in ata_wait_intr holding fs_lock
 * when the kill's wake arrives.  The kill is declined while a mutex is held
 * (th_mutex_depth): the victim finishes the write, sees the kill between
 * operations and leaves with the lock returned.  The volume must still
 * answer afterwards.
 */
#define	FK_FILE		"/var/db/kill-arena"
#define	FK_SIZE		(64u * 1024u)
#define	FK_PASSES	6u
#define	FK_WAIT_MS	8000u

static volatile uint32_t	fk_ops;		/* pwrites completed  */
static volatile int		fk_finished;	/* ...and surfaced    */

static void
fs_kill_entry(void *arg)
{
	struct fs_handle	 h;
	uint64_t		 off;
	uint32_t		 put;
	unsigned int		 pass, i;
	uint8_t			 blk[4096];

	(void)arg;
	for (pass = 0; pass < FK_PASSES; pass++) {
		if (fs_open(FK_FILE, &h) != FS_E_OK)
			break;
		for (off = 0; off + sizeof(blk) <= FK_SIZE;
		    off += sizeof(blk)) {
			for (i = 0; i < sizeof(blk); i++)
				blk[i] = (uint8_t)(pass * 7u + i);
			if (fs_pwrite(&h, off, blk, sizeof(blk),
			    &put) != FS_E_OK || put != sizeof(blk))
				break;
			fk_ops++;
			/*
			 * No syscall boundary in a kernel worker: check for
			 * the kill between operations, holding nothing.
			 */
			if (task_kill_pending(current_thread->th_task))
				break;
		}
		(void)fs_close(&h);
		if (task_kill_pending(current_thread->th_task))
			break;
	}
	fk_finished = 1;
	/* The trampoline's thread_exit retires us, empty-handed. */
}

void
fs_kill_selftest(void)
{
	struct fs_handle	 h;
	struct task		*vt;
	struct thread		*th;
	uint64_t		 ino;
	uint32_t		 got;
	unsigned int		 i;
	uint8_t			 probe[64];
	bool			 lock_free;
	int			 rv;

	if (!fs_apfs_ready())
		return;

	rv = fs_create(FK_FILE, 0644, &ino);
	if (rv != FS_E_OK && rv != FS_E_EXIST) {
		kprintf("fs-kill: FAIL the arena cannot be made (rv=%d)\n",
		    rv);
		return;
	}
	if (fs_open(FK_FILE, &h) != FS_E_OK) {
		kprintf("fs-kill: FAIL the arena cannot be opened\n");
		return;
	}
	if (h.fh_size < FK_SIZE && fs_truncate(&h, FK_SIZE) != FS_E_OK) {
		(void)fs_close(&h);
		(void)fs_unlink(FK_FILE);
		kprintf("fs-kill: FAIL the arena cannot be sized\n");
		return;
	}
	(void)fs_close(&h);

	vt = task_create("fs-kill");
	if (vt == NULL) {
		kprintf("fs-kill: FAIL no task to kill\n");
		return;
	}
	fk_ops      = 0;
	fk_finished = 0;
	th = thread_create(vt, fs_kill_entry, NULL, "fs-kill");
	if (th == NULL) {
		kprintf("fs-kill: FAIL no thread to kill\n");
		task_deref(vt);
		return;
	}
	thread_start(th);

	/* Let it get well into the write storm, then kill it mid-stride. */
	for (i = 0; i < FK_WAIT_MS && fk_ops < 4 && fk_finished == 0; i++)
		sched_nap_ms(1);
	if (fk_ops < 4 && fk_finished == 0) {
		kprintf("fs-kill: FAIL the victim never started writing\n");
		task_deref(vt);
		return;
	}
	task_request_terminate(vt->t_id);

	for (i = 0; i < FK_WAIT_MS && fk_finished == 0; i++)
		sched_nap_ms(1);
	if (fk_finished == 0) {
		kprintf("fs-kill: FAIL the victim never surfaced from the "
		    "write it was killed in\n");
		task_deref(vt);
		return;
	}

	/* The lock came home rather than to the grave... */
	lock_free = false;
	for (i = 0; i < FK_WAIT_MS && !lock_free; i++) {
		if (mutex_trylock(&fs_lock)) {
			mutex_unlock(&fs_lock);
			lock_free = true;
		} else
			sched_nap_ms(1);
	}
	if (!lock_free) {
		kprintf("fs-kill: FAIL fs_lock is in a dead thread's hand\n");
		task_deref(vt);
		return;
	}

	/* ...and the volume answers as though nothing had happened. */
	if (fs_open(FK_FILE, &h) != FS_E_OK ||
	    fs_pread(&h, 0, probe, sizeof(probe), &got) != FS_E_OK ||
	    got != sizeof(probe)) {
		kprintf("fs-kill: FAIL the volume stopped answering after "
		    "the kill\n");
		task_deref(vt);
		return;
	}
	(void)fs_close(&h);
	if (fs_unlink(FK_FILE) != FS_E_OK) {
		kprintf("fs-kill: FAIL the arena cannot be unlinked\n");
		task_deref(vt);
		return;
	}
	task_deref(vt);

	kprintf("fs-kill: PASS -- a task killed %u pwrites into a write "
	    "storm surfaced with the volume lock returned, and the volume "
	    "answered a read and an unlink as though nothing had happened\n",
	    (unsigned)fk_ops);
}

/*
 * The published past by name, at this layer.  apfs-view (the backend's
 * test) proves the mechanism.  This proves the promise about paths and
 * descriptors: /.xid/<N>/etc/x answers with the bytes fsync published at
 * N, /.xid lists what can be reached, every change under it is refused
 * with FS_E_ROFS, and a handle onto the past reads until the window moves
 * and then answers FS_E_GONE.
 *
 * Checkpoints are taken from fs_apfs_xid after each sync, not assumed to
 * advance by one: a room-forced checkpoint may land inside the test.
 */
#define	FS_VIEW_FILE	"/etc/viewpath.txt"
#define	FS_VIEW_SCRATCH	"/etc/viewpath-scratch.txt"
#define	FS_VIEW_FIRST	"the first thing published under this name\n"
#define	FS_VIEW_SECOND	"and the second, a little longer, published after\n"

/* "/.xid/<xid><rest>" */
static void
fs_view_path(char *dst, size_t cap, uint64_t xid, const char *rest)
{
	size_t	i;
	size_t	n;

	n = 0;
	for (i = 0; FS_VIEW_DIR[i] != '\0' && n + 1 < cap; i++)
		dst[n++] = FS_VIEW_DIR[i];
	if (n + 1 < cap)
		dst[n++] = '/';
	view_name(dst + n, cap - n, xid);
	n += slen(dst + n);
	for (i = 0; rest[i] != '\0' && n + 1 < cap; i++)
		dst[n++] = rest[i];
	dst[n] = '\0';
}

/* Make the name if it is not there, then put `text` at its start. */
static int
fs_view_publish(const char *path, const char *text)
{
	struct fs_handle	h;
	uint64_t		ino;
	uint32_t		put;
	int			rv;

	rv = fs_create(path, 0644, &ino);
	if (rv != FS_E_OK && rv != FS_E_EXIST)
		return (rv);
	rv = fs_open(path, &h);
	if (rv != FS_E_OK)
		return (rv);
	rv = fs_pwrite(&h, 0, (const uint8_t *)text, (uint32_t)slen(text),
	    &put);
	if (rv == FS_E_OK && put != slen(text))
		rv = FS_E_IO;
	(void)fs_close(&h);
	return (rv);
}

/* 1 = the file at `path` is exactly `text`; 0 = it is not; <0 = the FS_E_* */
static int
fs_view_reads(const char *path, const char *text)
{
	uint8_t		*buf;
	uint32_t	 got;
	int		 rv;
	int		 ok;

	rv = fs_slurp(path, &buf, &got);
	if (rv != FS_E_OK)
		return (rv);
	ok = (got == slen(text) && same(buf, (const uint8_t *)text, got));
	kfree(buf);
	return (ok);
}

void
fs_view_selftest(void)
{
	struct fs_handle	 h;
	struct fs_statbuf	 st;
	struct fs_dirent	 de;
	char			 p1[64];
	char			 p2[64];
	char			 pd[64];
	uint8_t			 back[128];
	uint64_t		 listed[FS_VIEW_LIST_MAX];
	uint64_t		 x1, x2, x3, x4;
	uint64_t		 ino;
	uint32_t		 n;
	uint32_t		 got;
	uint32_t		 i;
	int			 rv;
	int			 opened;

	if (!fs_apfs_ready())
		return;
	opened = 0;

	/* The mechanism first, under the lock like every backend test. */
	mutex_lock(&fs_lock);
	fs_apfs_view_selftest((uint64_t)clock_walltime_us() * 1000ULL);
	mutex_unlock(&fs_lock);

	/* Whatever an interrupted run left, then a checkpoint to stand on. */
	(void)fs_unlink(FS_VIEW_FILE);
	(void)fs_unlink(FS_VIEW_SCRATCH);
	if (fs_sync() != FS_E_OK) {
		kprintf("fs-view: FAIL the opening sync was refused\n");
		return;
	}

	/* Two publications of one name, each a checkpoint. */
	rv = fs_view_publish(FS_VIEW_FILE, FS_VIEW_FIRST);
	if (rv == FS_E_OK)
		rv = fs_sync();
	if (rv != FS_E_OK) {
		kprintf("fs-view: FAIL cannot publish the first text (%d)\n",
		    rv);
		goto clean;
	}
	x1 = fs_apfs_xid();
	rv = fs_view_publish(FS_VIEW_FILE, FS_VIEW_SECOND);
	if (rv == FS_E_OK)
		rv = fs_sync();
	if (rv != FS_E_OK) {
		kprintf("fs-view: FAIL cannot publish the second text (%d)\n",
		    rv);
		goto clean;
	}
	x2 = fs_apfs_xid();
	if (x2 <= x1) {
		kprintf("fs-view: FAIL the second sync left the xid at %llu\n",
		    (unsigned long long)x2);
		goto clean;
	}

	/* By name: each checkpoint answers with what it published. */
	fs_view_path(p1, sizeof(p1), x1, FS_VIEW_FILE);
	fs_view_path(p2, sizeof(p2), x2, FS_VIEW_FILE);
	if (fs_view_reads(p1, FS_VIEW_FIRST) != 1) {
		kprintf("fs-view: FAIL %s does not read as the first text\n",
		    p1);
		goto clean;
	}
	if (fs_view_reads(p2, FS_VIEW_SECOND) != 1) {
		kprintf("fs-view: FAIL %s does not read as the second text\n",
		    p2);
		goto clean;
	}
	if (fs_view_reads(FS_VIEW_FILE, FS_VIEW_SECOND) != 1) {
		kprintf("fs-view: FAIL the live name does not read as the "
		    "second text\n");
		goto clean;
	}

	/* The directory of checkpoints, and a checkpoint as a directory. */
	if (fs_stat(FS_VIEW_DIR, &st) != FS_E_OK || !st.fs_is_dir ||
	    st.fs_ino != FS_VIEW_INO(0) || (st.fs_mode & 0222) != 0) {
		kprintf("fs-view: FAIL %s does not stat as a read-only "
		    "directory under its own inode\n", FS_VIEW_DIR);
		goto clean;
	}
	fs_view_path(pd, sizeof(pd), x2, "");
	if (fs_stat(pd, &st) != FS_E_OK || !st.fs_is_dir ||
	    st.fs_ino != FS_VIEW_INO(x2)) {
		kprintf("fs-view: FAIL %s does not stat as a directory under "
		    "the inode its listing entry carries\n", pd);
		goto clean;
	}
	fs_view_path(pd, sizeof(pd), x2 + 1000, "");
	if (fs_stat(pd, &st) != FS_E_NOTFOUND) {
		kprintf("fs-view: FAIL %s, a checkpoint never written, "
		    "stats\n", pd);
		goto clean;
	}
	if (fs_stat("/.xid/latest", &st) != FS_E_NOTFOUND) {
		kprintf("fs-view: FAIL /.xid/latest, which is not a number, "
		    "stats\n");
		goto clean;
	}
	n = 0;
	while (n < FS_VIEW_LIST_MAX && fs_readdir(FS_VIEW_DIR, n, &de) == 1) {
		listed[n] = de.fde_ino & ~FS_VIEW_INO(0);
		if (!de.fde_is_dir || de.fde_ino != FS_VIEW_INO(listed[n]) ||
		    (n > 0 && listed[n] <= listed[n - 1])) {
			kprintf("fs-view: FAIL entry %u of %s is \"%s\" -- "
			    "not a directory, or out of order\n", (unsigned)n,
			    FS_VIEW_DIR, de.fde_name);
			goto clean;
		}
		n++;
	}
	if (n == 0 || listed[n - 1] != x2) {
		kprintf("fs-view: FAIL %s lists %u checkpoint(s) and the "
		    "newest is not %llu\n", FS_VIEW_DIR, (unsigned)n,
		    (unsigned long long)x2);
		goto clean;
	}
	for (i = 0; i < n && listed[i] != x1; i++)
		continue;
	if (i == n) {
		kprintf("fs-view: FAIL %s does not list %llu, which was "
		    "published moments ago\n", FS_VIEW_DIR,
		    (unsigned long long)x1);
		goto clean;
	}

	/* Every way of changing the past, refused with the one word. */
	fs_view_path(pd, sizeof(pd), x2, "/etc/nope.txt");
	if (fs_create(pd, 0644, &ino) != FS_E_ROFS ||
	    fs_mkdir(pd, 0755, &ino) != FS_E_ROFS ||
	    fs_unlink(p2) != FS_E_ROFS ||
	    fs_rmdir(pd) != FS_E_ROFS ||
	    fs_rename(p2, "/etc/viewpath-moved.txt") != FS_E_ROFS ||
	    fs_rename("/etc/viewpath-moved.txt", p2) != FS_E_ROFS ||
	    fs_chmod(p2, 0600) != FS_E_ROFS) {
		kprintf("fs-view: FAIL something under %s could be changed, "
		    "or was refused with a word other than ROFS\n",
		    FS_VIEW_DIR);
		goto clean;
	}

	/* A handle onto the past: reads it, will not write it. */
	rv = fs_open(p1, &h);
	if (rv != FS_E_OK || h.fh_xid != x1 ||
	    h.fh_size != slen(FS_VIEW_FIRST)) {
		kprintf("fs-view: FAIL opening %s answered %d (xid %llu, %llu "
		    "bytes)\n", p1, rv, (unsigned long long)h.fh_xid,
		    (unsigned long long)h.fh_size);
		goto clean;
	}
	opened = 1;
	if (fs_pread(&h, 0, back, sizeof(back), &got) != FS_E_OK ||
	    got != slen(FS_VIEW_FIRST) ||
	    !same(back, (const uint8_t *)FS_VIEW_FIRST, got)) {
		kprintf("fs-view: FAIL the handle onto %s does not read the "
		    "first text\n", p1);
		goto clean;
	}
	if (fs_pwrite(&h, 0, back, 1, &got) != FS_E_ROFS ||
	    fs_truncate(&h, 0) != FS_E_ROFS) {
		kprintf("fs-view: FAIL the handle onto %s accepted a write or "
		    "a truncate\n", p1);
		goto clean;
	}

	/*
	 * Slide the window with two more publications.  What the free queue
	 * lets go of is its retention's decision, so the claims below are
	 * about the listing's own width; then the old handle is asked again.
	 */
	rv = fs_unlink(FS_VIEW_FILE);
	if (rv == FS_E_OK)
		rv = fs_sync();
	if (rv == FS_E_OK)
		rv = fs_view_publish(FS_VIEW_SCRATCH, "x");
	if (rv == FS_E_OK)
		rv = fs_sync();
	if (rv != FS_E_OK) {
		kprintf("fs-view: FAIL sliding the window was refused (%d)\n",
		    rv);
		goto clean;
	}
	x4 = fs_apfs_xid();
	x3 = 0;
	n = 0;
	while (n < FS_VIEW_LIST_MAX && fs_readdir(FS_VIEW_DIR, n, &de) == 1) {
		listed[n] = de.fde_ino & ~FS_VIEW_INO(0);
		n++;
	}
	if (n == 0 || listed[n - 1] != x4 || listed[0] + (n - 1) != x4) {
		kprintf("fs-view: FAIL after the slide %s lists %u "
		    "checkpoint(s) that are not consecutive up to %llu\n",
		    FS_VIEW_DIR, (unsigned)n, (unsigned long long)x4);
		goto clean;
	}
	x3 = listed[0];			/* the floor, as listed */
	if (x1 >= x3) {
		kprintf("fs-view: the window is %u wide and still holds %llu "
		    "-- its edge is not decidable with three checkpoints; "
		    "skipped\n", (unsigned)n, (unsigned long long)x1);
	} else {
		if (fs_pread(&h, 0, back, sizeof(back), &got) != FS_E_GONE) {
			kprintf("fs-view: FAIL the handle onto %s still "
			    "answers after its checkpoint slid out of the "
			    "window\n", p1);
			goto clean;
		}
		if (fs_view_reads(p1, FS_VIEW_FIRST) != FS_E_GONE) {
			kprintf("fs-view: FAIL %s answers after its "
			    "checkpoint slid out of the window\n", p1);
			goto clean;
		}
		if (fs_stat(p1, &st) != FS_E_GONE) {
			kprintf("fs-view: FAIL %s stats after its checkpoint "
			    "slid out of the window\n", p1);
			goto clean;
		}
	}
	if (fs_close(&h) != FS_E_OK) {
		kprintf("fs-view: FAIL closing the handle onto the past was "
		    "refused\n");
		goto clean;
	}
	opened = 0;

	kprintf("fs-view: PASS -- %s read the first text at xid %llu and the "
	    "second at %llu while the live name held the second; the "
	    "directory stats and lists, seven writes under it were refused "
	    "with ROFS, and a handle onto %llu read it until the window slid "
	    "to %llu..%llu and then answered GONE\n", FS_VIEW_DIR,
	    (unsigned long long)x1, (unsigned long long)x2,
	    (unsigned long long)x1, (unsigned long long)x3,
	    (unsigned long long)x4);

clean:
	if (opened)
		(void)fs_close(&h);
	(void)fs_unlink(FS_VIEW_FILE);
	(void)fs_unlink(FS_VIEW_SCRATCH);
	if (fs_sync() != FS_E_OK)
		kprintf("fs-view: the closing sync was refused\n");
}

