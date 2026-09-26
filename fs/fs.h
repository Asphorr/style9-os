/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_FS_H_
#define	_SYS_FS_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Which filesystem answers a path.
 *
 * One disk, one volume: not a mount table or a VFS, just "who has the
 * files?", asked per call -- APFS if a container was found at boot, FAT
 * otherwise.  The Darwin syscall layer talks only to this, so neither
 * backend's error numbering or size width leaks into it.  A real VFS
 * (mount points, vnodes) would go at this seam.
 */

/*
 * Longest name reported (APFS allows 255 bytes, 8.3 needs 12).  The
 * structs below are the kernel<->libSystem wire format; user/libsystem.c
 * mirrors them and repeats the static asserts.
 */
#define	FS_NAME_MAX	256

/*
 * One directory entry.  Sizes and inode numbers are 64-bit, as in APFS
 * and in the macOS ABI they end up in ($INODE64 stat, struct dirent).
 */
struct fs_dirent {
	uint64_t	fde_ino;
	uint64_t	fde_size;	/* byte length (0 for a directory) */
	uint8_t		fde_is_dir;
	char		fde_name[FS_NAME_MAX];
};

/*
 * The mode word's type field, as every Unix spells it: APFS reports these
 * bits off the disk, FAT synthesises them, and the Darwin path copies them
 * out unchanged.  Only the two types produced here are listed.
 */
#define	FS_S_IFMT	0170000
#define	FS_S_IFREG	0100000
#define	FS_S_IFDIR	0040000

#define	FS_ISDIR(m)	(((m) & FS_S_IFMT) == FS_S_IFDIR)
#define	FS_ISREG(m)	(((m) & FS_S_IFMT) == FS_S_IFREG)

/*
 * A file's or directory's metadata, without reading its contents.
 *
 * Timestamps are nanoseconds since the Unix epoch, APFS's unit; FAT's
 * two-second ticks since 1980 are converted.  Zero means the volume does
 * not record that time.
 *
 * fs_mode carries the type bits too, so it repeats fs_is_dir; fs_is_dir
 * is for callers that only need to know whether to descend.
 */
struct fs_statbuf {
	uint64_t	fs_size;
	uint64_t	fs_ino;
	uint64_t	fs_alloced;	/* bytes the volume actually spent  */
	uint64_t	fs_mtime_ns;	/* contents last written            */
	uint64_t	fs_atime_ns;	/* contents last read               */
	uint64_t	fs_ctime_ns;	/* inode last changed               */
	uint64_t	fs_btime_ns;	/* created ("birth")                */
	uint32_t	fs_nlink;
	uint32_t	fs_uid;
	uint32_t	fs_gid;
	uint16_t	fs_mode;	/* POSIX mode word, S_IF* included  */
	uint8_t		fs_is_dir;
};

_Static_assert(sizeof(struct fs_dirent) == 280,
    "fs_dirent is a wire format shared with user/libsystem.c");
_Static_assert(sizeof(struct fs_statbuf) == 72,
    "fs_statbuf is a wire format shared with user/libsystem.c");

#define	FS_E_OK		0
#define	FS_E_NOMOUNT	(-1)	/* nothing mounted            */
#define	FS_E_NOTFOUND	(-2)	/* no such path               */
#define	FS_E_IO		(-3)	/* the disk or the tree lied  */
#define	FS_E_NOMEM	(-4)	/* out of kernel heap         */
#define	FS_E_TOOBIG	(-5)	/* file too large to slurp    */
#define	FS_E_ROFS	(-6)	/* this volume cannot be written */
#define	FS_E_NOALLOC	(-7)	/* a change this writer does not make */
#define	FS_E_EXIST	(-8)	/* the name is already taken     */
#define	FS_E_ISDIR	(-9)	/* ...and what it names is a directory */
#define	FS_E_SPREAD	(-10)	/* the records are in more nodes than one
				   edit can move at once */
#define	FS_E_NOTDIR	(-11)	/* ...and what it names is NOT a directory */
#define	FS_E_NOTEMPTY	(-12)	/* a directory that still holds a name     */
/*
 * The request makes no sense: a name this volume cannot spell, a directory
 * moved inside itself.  Not FS_E_IO: EINVAL says ask differently, EIO says
 * the disk is wrong.
 */
#define	FS_E_INVAL	(-13)
/*
 * The kernel's table of open files is full (not the volume, not the heap):
 * close something.
 */
#define	FS_E_NOSPACE	(-14)
/*
 * A checkpoint being read through /.xid (below) has since been released
 * by the free queue, and its blocks may be reused.  Not FS_E_NOTFOUND: the
 * path was valid until moments ago.
 */
#define	FS_E_GONE	(-15)

/* Non-zero once some filesystem is mounted and can serve files. */
int		fs_ready(void);

/*
 * The published past, by name.  Every checkpoint whose blocks the free
 * queue still holds is a complete older volume on the platter
 * (fs/apfs/apfs.h).  /.xid/<N> is the volume as checkpoint N left it,
 * /.xid/<N>/etc/notes.txt that file as of then, and /.xid lists the
 * reachable checkpoints, oldest first.  The window slides as checkpoints
 * are written; a name under one that has slid out answers FS_E_GONE.
 *
 * Hidden as ZFS hides .zfs: the root's listing does not name it, so walkers
 * do not descend into copies of the volume.  /.xid/<N> reports that
 * checkpoint's root under a synthetic inode, matching its /.xid entry.
 *
 * All read-only: every change under /.xid answers FS_E_ROFS, and a handle
 * opened there refuses writes.  Such a handle carries its checkpoint
 * number and every read resolves it again, so the window's edge is
 * enforced per read, not once at open.
 */
#define	FS_VIEW_DIR	"/.xid"

/*
 * The synthetic inode of /.xid (xid 0) and of /.xid/<N>.  Bit 63 is outside
 * the 60-bit object-id space an APFS record key can hold, so nothing on the
 * volume can collide with it.
 */
#define	FS_VIEW_INO(xid)	((uint64_t)1 << 63 | (uint64_t)(xid))

/*
 * Non-zero when `path' lies under /.xid and may not be changed.  open(2)
 * asks, so a write request is refused at the open, as POSIX has it.
 */
int		fs_readonly(const char *path);

/* "apfs", "fat", or "none" -- for banners and diagnostics. */
const char	*fs_kind(void);

/*
 * Read a whole file into a freshly kmalloc'd buffer (the caller kfree's it).
 * Returns FS_E_OK, or a negative FS_E_*.
 */
int		fs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size);

/*
 * A file, resolved.  Resolving a path is most of the cost of a read (one
 * tree walk per component, 936 us per page for the pager before handles
 * existed), so a handle keeps the answer: the backend's name for the
 * content (an APFS dstream id, a FAT starting cluster) and the length to
 * clamp ranged reads against.  No cursor; see fs_open for the hold.
 *
 * fh_size is a copy another writer can change.  So the volume has a
 * generation, bumped on every metadata change, and a handle records the
 * one it was made at; a ranged call on an older handle first refreshes
 * the length from the inode (fh_ino, no path needed).  An unwritten
 * volume pays one comparison.
 */
#define	FS_HANDLE_NONE	0
#define	FS_HANDLE_APFS	1
#define	FS_HANDLE_FAT	2

struct fs_handle {
	uint64_t	fh_id;		/* the backend's name for the bytes */
	uint64_t	fh_size;
	/*
	 * The inode, which is not fh_id: APFS keys extents on a dstream id
	 * and the inode record on the object id, equal only until a hard
	 * link.  A write needs both, to find the bytes and to stamp the time.
	 * Zero where the backend has no such notion.
	 */
	uint64_t	fh_ino;
	/* The volume generation fh_size was read at (see above). */
	uint64_t	fh_gen;
	/*
	 * The published checkpoint this handle reads, or 0 for the live
	 * volume.  A handle onto the past has no open-table row and no
	 * generation to refresh; fs_pread checks the window on every read.
	 */
	uint64_t	fh_xid;
	uint8_t		fh_kind;	/* FS_HANDLE_*                      */
};

/*
 * Resolve `path' to a handle.  Directories are refused.  Returns FS_E_OK,
 * or a negative FS_E_*.
 *
 * A live APFS handle is a hold on the file, so unlink can tell a name in
 * use from a free one: every handle must be given back with fs_close, and
 * every copy (fork, dup) announced with fs_hold.  FS_E_NOSPACE: the table
 * of held files is full.
 */
int		fs_open(const char *path, struct fs_handle *out);

/* One more holder of an open file (dup, fork).  No path lookup. */
int		fs_hold(const struct fs_handle *h);

/*
 * Give a handle back and empty it.  When the last holder of a file whose
 * name was unlinked lets go, the file is reaped here (see fs_unlink).
 * Harmless on a zeroed or non-disk handle.
 */
int		fs_close(struct fs_handle *h);

/*
 * Reap every nameless file an interrupted boot left waiting.  Called once
 * from kmain after the mount, before anything opens a file.
 */
int		fs_reap_orphans(void);

/*
 * Read at most `len' bytes of a resolved file at offset `off' into `buf',
 * the count delivered in *out_got.  A read at or past end-of-file returns
 * FS_E_OK with zero bytes, as pread(2) does; one running off the end is
 * short.  Backs the pager (vm/vm_object.c) and read(2).
 */
int		fs_pread(struct fs_handle *h, uint64_t off, uint8_t *buf,
		    uint32_t len, uint32_t *out_got);

/*
 * Write `len' bytes of a resolved file at offset `off', the count written
 * in *out_put, and stamp the modification time (here, so no caller can
 * forget it).  A write running past the end grows the file first; one
 * starting beyond the end would leave a hole and returns FS_E_NOALLOC,
 * changing nothing.  A backend that cannot write returns FS_E_ROFS.
 */
int		fs_pwrite(struct fs_handle *h, uint64_t off,
		    const uint8_t *buf, uint32_t len, uint32_t *out_put);

/*
 * Set a resolved file's length, either way, and stamp it.  Shorter: runs
 * past the new end are shortened or their records removed, and their
 * blocks go back.  Longer: the write path's growth, so the new bytes read
 * as zeroes (the allocator hands out zeroed blocks), as POSIX requires.
 * The handle's length and the volume generation move, so other handles
 * notice.  A backend that cannot write answers FS_E_ROFS.
 */
int		fs_truncate(struct fs_handle *h, uint64_t new_size);

/*
 * Make and unmake files and directories.
 *
 * fs_create makes an empty regular file and reports the inode number the
 * volume gave it.  An existing name answers FS_E_EXIST; create is not
 * create-or-replace.
 *
 * fs_unlink removes the name and, usually, the file (this kernel makes no
 * hard links).  If something holds the file open, the name goes now and
 * the bytes when the last descriptor closes; in between the file lives in
 * the volume's private directory (APFS_PRIV_DIR_INO).  A directory
 * answers FS_E_ISDIR.
 *
 * fs_mkdir and fs_rmdir do the same for a directory.  fs_rmdir answers
 * FS_E_NOTDIR for a non-directory and FS_E_NOTEMPTY for one that still
 * holds a name.  These two accept a trailing separator ("/tmp/x/"); the
 * file calls refuse it.
 *
 * All four answer FS_E_ROFS on a backend that cannot write, and leave the
 * checkpoint to the batching policy (fs.c): a crash before it takes back
 * the whole operation.
 */
int		fs_create(const char *path, uint16_t perm, uint64_t *ino_out);
int		fs_unlink(const char *path);
int		fs_mkdir(const char *path, uint16_t perm, uint64_t *ino_out);
int		fs_rmdir(const char *path);

/*
 * fs_rename: move a name, within a directory or between two, with whatever
 * is under it, over whatever stands at the destination (POSIX).  One
 * transaction, so a reader sees the old file or the new one, never half of
 * each.  A replaced file still open somewhere keeps its bytes until its
 * last descriptor closes, as an unlinked one does.
 *
 * FS_E_ISDIR / FS_E_NOTDIR when the two ends differ in kind, FS_E_NOTEMPTY
 * for a non-empty directory destination, FS_E_INVAL for a directory moved
 * inside itself.
 */
int		fs_rename(const char *opath, const char *npath);

/*
 * fs_chmod: set the permission bits of an existing name; only the low
 * twelve are taken.  FAT answers FS_E_ROFS: it has no mode to set.
 */
int		fs_chmod(const char *path, uint16_t mode);

/* Metadata for a path.  Returns FS_E_OK and fills *out, or a negative FS_E_*. */
int		fs_stat(const char *path, struct fs_statbuf *out);

/*
 * Fill *out with the `index'-th entry of a directory.  Returns 1 when an
 * entry was written, 0 at end-of-directory, or a negative FS_E_*.
 * Stateless: each call re-resolves and re-scans.
 */
int		fs_readdir(const char *path, uint32_t index,
		    struct fs_dirent *out);

/*
 * Boot self-test of the write path.  A read-back cannot prove a write
 * reached the disk, so it checks what a plausible-but-wrong writer gets
 * wrong -- the bytes around a partial-block write survive, a write
 * starting past the end is refused, the mtime moves -- and leaves a marker
 * for the next boot to find.  Skipped when no writable volume is mounted.
 */
void		fs_write_selftest(void);

/*
 * Close the volume's current transaction.  On APFS, write a checkpoint
 * (fs/apfs/apfs.h): the container is the old one entire until the last
 * block lands and the new one after.  Mutations batch (see FS_SYNC_MS in
 * fs.c); this is the call that means now, what fsync(2) becomes.  A volume
 * with nothing owed succeeds without writing.  On FAT there is nothing to
 * close, and it succeeds.  Returns FS_E_OK or a negative FS_E_*.
 */
int		fs_sync(void);

/*
 * Start the syncer, the kernel thread that publishes a dirty volume every
 * FS_SYNC_MS, bounding what a crash loses in time as well as in edits.
 * Called once from kmain after the self-tests and their closing sync, so
 * no second checkpoint writer appears mid-test.  Does nothing unless the
 * volume is APFS.
 */
void		fs_syncer_start(void);

/*
 * Boot self-tests.  Each does nothing unless the volume is APFS; the
 * backend tests run under fs_lock.
 *
 *	fs_ckpt_selftest	the checkpoint writer
 *	fs_data_selftest	writing a file moves its bytes: the run the
 *				live checkpoint names still reads as it did
 *	fs_trunc_selftest	a file gets shorter: the length moves, bytes
 *				below the cut stay, blocks come back, a run
 *				wholly past the end loses its record.  Runs
 *				before fs_grow_selftest and first checks the
 *				tail the previous boot's growth appended
 *	fs_grow_selftest	a file gets longer and stays longer
 *	fs_make_selftest	a file is made and unmade across boots: finds
 *				and removes the previous boot's file, makes it
 *				again and leaves it, reusing the node space
 *				the removal gave back
 *	fs_dirs_selftest	the same for a directory, which must also
 *				hold a name made inside it and refuse removal
 *				while it does; left empty for the next boot
 *	fs_shell_selftest	a file a ring-3 shell wrote in the previous
 *				boot survived; the first boot skips
 *	fs_split_selftest	a B-tree node splits without losing a record
 *	fs_index_selftest	the parent index is corrected when a node's
 *				first record is deleted (apfsck rejects
 *				otherwise); arranged on purpose
 *	fs_drop_selftest	an emptied node leaves the tree: parent, omap,
 *				block and tree counts all follow
 *	fs_stream_selftest	a file whose inode and dstream records a split
 *				put in different nodes is still removed whole
 *	fs_room_selftest	a create in a full leaf splits it and succeeds
 *	fs_extref_selftest	apfs-extref: the extent-reference tree outgrows
 *				its root and appends are still answered
 *	fs_move_selftest	a name moves (within and across directories,
 *				longer and shorter) with its inode and bytes;
 *				refusals are checked too
 *	fs_orphan_selftest	apfs-orphan: a file kept whole only by an open
 *				descriptor (fs/apfs/apfs.h)
 *	fs_clobber_selftest	apfs-clobber, then fs-clobber: a rename onto a
 *				held name; the holder still reads the old file,
 *				which goes when it closes
 *	fs_view_selftest	/.xid: earlier bytes by name, the listing,
 *				writes refused, a handle falling out of the
 *				window.  After every writer above
 *	fs_seek_selftest	a lookup by key answers what a full walk does
 *	fs_kill_selftest	a task killed mid-write surfaces with fs_lock
 *				returned (th_mutex_depth, kern/thread.h) and
 *				the volume answers after.  Runs late in boot
 */
void		fs_ckpt_selftest(void);
void		fs_data_selftest(void);
void		fs_grow_selftest(void);
void		fs_trunc_selftest(void);
void		fs_make_selftest(void);
void		fs_dirs_selftest(void);
void		fs_shell_selftest(void);
void		fs_split_selftest(void);
void		fs_index_selftest(void);
void		fs_drop_selftest(void);
void		fs_stream_selftest(void);
void		fs_room_selftest(void);
void		fs_extref_selftest(void);
void		fs_move_selftest(void);
void		fs_orphan_selftest(void);
void		fs_clobber_selftest(void);
void		fs_view_selftest(void);

/*
 * fs-open: nothing the self-tests opened is still held.  Run after the
 * self-tests that open files and before ring 3 starts; a leaked hold would
 * otherwise turn a later unlink into an orphaning.
 */
void		fs_open_check(void);

void		fs_seek_selftest(void);
void		fs_kill_selftest(void);

/*
 * Print the batching tally, the volume generation with how many handles
 * were found stale and how many had really changed length, and the
 * orphan/reap counts.
 */
void		fs_handle_stats(void);

#endif /* !_SYS_FS_H_ */
