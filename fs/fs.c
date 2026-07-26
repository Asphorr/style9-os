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

/*
 * One lock for the volume, and it lives here rather than in either backend.
 *
 * This is the only door: every caller outside fs/ reaches a filesystem
 * through the functions below, so a lock here covers both backends and any
 * later one, and lets each backend stay written as straight-line code.
 *
 * Until writing existed there was nothing to serialise.  The APFS reader
 * fills its volume state at mount and never touches it again -- the struct
 * says so, every field marked (m) -- so concurrent readers shared only
 * immutable data and the block cache, which has always had its own lock.
 * Writing ends that: two writers can now target the same block, and a reader
 * can now observe a block mid-write, since a 4 KiB APFS block is eight ATA
 * sectors and the drive is under no obligation to make them appear at once.
 *
 * It is a mutex and not a spinlock because every one of these calls reaches
 * the disk, and reaching the disk sleeps.  See kern/mutex.h for why that
 * distinction is not optional in this kernel.
 *
 * Held for the whole of an operation, including a slurp that may read
 * megabytes.  That is a deliberate choice of correctness over concurrency
 * while there is one disk and one lock: the alternative is per-file or
 * per-range locking, which is worth building when there is evidence of
 * contention rather than in anticipation of it.
 */
static struct mutex	fs_lock = MUTEX_INIT("fs");

/*
 * Volume generation: bumped under fs_lock whenever metadata changes, so a
 * handle can tell whether the length it copied is still the length the file
 * has.  Starts at 1 rather than 0 so a zeroed handle -- one that was never
 * filled by fs_open -- can never accidentally match it.
 */
static uint64_t		fs_gen = 1;

/*
 * Two counters, not one, because they answer different questions.  fs_n_stale
 * says how often the mechanism FIRED -- a handle older than the volume -- and
 * is the only one that moves while nothing can change a file's length.
 * fs_n_resize says how often it CORRECTED something, and stays at zero until
 * files can grow.  Collapsing them into one number would make a working
 * mechanism indistinguishable from a dead one for as long as that is true.
 */
static uint64_t		fs_n_stale;
static uint64_t		fs_n_resize;

/*
 * Bring a handle's cached length up to date if the volume moved on since it
 * was made.  Called with fs_lock held, from the two calls that clamp against
 * it.  Nothing to do on FAT, which cannot be written and therefore cannot go
 * stale, and nothing to do in the overwhelmingly common case where the
 * generation has not changed at all.
 */
static void
handle_refresh(struct fs_handle *h)
{
	uint64_t	size;

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
	 * The generation is adopted even when the size turned out unchanged,
	 * and even when the lookup failed.  Otherwise a handle to a file that
	 * nobody is modifying would re-walk the tree on every single read for
	 * the rest of its life, once any unrelated write had happened.
	 */
	h->fh_gen = fs_gen;
}

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
	case FS_APFS_E_NOALLOC:		return (FS_E_NOALLOC);
	case FS_APFS_E_EXIST:		return (FS_E_EXIST);
	case FS_APFS_E_ISDIR:		return (FS_E_ISDIR);
	case FS_APFS_E_NOTDIR:		return (FS_E_NOTDIR);
	case FS_APFS_E_NOTEMPTY:	return (FS_E_NOTEMPTY);
	case FS_APFS_E_SPREAD:		return (FS_E_SPREAD);
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

/*
 * Each operation is written once, as a _locked body, and wrapped.  Wrapping
 * rather than sprinkling lock/unlock through the bodies is what keeps an
 * early return from leaking the lock -- several of these have four or five
 * exits, and the one that forgets is the one nobody finds.
 */

static int
slurp_locked(const char *path, uint8_t **out_buf, uint32_t *out_size)
{

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
	uint64_t	id;
	uint64_t	size;
	uint64_t	ino;
	int		rv;

	if (out == NULL)
		return (FS_E_NOTFOUND);
	out->fh_kind = FS_HANDLE_NONE;
	out->fh_id   = 0;
	out->fh_size = 0;
	out->fh_ino  = 0;
	out->fh_gen  = fs_gen;

	if (fs_apfs_ready()) {
		rv = fs_apfs_open(path, &id, &size, &ino);
		if (rv != FS_APFS_E_OK)
			return (apfs_err(rv));
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

static int
pread_locked(struct fs_handle *h, uint64_t off, uint8_t *buf,
    uint32_t len, uint32_t *out_got)
{

	if (h == NULL || out_got == NULL)
		return (FS_E_NOTFOUND);
	handle_refresh(h);
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

	/*
	 * Before the bounds check, not after: this call refuses a write that
	 * would run past end-of-file, and deciding that against a length some
	 * other writer has already moved is how a legal write gets rejected --
	 * or, once files can grow, how an illegal one gets through.
	 */
	handle_refresh(h);

	/*
	 * A write that runs past the end makes the file longer first.  Not a
	 * clamp and not a second write: the length moves, the records that say
	 * so move with it, and only then are the bytes put down -- so a failure
	 * to grow is a write that did not happen rather than one that half did.
	 *
	 * Starting BEYOND the end is still refused, by fs_apfs_pwrite.  That
	 * would leave a gap no extent describes, which is a sparse file and a
	 * different thing from a longer one.
	 */
	if (off <= h->fh_size && off + (uint64_t)len > h->fh_size) {
		rv = apfs_err(fs_apfs_grow(h->fh_ino, h->fh_id,
		    off + (uint64_t)len));
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
	 * The bytes are down; stamp the file.  A failure to stamp is reported
	 * even though the write itself succeeded, because the alternative is
	 * to return success for a file whose recorded modification time is a
	 * lie -- and a caller told "written" has no way to find out.
	 *
	 * clock_walltime_us is microseconds since the epoch and APFS records
	 * nanoseconds, so the stamp lands on a microsecond boundary.  That is
	 * the clock this machine has, not a rounding choice.
	 */
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_touch(h->fh_ino, now_ns));

	/*
	 * And close the transaction, because stamping the file no longer
	 * writes anything where it stood: the inode's node is copied, and so
	 * is every object between it and the container superblock.  All of
	 * that is reachable only from a checkpoint, so a write that returned
	 * without one would have put its bytes on the disk and left the
	 * modification time in memory.
	 *
	 * One checkpoint per write is not how a filesystem should batch, and
	 * it is what "the bytes are down when this returns" costs until
	 * something above here knows when it is finished.
	 */
	if (rv == FS_E_OK)
		rv = apfs_err(fs_apfs_checkpoint());

	/*
	 * Metadata moved, so every handle in the system is now suspect --
	 * including this one, whose generation is advanced with it so the
	 * writer does not immediately re-read what it just changed.  Bumped
	 * even when the stamp failed: the bytes went down regardless, and a
	 * generation that under-reports change is worse than one that
	 * over-reports it.
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
	 * One acquisition covers the bytes AND the timestamp.  They are two
	 * disk updates describing one event, and a reader that got between
	 * them would see a file whose contents had changed and whose recorded
	 * modification time had not.
	 */
	mutex_lock(&fs_lock);
	rv = pwrite_locked(h, off, buf, len, out_put);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
truncate_locked(struct fs_handle *h, uint64_t new_size)
{
	uint64_t	now_ns;
	int		rv;

	if (h == NULL)
		return (FS_E_NOTFOUND);
	if (h->fh_kind != FS_HANDLE_APFS)
		return (FS_E_ROFS);

	/*
	 * Against the length the volume has now, not the one this handle was
	 * opened with: "shorten it to n" and "lengthen it to n" are different
	 * operations, and which one this is must be decided from what is
	 * actually there.
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

	/* Stamped and closed for exactly the reasons a write is: see above. */
	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_touch(h->fh_ino, now_ns));
	if (rv == FS_E_OK)
		rv = apfs_err(fs_apfs_checkpoint());
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
 * Split a path at its last separator: the directory that holds the last
 * component, and the component.
 *
 * Path parsing belongs here and not in a backend.  The APFS writer is given an
 * object id and a single name, which is what the on-disk records are keyed on;
 * making it re-derive that from a string would put a second path parser next to
 * the first one, and two of those drift.
 *
 * A trailing separator is refused rather than ignored: "make /tmp/x/" says
 * something about directories, and this makes files.  The calls that DO make
 * directories take it off first -- see path_undress below, which exists so
 * that the refusal here can stay a refusal.
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
create_locked(const char *path, uint64_t *ino_out)
{
	char		 dir[FS_NAME_MAX];
	const char	*leaf;
	uint64_t	 parent;
	uint64_t	 now_ns;
	int		 is_dir;
	int		 rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	rv = path_split(path, dir, sizeof(dir), &leaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_lookup(dir, &parent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTFOUND);

	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_create(parent, leaf, now_ns, ino_out));
	if (rv != FS_E_OK)
		return (rv);
	/*
	 * Closed here for the same reason a write is: the records describing
	 * one file are several disk updates, and a reader that got between
	 * them would see a directory naming an inode that does not exist yet.
	 */
	rv = apfs_err(fs_apfs_checkpoint());
	fs_gen++;
	return (rv);
}

int
fs_create(const char *path, uint64_t *ino_out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = create_locked(path, ino_out);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
unlink_locked(const char *path)
{
	char		 dir[FS_NAME_MAX];
	const char	*leaf;
	uint64_t	 parent;
	uint64_t	 now_ns;
	int		 is_dir;
	int		 rv;

	if (!fs_apfs_ready())
		return (fs_fat_ready() ? FS_E_ROFS : FS_E_NOMOUNT);
	rv = path_split(path, dir, sizeof(dir), &leaf);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_lookup(dir, &parent, &is_dir));
	if (rv != FS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_E_NOTFOUND);

	now_ns = (uint64_t)clock_walltime_us() * 1000ULL;
	rv = apfs_err(fs_apfs_unlink(parent, leaf, now_ns));
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_checkpoint());
	fs_gen++;
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
 * Take the trailing separators off a path, keeping "/" itself.
 *
 * Only the two directory calls do this, and the asymmetry is the point.
 * "mkdir /tmp/x/" is a request that can be honoured exactly as written -- the
 * slash claims the name is a directory, and it is about to be one -- whereas
 * "create /tmp/x/" claims something the call cannot deliver, so path_split
 * goes on refusing it rather than quietly making a file with the name the
 * caller did not ask for.
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
 * Making and removing a directory: the same two steps as for a file -- find
 * the directory that will hold the name, then hand the backend an object id
 * and one component -- and one function, because at this layer that is the
 * whole of both of them and they differ only in which call ends them.
 */
static int
dir_locked(const char *path, int make, uint64_t *ino_out)
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
	rv = make ? fs_apfs_mkdir(parent, leaf, now_ns, ino_out) :
	    fs_apfs_rmdir(parent, leaf, now_ns);
	rv = apfs_err(rv);
	if (rv != FS_E_OK)
		return (rv);
	rv = apfs_err(fs_apfs_checkpoint());
	fs_gen++;
	return (rv);
}

int
fs_mkdir(const char *path, uint64_t *ino_out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = dir_locked(path, 1, ino_out);
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_rmdir(const char *path)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = dir_locked(path, 0, NULL);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
stat_locked(const char *path, struct fs_statbuf *out)
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
fs_stat(const char *path, struct fs_statbuf *out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = stat_locked(path, out);
	mutex_unlock(&fs_lock);
	return (rv);
}

static int
readdir_locked(const char *path, uint32_t index, struct fs_dirent *out)
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

int
fs_readdir(const char *path, uint32_t index, struct fs_dirent *out)
{
	int	rv;

	mutex_lock(&fs_lock);
	rv = readdir_locked(path, index, out);
	mutex_unlock(&fs_lock);
	return (rv);
}

int
fs_sync(void)
{
	int	rv;

	mutex_lock(&fs_lock);
	if (fs_apfs_ready())
		rv = apfs_err(fs_apfs_checkpoint());
	else if (fs_fat_ready())
		rv = FS_E_OK;	/* FAT has no transaction to close */
	else
		rv = FS_E_NOMOUNT;
	mutex_unlock(&fs_lock);
	return (rv);
}

/*
 * Under the same lock, because the test writes checkpoints: everything it
 * checks is state fs_apfs_checkpoint moves, and a reader arriving between the
 * write and the check would see a container the test has not finished
 * describing.
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

	if (fs_n_stale == 0)
		return;
	kprintf("fs: volume generation %llu -- %llu stale handle(s) refreshed, "
	    "%llu had actually changed length\n",
	    (unsigned long long)fs_gen, (unsigned long long)fs_n_stale,
	    (unsigned long long)fs_n_resize);
}

/* ---- write self-test ------------------------------------------------------ */

/*
 * The file this exercises.  It is the multi-extent one on the test image
 * (4096 bytes in one extent, the rest in another), which matters: the probe
 * offset below is chosen to straddle both the 4 KiB block boundary AND the
 * boundary between those two extents, so one 12-byte write has to find two
 * different physical runs and read-modify-write a partial block at each end.
 * A writer that handled only the easy aligned case would pass a gentler test
 * and corrupt this one.
 */
#define	SELFTEST_PATH	"/var/db/big.txt"
#define	SELFTEST_OFF	4090		/* 6 bytes before the boundary */
#define	SELFTEST_LEN	12		/* ...and 6 bytes past it      */
#define	SELFTEST_CTX	32		/* window read back around it  */
#define	SELFTEST_PAD	10		/* SELFTEST_OFF - window start */

/*
 * Left at offset 0 on purpose, and read on the next boot.  Reading back what
 * we just wrote proves the write path is self-consistent; finding it after a
 * power cycle proves it reached the platter, which is the only claim that
 * actually matters and the only one a cache cannot fake.
 */
/*
 * The text used to read "style9 wrote this in place", which was true and is
 * the thing the write path stopped doing: bytes are copied to fresh blocks
 * now, so that the checkpoint behind this one keeps the contents it described.
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

	rv = fs_open(SELFTEST_PATH, &h);
	if (rv != FS_E_OK) {
		kprintf("apfs-write: %s absent (rv=%d) -- self-test skipped\n",
		    SELFTEST_PATH, rv);
		return;
	}
	if (h.fh_size < SELFTEST_OFF + SELFTEST_CTX) {
		kprintf("apfs-write: %s too small -- self-test skipped\n",
		    SELFTEST_PATH);
		return;
	}

	if (fs_stat(SELFTEST_PATH, &st0) != FS_E_OK) {
		kprintf("apfs-write: FAIL cannot stat before\n");
		return;
	}

	/*
	 * A write that STARTS past the end must be refused outright.  Not
	 * clamped and not grown into: the bytes between would belong to no
	 * extent, which is a sparse file rather than a longer one, and a short
	 * write reported as success is how a file ends up half-updated with
	 * the caller told everything went in.  A write that merely runs off
	 * the end is a different matter and now lengthens the file; see
	 * fs_apfs_grow and the growth self-test.
	 */
	rv = fs_pwrite(&h, h.fh_size + 4096, (const uint8_t *)marker, 8, &put);
	if (rv != FS_E_NOALLOC) {
		kprintf("apfs-write: FAIL a write starting past the end was "
		    "not refused (rv=%d)\n", rv);
		return;
	}

	/* The window as it stands, so we can put it back and check neighbours. */
	rv = fs_pread(&h, SELFTEST_OFF - SELFTEST_PAD, save, SELFTEST_CTX, &got);
	if (rv != FS_E_OK || got != SELFTEST_CTX) {
		kprintf("apfs-write: FAIL pre-read (rv=%d got=%u)\n", rv, got);
		return;
	}

	for (i = 0; i < SELFTEST_LEN; i++)
		pat[i] = (uint8_t)('A' + i);

	rv = fs_pwrite(&h, SELFTEST_OFF, pat, SELFTEST_LEN, &put);
	if (rv != FS_E_OK || put != SELFTEST_LEN) {
		kprintf("apfs-write: FAIL write (rv=%d put=%u)\n", rv, put);
		return;
	}

	rv = fs_pread(&h, SELFTEST_OFF - SELFTEST_PAD, back, SELFTEST_CTX, &got);
	if (rv != FS_E_OK || got != SELFTEST_CTX) {
		kprintf("apfs-write: FAIL read-back (rv=%d got=%u)\n", rv, got);
		return;
	}
	if (!same(back + SELFTEST_PAD, pat, SELFTEST_LEN)) {
		kprintf("apfs-write: FAIL written bytes differ\n");
		return;
	}
	/*
	 * The half of this that matters.  Both ends of the write land inside
	 * a block that is mostly not ours, and the read-modify-write is what
	 * keeps the rest of it.  Damage here would sit outside the range
	 * anyone thinks to check, which is exactly why it is checked.
	 */
	if (!same(back, save, SELFTEST_PAD) ||
	    !same(back + SELFTEST_PAD + SELFTEST_LEN,
	    save + SELFTEST_PAD + SELFTEST_LEN,
	    SELFTEST_CTX - SELFTEST_PAD - SELFTEST_LEN)) {
		kprintf("apfs-write: FAIL neighbouring bytes clobbered\n");
		return;
	}

	/* Put it back, and prove the restore too. */
	rv = fs_pwrite(&h, SELFTEST_OFF, save + SELFTEST_PAD, SELFTEST_LEN,
	    &put);
	if (rv != FS_E_OK) {
		kprintf("apfs-write: FAIL restore (rv=%d)\n", rv);
		return;
	}
	rv = fs_pread(&h, SELFTEST_OFF - SELFTEST_PAD, back, SELFTEST_CTX, &got);
	if (rv != FS_E_OK || !same(back, save, SELFTEST_CTX)) {
		kprintf("apfs-write: FAIL restore did not restore\n");
		return;
	}

	if (fs_stat(SELFTEST_PATH, &st1) != FS_E_OK) {
		kprintf("apfs-write: FAIL cannot stat after\n");
		return;
	}
	if (st1.fs_mtime_ns <= st0.fs_mtime_ns) {
		kprintf("apfs-write: FAIL mtime did not move (%llu -> %llu)\n",
		    (unsigned long long)st0.fs_mtime_ns,
		    (unsigned long long)st1.fs_mtime_ns);
		return;
	}

	/*
	 * A second handle to the same file, opened BEFORE the write above and
	 * therefore carrying a generation the write has since left behind.
	 * Reading through it must notice.  Nothing observable changes yet --
	 * this rung cannot alter a length -- so what is checked is that the
	 * mechanism fires at all, which is exactly the part that would rot
	 * unnoticed between now and the rung that needs it.
	 */
	{
		struct fs_handle	stale;
		uint64_t		n0;
		uint8_t			one;

		n0 = fs_n_stale;
		if (fs_open(SELFTEST_PATH, &stale) != FS_E_OK) {
			kprintf("apfs-write: FAIL second open\n");
			return;
		}
		rv = fs_pwrite(&h, SELFTEST_OFF, save + SELFTEST_PAD,
		    SELFTEST_LEN, &put);
		if (rv != FS_E_OK) {
			kprintf("apfs-write: FAIL write before staleness "
			    "check (rv=%d)\n", rv);
			return;
		}
		if (fs_pread(&stale, 0, &one, 1, &got) != FS_E_OK) {
			kprintf("apfs-write: FAIL read through stale handle\n");
			return;
		}
		if (fs_n_stale == n0) {
			kprintf("apfs-write: FAIL a handle older than the "
			    "volume was not noticed\n");
			return;
		}
	}

	/* The marker, and what it says about a previous boot. */
	rv = fs_pread(&h, 0, mark, (uint32_t)sizeof(mark), &got);
	if (rv != FS_E_OK || got != sizeof(mark)) {
		kprintf("apfs-write: FAIL marker read (rv=%d)\n", rv);
		return;
	}
	if (same(mark, (const uint8_t *)marker, sizeof(mark))) {
		kprintf("apfs-write: PASS -- and the marker at %s:0 is still "
		    "there from an earlier boot\n", SELFTEST_PATH);
		return;
	}
	rv = fs_pwrite(&h, 0, (const uint8_t *)marker, (uint32_t)sizeof(mark),
	    &put);
	if (rv != FS_E_OK || put != sizeof(mark)) {
		kprintf("apfs-write: FAIL marker write (rv=%d put=%u)\n", rv,
		    put);
		return;
	}
	rv = fs_pread(&h, 0, mark, (uint32_t)sizeof(mark), &got);
	if (rv != FS_E_OK || !same(mark, (const uint8_t *)marker, sizeof(mark))) {
		kprintf("apfs-write: FAIL marker read-back\n");
		return;
	}
	kprintf("apfs-write: PASS -- marker written at %s:0; a reboot should "
	    "find it\n", SELFTEST_PATH);
}

/*
 * A FILE GETS LONGER, STAYS LONGER, AND FILLS A NODE DOING IT
 *
 * Three claims, and the third is why this appends more than once.  A file
 * extent record costs 48 bytes of the leaf it lands in and this container's
 * leaf had room for four, so a test that appended once would leave the
 * interesting case -- a node with no room -- permanently untested.  A block
 * per append reaches it in five.
 *
 * A block per append and not a byte: the length is always rounded up to a
 * block by the allocation behind it, so a whole block is the smallest append
 * guaranteed to need a new run, and a new run is what needs a new record.
 *
 * Bounded by the FILE rather than by a counter this kernel would have to keep:
 * the loop stops once the file is at least SELFTEST_GROW_TO bytes.  That used
 * to mean the growing happened once, ever, because there was no truncate and a
 * test that grew the file every boot would grow it without end.  There is one
 * now, it runs first, and it leaves the file at the length it ships at -- so
 * this appends again on every boot, and the whole path is exercised every time
 * rather than once in the life of an image.
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
	rounds = 0;

	while (h.fh_size < SELFTEST_GROW_TO) {
		at = h.fh_size;
		rv = fs_pwrite(&h, at, chunk, GROW_CHUNK, &put);
		/*
		 * A REFUSAL THE WRITER DOCUMENTS IS NOT A FAILURE OF IT.  The
		 * records of one file need not share a leaf, and the more the
		 * tree splits the less likely it is that they do; an edit that
		 * can move one node at a time says so and changes nothing.
		 * Calling that FAIL teaches the reader to ignore the word.
		 */
		if (rv == FS_E_SPREAD) {
			kprintf("apfs-grow: %s keeps its bytes and its inode "
			    "in different leaves -- appending across two is a "
			    "different rung; skipped after %u round(s)\n",
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
		 * Read back through the file, not out of what was written: the
		 * bytes have been through an allocation, a zeroing, a record
		 * insert and a checkpoint since.
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
		 * Nothing was appended, so the file arrived at this boot long
		 * enough already -- which now means the truncate test did not
		 * run, since it leaves the file at the length it ships at.
		 * Saying so is better than reporting a pass for a path that
		 * was not taken; the claim that the length outlives the
		 * machine is checked where it can be, in fs_trunc_selftest,
		 * against the tail this test wrote on the boot before.
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
	 * THE THIRD CLAIM, and it is not the one this test was written with.
	 * The plan was that six appends would fill a leaf and force a split;
	 * what the boot showed instead was six appends and no split at all,
	 * because the allocator kept handing back the blocks immediately after
	 * the file's last run and two runs that touch are ONE run.  That is the
	 * better answer -- a record per appended block, in each of two trees,
	 * is how the extent reference tree filled at sixteen -- so the claim
	 * became the one the code actually makes.
	 *
	 * The split still has to be proved, and is, by asking for one outright:
	 * see fs_apfs_split_selftest.
	 *
	 * All but the FIRST, and that exception arrived with the truncate test.
	 * It ran a moment ago and gave back the blocks immediately past this
	 * file's run; the free queue holds them for the checkpoints that still
	 * name them, so the first append cannot be a continuation of anything
	 * and is given a record of its own.  Every append after it merges into
	 * that.  The claim is written as "all but one" rather than "all"
	 * because the one that cannot merge is not a defect -- it is the queue
	 * doing the only thing that makes an abandoned checkpoint safe.
	 */
	merges = fs_apfs_merges() - merges;
	if (merges + 1 < rounds) {
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
	kfree(chunk);
	kfree(back);
}

/*
 * A FILE GETS SHORTER, AND A RECORD LEAVES A TREE
 *
 * Two halves, and only the second is hard.  Cutting inside a run shortens a
 * length in place; cutting away a run entirely takes its record out of two
 * B-trees, which is the first thing this kernel does that makes a tree
 * smaller.  A test that only ever did the first would pass on a truncate that
 * could not do the second at all.
 *
 * So it MAKES both happen rather than hoping the file is shaped right:
 *
 *	cut to the shipped length	-- undoes what the growth test appended
 *	append two blocks		-- one run, one allocation, one record
 *	cut away one of them		-- lands INSIDE that run: a shortening
 *	append one block		-- CANNOT continue the run, because the
 *					   block it would continue into was
 *					   given back one checkpoint ago and the
 *					   free queue is still holding it, so it
 *					   gets a record of its own
 *	cut it away			-- a whole run past the end: a dropping
 *	cut back to the shipped length	-- leaves the file for the growth test
 *
 * The fourth step is the one worth reading twice.  It is deterministic not
 * because the allocator was asked for a fresh run but because it was asked for
 * the OLD one and refused: the free queue exists precisely so that a block an
 * abandoned checkpoint still names cannot be handed out, and that refusal is
 * what makes a second record certain here.
 *
 * On the way in it checks the tail of the file it was given, which the boot
 * before appended.  That is the growth test's persistence claim, checked here
 * because this is the last moment it is true.
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
		 * What the boot before left, checked before it is thrown away:
		 * the file is longer than it ships because the growth test
		 * appended to it, and its last block should still read as what
		 * was appended.
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
	 * A REFUSAL THE WRITER DOCUMENTS IS NOT A FAILURE OF IT.  A cut moves
	 * the file's extent records and its inode record together, in one copy
	 * of one node, and a tree that has been splitting for a while stops
	 * keeping them in the same one.  The writer says so and changes
	 * nothing; reporting FAIL for it is how a suite trains its reader to
	 * stop believing the word.
	 */
	if (rv == FS_E_SPREAD) {
		kprintf("apfs-trunc: %s no longer keeps its runs and its "
		    "inode in one leaf -- cutting across two is a different "
		    "rung; skipped\n", SELFTEST_PATH);
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

	/*
	 * Reading AT the new end gives nothing.  Not an error and not a short
	 * read of stale bytes: the file stops there, and a truncate that moved
	 * a length without moving the records would still answer this wrongly.
	 */
	rv = fs_pread(&h, SELFTEST_TRUNC_TO, back, GROW_CHUNK, &got);
	if (rv != FS_E_OK || got != 0) {
		kprintf("apfs-trunc: FAIL reading at the new end gave %u "
		    "bytes (rv=%d) -- the file did not stop where it says\n",
		    (unsigned)got, rv);
		goto out;
	}

	/*
	 * And the volume agrees, about the length AND about the blocks.  The
	 * second is the one that catches a truncate which edited a number and
	 * left the space behind it spoken for.
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
	 * One block back.  It cannot continue the run it follows: the block
	 * immediately past that run was given back by the cut above, and the
	 * free queue holds it for the checkpoints that still name it, so the
	 * allocator has to go elsewhere and the file gets a SECOND record.
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

	/*
	 * Through all of that, the bytes below the cut never moved.  Read
	 * through the file rather than compared to what was written: they have
	 * been past four truncations, two allocations and five checkpoints
	 * since anybody looked at them.
	 */
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
	kfree(edge);
	kfree(back);
	kfree(chunk);
}

/*
 * The file this one MAKES, in a directory small enough that counting its
 * entries is a real check rather than a loop.
 *
 * It is left behind on purpose, and the next boot is what turns this from a
 * self-consistency check into a claim about the disk: a create that never
 * reached the platter answers every question in this function perfectly.
 */
#define	SELFTEST_MADE_DIR	"/etc"
#define	SELFTEST_MADE		"/etc/made.txt"
#define	SELFTEST_MADE_MARK	"style9 made this file from nothing.\n"

/*
 * Take every name out of a directory, so a fixture somebody else has written
 * into can still be used.  Always removes entry ZERO rather than walking an
 * index up: the listing is a live view of a tree that this loop is editing,
 * and an index into it stops meaning what it meant the moment one goes.
 *
 * Returns false having said why -- a directory that will not empty is a bug in
 * the writer, which is exactly the thing this must not swallow.
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
		 * What the boot before made, and wrote, and left.  This is the
		 * whole claim: nothing in this function proves a create reached
		 * the disk except finding one that a power cycle ago did.
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
			return;
		}
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
	 * And make it.  The hole counter is read across this, not around the
	 * whole test: when there was a file to remove, the delete just above
	 * left holes exactly the size this create wants, and an insert that
	 * ignored them would take the room from a span that never grows back.
	 */
	holes = fs_apfs_holes();
	ino   = 0;
	rv = fs_create(SELFTEST_MADE, &ino);
	/*
	 * A leaf with no room refuses, and a create does not split and retry
	 * the way growing a file does -- it puts records into two leaves at
	 * once, and a split would move both of them and everything above.  That
	 * is the create rung's own documented edge, and it clears itself: the
	 * split test runs after this one and makes room, so the next boot gets
	 * on with it.  Reporting a refusal the writer explains as FAIL is how a
	 * suite teaches its reader to stop believing the word.
	 */
	if (rv == FS_E_NOALLOC) {
		kprintf("apfs-make: the leaf that would hold %s is full, and a "
		    "create that splits and retries is a different rung; "
		    "skipped\n", SELFTEST_MADE);
		return;
	}
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
	rv = fs_create(SELFTEST_MADE, NULL);
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
	 * Bytes into a file that has none: the first write to a file this
	 * kernel made, which is the whole point of making one.  It goes
	 * through the growth path, so the file gets its first block, its
	 * first extent record and its first owner.
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
		return;
	}
	if (fs_pread(&h, 0, back, marklen, &got) != FS_E_OK ||
	    got != marklen || !same(back, (const uint8_t *)mark, marklen)) {
		kprintf("apfs-make: FAIL %s does not read back what was just "
		    "written into it\n", SELFTEST_MADE);
		return;
	}
	if (fs_stat(SELFTEST_MADE, &st) != FS_E_OK || st.fs_size != marklen) {
		kprintf("apfs-make: FAIL %s does not say it is %u bytes\n",
		    SELFTEST_MADE, (unsigned)marklen);
		return;
	}

	kprintf("apfs-make: PASS -- %s made as inode %llu, %u bytes written "
	    "into a file that had none, %d names in %s%s\n", SELFTEST_MADE,
	    (unsigned long long)ino, (unsigned)marklen, after,
	    SELFTEST_MADE_DIR,
	    had ? ", the one the boot before left having been read and "
	    "removed first" : "");
}

/*
 * THE DIRECTORY THIS ONE MAKES
 *
 * Same shape as the file above and for the same reason: it is left on the
 * volume, and the boot after is what turns a self-consistency check into a
 * claim about the disk.  A mkdir that never reached the platter answers every
 * question here perfectly, once.
 *
 * What this asks that the file could not: whether the record written is a
 * directory to the REST OF THE KERNEL and not just to apfsck.  A name is put
 * into it -- which means the reader descended into a directory that did not
 * exist an instant ago, and the writer keyed an entry under it -- and then
 * the removal is asked for while that name is still there, and must be
 * refused.  Refusals are checked against the writer's own counter rather than
 * against the error code, because an error is also what a half-done edit
 * answers; the counter says whether anything happened.
 */
#define	SELFTEST_DIRS		"/etc/madedir"
#define	SELFTEST_DIRS_SLASH	"/etc/madedir/"
#define	SELFTEST_DIRS_FILE	"/etc/madedir/inside.txt"

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
		/*
		 * What a power cycle ago made and left.
		 */
		if (!st.fs_is_dir || st.fs_size != 0) {
			kprintf("apfs-dirs: FAIL %s came back as %s of %llu "
			    "bytes -- wanted a directory of none\n",
			    SELFTEST_DIRS, st.fs_is_dir ? "a directory" :
			    "a file", (unsigned long long)st.fs_size);
			return;
		}
		/*
		 * IT MAY NOT COME BACK EMPTY, AND THAT IS NOT A FAILURE.
		 *
		 * This used to insist on empty, on the reasoning that the boot
		 * which made it put a name in and took the name out, so
		 * anything left meant a removal had not reached the disk.  That
		 * reasoning held for exactly as long as nothing but this test
		 * could write to the volume.  A person typing
		 * `> /etc/madedir/hi.txt` at a real Apple shell -- which this
		 * system now invites -- was enough to make the NEXT boot report
		 * a regression that had not happened, and a test that cries
		 * wolf about its own users is worse than one check short.
		 *
		 * So what survives the reboot is the claim worth making: the
		 * directory is still a directory, it still lists, and whatever
		 * it holds can still be taken out of it.  The names are cleared
		 * and counted out loud, because a directory that will not empty
		 * IS our bug.
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

	/* And make one.  A full leaf refuses here exactly as a create does. */
	count = fs_apfs_dirmakes();
	ino   = 0;
	rv = fs_mkdir(SELFTEST_DIRS, &ino);
	if (rv == FS_E_NOALLOC) {
		kprintf("apfs-dirs: the leaf that would hold %s is full, and a "
		    "mkdir that splits and retries is the same rung a create "
		    "is waiting on; skipped\n", SELFTEST_DIRS);
		return;
	}
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
	 * A name is taken once, whatever kind of thing took it -- and asking
	 * with the separator a directory is entitled to must reach the same
	 * name, not a different one.
	 */
	rv = fs_mkdir(SELFTEST_DIRS, NULL);
	if (rv != FS_E_EXIST) {
		kprintf("apfs-dirs: FAIL making %s a second time answered %d, "
		    "wanted %d\n", SELFTEST_DIRS, rv, FS_E_EXIST);
		return;
	}
	rv = fs_mkdir(SELFTEST_DIRS_SLASH, NULL);
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
	 * And a name INSIDE it, which is the part no amount of checking the
	 * record could stand in for: the lookup descends into a directory this
	 * kernel made a moment ago, the create keys an entry under its object
	 * id, and the removal must then refuse while that entry is there.
	 */
	rv = fs_create(SELFTEST_DIRS_FILE, NULL);
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

	kprintf("apfs-dirs: PASS -- %s made as inode %llu, a name made inside "
	    "it and taken back out, removal refused while it held one, %d "
	    "names in %s%s\n", SELFTEST_DIRS, (unsigned long long)ino, after,
	    SELFTEST_MADE_DIR,
	    had ? ", the directory the boot before left having been read and "
	    "removed first" : "");
}

/*
 * WHAT A SHELL LEFT ON THE DISK, ONE BOOT AGO
 *
 * Every other test here writes through this layer and reads back through it,
 * which proves the writer and proves nothing about who can reach it.  This one
 * cannot be satisfied by anything the kernel does: the file it looks for is
 * made by dash redirecting into it -- open(2) with O_CREAT, dup2 onto fd 1,
 * and the shell's own `echo` -- during the PREVIOUS boot, and by then this has
 * already run and found nothing.
 *
 * So the first boot skips, every boot after it checks, and what it checks is
 * that bytes a ring-3 program wrote survived a power cycle.  A write that
 * reached a cache and no further answers this with a missing file.
 *
 * The two lines are spelled out here and in user/hello.c, which is a
 * duplication on purpose: if the demo changes what it writes, this fails
 * loudly rather than quietly checking nothing.
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
		return;
	}
	if (rv != FS_E_OK) {
		kprintf("apfs-shell: FAIL cannot open %s (rv=%d)\n",
		    SELFTEST_SHELL, rv);
		return;
	}
	/*
	 * EMPTY IS A STATE THE DISK CAN HONESTLY BE IN, and it is worth
	 * naming rather than failing on.  A redirection is two events -- the
	 * file is created and emptied by open(2), then written -- and each
	 * closes its own checkpoint, so a machine switched off between them
	 * leaves exactly this: the name, with nothing in it.  That is the
	 * crash-consistent midpoint the checkpoint boundary exists to
	 * produce, not a write that went missing.  Anything ELSE in the file
	 * is a failure, because nothing but the shell writes here.
	 */
	if (h.fh_size == 0) {
		kprintf("apfs-shell: %s is there but empty -- a boot was "
		    "interrupted between the shell creating it and writing "
		    "into it, which is a state this volume is allowed to be "
		    "in; skipped\n", SELFTEST_SHELL);
		return;
	}
	if (h.fh_size != wantlen) {
		kprintf("apfs-shell: FAIL %s is %llu bytes and a shell wrote "
		    "%u\n", SELFTEST_SHELL, (unsigned long long)h.fh_size,
		    (unsigned)wantlen);
		return;
	}
	if (fs_pread(&h, 0, back, wantlen, &got) != FS_E_OK || got != wantlen) {
		kprintf("apfs-shell: FAIL %s will not read back (got %u of "
		    "%u)\n", SELFTEST_SHELL, (unsigned)got, (unsigned)wantlen);
		return;
	}
	if (!same(back, (const uint8_t *)want, wantlen)) {
		kprintf("apfs-shell: FAIL %s holds something other than what "
		    "a shell wrote into it\n", SELFTEST_SHELL);
		return;
	}
	kprintf("apfs-shell: PASS -- %u bytes a REAL Apple shell redirected "
	    "into %s in an earlier boot are still there, byte for byte\n",
	    (unsigned)wantlen, SELFTEST_SHELL);
}

/* As above, and about a node that is asked to run out of room. */
void
fs_split_selftest(void)
{

	if (!fs_apfs_ready())
		return;
	mutex_lock(&fs_lock);
	fs_apfs_split_selftest();
	mutex_unlock(&fs_lock);
}

/* As above, and about the same file every other write test uses. */
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
 * As above, and about a node that stops starting where its parent says.  The
 * clock is passed in because fs/apfs/apfs.c has none -- the same reason create and
 * unlink take a timestamp rather than reading one.
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

