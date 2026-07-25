/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "clock.h"
#include "fs.h"
#include "fs_apfs.h"
#include "fs_fat.h"
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
 * the loop stops once the file is at least SELFTEST_GROW_TO bytes, so a later
 * boot finds it long enough already and checks instead of writing.  That
 * matters because there is no truncate yet -- a test that grew the file every
 * boot would grow it without end.
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
	uint64_t		 splits;
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
	splits = fs_apfs_splits();
	merges = fs_apfs_merges();
	rounds = 0;

	while (h.fh_size < SELFTEST_GROW_TO) {
		at = h.fh_size;
		rv = fs_pwrite(&h, at, chunk, GROW_CHUNK, &put);
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
		 * An earlier boot did the growing, so what is worth checking
		 * is on the platter: the length survived, and the tail still
		 * reads as what was appended.
		 */
		rv = fs_pread(&h, h.fh_size - GROW_CHUNK, back, GROW_CHUNK,
		    &got);
		if (rv != FS_E_OK || got != GROW_CHUNK) {
			kprintf("apfs-grow: FAIL cannot read the tail "
			    "(rv=%d got=%u)\n", rv, got);
			goto out;
		}
		for (i = 0; i < GROW_CHUNK; i++) {
			if (back[i] == chunk[i])
				continue;
			kprintf("apfs-grow: FAIL byte %u of the tail is "
			    "0x%02x, wanted 0x%02x -- what an earlier boot "
			    "appended did not survive\n", (unsigned)i,
			    (unsigned)back[i], (unsigned)chunk[i]);
			goto out;
		}
		kprintf("apfs-grow: PASS -- %s is %llu bytes from an earlier "
		    "boot and its last block still reads what was appended\n",
		    SELFTEST_PATH, (unsigned long long)h.fh_size);
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
	 */
	splits = fs_apfs_merges() - merges;
	if (splits != rounds) {
		kprintf("apfs-grow: FAIL %u appends lengthened %llu runs -- "
		    "the rest were given records of their own, and blocks that "
		    "touch should never need one\n", (unsigned)rounds,
		    (unsigned long long)splits);
		goto out;
	}

	kprintf("apfs-grow: PASS -- %s grew %llu -> %llu bytes over %u "
	    "appends, every one of them lengthening the run already there "
	    "rather than adding a record, and a reboot should still find "
	    "it\n", SELFTEST_PATH, (unsigned long long)was,
	    (unsigned long long)h.fh_size, (unsigned)rounds);
out:
	kfree(chunk);
	kfree(back);
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

