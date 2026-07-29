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
 * There is one disk and one volume on it, so this is not a mount table and
 * not a VFS: it is the single question "who has the files?", asked once per
 * call.  APFS answers if a container was found at boot, FAT otherwise.  The
 * Darwin syscall layer talks only to this, so the two readers stay
 * interchangeable and neither leaks its own error numbering or its own idea
 * of how wide a size is into the syscall path.
 *
 * A real VFS -- mount points, vnodes, per-fd cursors -- is a different piece
 * of work.  This is the seam where it would go.
 */

/*
 * Longest name reported.  APFS allows 255 bytes; FAT 8.3 needs 12.  This is
 * also the kernel<->libSystem wire format, so user/libsystem.c mirrors these
 * structs exactly and the static asserts below are duplicated there: two hand
 * written copies of a layout are exactly the kind of thing that drifts.
 */
#define	FS_NAME_MAX	256

/*
 * One directory entry.  Sizes and inode numbers are 64-bit because APFS's
 * are, and because the macOS ABI these end up in ($INODE64 struct stat, struct
 * dirent) is 64-bit too -- narrowing here would only have to be widened again
 * on the other side of the syscall.
 */
struct fs_dirent {
	uint64_t	fde_ino;
	uint64_t	fde_size;	/* byte length (0 for a directory) */
	uint8_t		fde_is_dir;
	char		fde_name[FS_NAME_MAX];
};

/*
 * The mode word's type field, spelled the way every Unix spells it.  These
 * live in the neutral header because three layers share the vocabulary: APFS
 * reports these bits straight off the disk, FAT synthesises them from an
 * attribute byte, and the Darwin syscall path copies them out to a libSystem
 * that hands them to an Apple binary expecting exactly them.  Only the two
 * types this filesystem can produce are listed.
 */
#define	FS_S_IFMT	0170000
#define	FS_S_IFREG	0100000
#define	FS_S_IFDIR	0040000

#define	FS_ISDIR(m)	(((m) & FS_S_IFMT) == FS_S_IFDIR)
#define	FS_ISREG(m)	(((m) & FS_S_IFMT) == FS_S_IFREG)

/*
 * A file-or-directory's metadata, without reading it.
 *
 * This used to answer three questions -- how big, which inode, directory or
 * not -- because that is all any binary had asked.  `ls -l` asks the rest of
 * them, and the rest of them were being read off the disk and thrown away:
 * an APFS inode record carries the mode word, the link count, the owner and
 * group, and four timestamps in its FIXED part, so a stat that reported none
 * of it was paying for the tree walk and discarding the answer.
 *
 * Timestamps are nanoseconds since the Unix epoch, which is APFS's own unit;
 * a filesystem with a coarser clock (FAT counts two-second ticks since 1980)
 * converts on the way out rather than making every caller know.  Zero means
 * "this volume does not record that time" and is distinguishable from a real
 * 1970 timestamp only in theory, which is the usual bargain.
 *
 * fs_mode is a POSIX mode word with its type bits set, so it already says
 * what fs_is_dir says.  Both are here on purpose: mode is what a filesystem
 * that HAS permissions reports, fs_is_dir is what one that does not can
 * always answer, and a caller that only wants to know whether to descend
 * should not have to know which kind of volume replied.
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
#define	FS_E_NOALLOC	(-7)	/* the write would need a block allocator */
#define	FS_E_EXIST	(-8)	/* the name is already taken     */
#define	FS_E_ISDIR	(-9)	/* ...and what it names is a directory */
#define	FS_E_SPREAD	(-10)	/* the records are in more nodes than one
				   edit can move at once */
#define	FS_E_NOTDIR	(-11)	/* ...and what it names is NOT a directory */
#define	FS_E_NOTEMPTY	(-12)	/* a directory that still holds a name     */

/* Non-zero once some filesystem is mounted and can serve files. */
int		fs_ready(void);

/* "apfs", "fat", or "none" -- for banners and diagnostics. */
const char	*fs_kind(void);

/*
 * Read a whole file into a freshly kmalloc'd buffer (the caller kfree's it).
 * Returns FS_E_OK, or a negative FS_E_*.
 */
int		fs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size);

/*
 * A file, resolved.
 *
 * Reading by path means resolving the path, and resolving it is most of the
 * work: on APFS "/var/db/big.txt" costs one tree walk per component plus one
 * more for the inode, and only then does anything read a byte.  Doing that per
 * call is fine for a one-shot open; it is absurd for a pager, which asks for
 * the same file 4 KiB at a time.  Measured before this existed: 936 us per
 * page, nearly all of it re-answering a question already answered.
 *
 * So a handle is the answer to "which file", kept: the backend's own name for
 * the content (an APFS dstream id, a FAT starting cluster) plus the length,
 * which is what a ranged read needs to clamp against.  It is not an open file
 * -- no cursor, no reference count, nothing to close.
 *
 * It used to say here that a handle stays valid as long as the file does,
 * "which on a read-only volume is forever".  That sentence was true when it
 * was written and stopped being true the moment this filesystem could write:
 * fh_size is a COPY of a length that another writer can now change, and a
 * handle opened before a file grew would keep clamping reads to the old end
 * of it -- silently, since a short read is a legal answer.
 *
 * So the volume carries a generation number, bumped whenever any metadata
 * changes, and a handle remembers the generation it was made at.  A ranged
 * call whose handle predates the current generation refreshes its length from
 * the inode before using it.  The check costs a comparison; the refresh
 * happens only when something really did change, and needs no path -- fh_ino
 * names the inode record directly.  A volume nobody writes to therefore pays
 * exactly what it paid before.
 */
#define	FS_HANDLE_NONE	0
#define	FS_HANDLE_APFS	1
#define	FS_HANDLE_FAT	2

struct fs_handle {
	uint64_t	fh_id;		/* the backend's name for the bytes */
	uint64_t	fh_size;
	/*
	 * The file's inode, which is NOT fh_id: on APFS the extents are keyed
	 * on a dstream id while the metadata record is keyed on the object id,
	 * and the two are equal only until something is hard-linked.  A write
	 * needs both -- one to find the bytes, the other to stamp the time --
	 * so the handle carries both.  Zero where the backend has no such
	 * notion.
	 */
	uint64_t	fh_ino;
	/*
	 * The volume generation this handle's fh_size was read at.  Compared,
	 * not trusted: see the note above about the sentence that stopped
	 * being true.
	 */
	uint64_t	fh_gen;
	uint8_t		fh_kind;	/* FS_HANDLE_*                      */
};

/*
 * Resolve `path` to a handle.  Directories are refused: a handle is a thing
 * to read.  Returns FS_E_OK, or a negative FS_E_*.
 */
int		fs_open(const char *path, struct fs_handle *out);

/*
 * Read at most `len` bytes of a resolved file starting at byte offset `off`
 * into `buf`, writing the count actually delivered through *out_got.  A read
 * that starts at or past end-of-file is not an error -- it returns FS_E_OK
 * with zero bytes, the way pread(2) does -- and a read that runs off the end
 * is short.
 *
 * This is the ranged read the whole-file fs_slurp cannot be: it is what backs
 * the pager (kern/vm_object.c), which needs one 4 KiB page of a file and has
 * nowhere to put the rest of it.
 */
int		fs_pread(struct fs_handle *h, uint64_t off, uint8_t *buf,
		    uint32_t len, uint32_t *out_got);

/*
 * Overwrite `len` bytes of a resolved file at byte offset `off`, reporting the
 * count written through *out_put, and stamp the file's modification time.
 *
 * Stamping is done here rather than left to the caller because "the bytes
 * changed but the time did not" is not a state a filesystem should be able to
 * produce by forgetting a call.
 *
 * What this cannot do is grow a file or fill a hole: both need a block
 * allocator, which the APFS writer does not have (see fs/apfs/apfs.h for why
 * that boundary is where it is), and both return FS_E_NOALLOC having changed
 * nothing.  A backend with no write support at all returns FS_E_ROFS, which
 * is a different answer and means a different thing: not "this write was too
 * ambitious" but "not on this volume".
 */
int		fs_pwrite(struct fs_handle *h, uint64_t off,
		    const uint8_t *buf, uint32_t len, uint32_t *out_put);

/*
 * Set a resolved file's length, in either direction, and stamp it.
 *
 * Shorter is the interesting one: the runs past the new end are shortened or
 * their records removed outright, and their blocks go back.  Longer is the
 * same call the write path already makes when a write runs off the end, so
 * the new bytes read as zeroes -- which is what POSIX says an extending
 * truncate produces, and is here because the blocks are zeroed on the way out
 * of the allocator rather than because anybody wrote a special case.
 *
 * The handle's length is updated, and the volume's generation moves, so every
 * other handle open on the file finds out.  A backend that cannot write at all
 * answers FS_E_ROFS.
 */
int		fs_truncate(struct fs_handle *h, uint64_t new_size);

/*
 * Make a file, and unmake one.
 *
 * The two calls that change what a directory CONTAINS rather than what a file
 * holds, and the last thing between this filesystem and a volume programs could
 * live on: everything above works on a file that somebody else put there.
 *
 * fs_create makes an empty regular file and reports the inode number it was
 * given, which is the volume's and not this layer's invention.  It answers
 * FS_E_EXIST rather than truncating an existing file, because "create" and
 * "create or replace" are different requests and only the caller knows which
 * one it made.
 *
 * fs_unlink removes the name and, with it, the file: this kernel makes no hard
 * links, so the last name is the only name.  It answers FS_E_ISDIR for a
 * directory, which is fs_rmdir's business.
 *
 * fs_mkdir and fs_rmdir are the same two calls for a directory.  fs_rmdir
 * answers FS_E_NOTDIR for a name that is not one and FS_E_NOTEMPTY for one
 * that still holds a name: this removes a directory, it does not remove what
 * is in it, and doing both is a decision for whoever asked rather than for
 * this layer.  A directory being made or removed may have a trailing separator
 * ("/tmp/x/" names the same thing as "/tmp/x"), which the file calls refuse --
 * there the slash is a claim about the name that the call cannot honour.
 *
 * All four refuse a backend that cannot write with FS_E_ROFS, and all four
 * close a checkpoint on success, because a name and the inode under it are
 * several disk updates describing one event.
 */
int		fs_create(const char *path, uint16_t perm, uint64_t *ino_out);
int		fs_unlink(const char *path);
int		fs_mkdir(const char *path, uint16_t perm, uint64_t *ino_out);
int		fs_rmdir(const char *path);

/*
 * fs_chmod: set the permission bits of an existing name.  Only the low twelve
 * are taken; the type belongs to the file and not to the caller.  FAT answers
 * FS_E_ROFS -- its directory entry has no mode to set, and pretending would
 * leave a program unable to tell a chmod that worked from one that did not.
 */
int		fs_chmod(const char *path, uint16_t mode);

/* Metadata for a path.  Returns FS_E_OK and fills *out, or a negative FS_E_*. */
int		fs_stat(const char *path, struct fs_statbuf *out);

/*
 * Fill *out with the `index`-th entry of a directory.  Returns 1 when an entry
 * was written, 0 at end-of-directory, or a negative FS_E_*.  Stateless: each
 * call re-resolves and re-scans, so the kernel keeps no per-fd cursor.
 */
int		fs_readdir(const char *path, uint32_t index,
		    struct fs_dirent *out);

/*
 * Prove the write path against the mounted volume, at boot, out loud.
 *
 * A write is the one operation that cannot be checked by reading back what it
 * wrote: a cache that never reached the disk answers a read-back perfectly.
 * So this checks the things a plausible-but-wrong writer would get wrong --
 * that the bytes AROUND a partial-block write survived it, that a write which
 * would need an allocator is refused rather than truncated, that the file's
 * modification time moved -- and it leaves a marker behind, so the NEXT boot
 * can report finding it and thereby prove the bytes outlived the machine.
 *
 * Silently skipped when no writable volume is mounted.
 */
void		fs_write_selftest(void);

/*
 * Close the volume's current transaction, so that what has been written is
 * written as of a point in time rather than gradually.
 *
 * On APFS this writes a checkpoint (see fs/apfs/apfs.h); the container is the
 * old one entire until the last block lands and the new one entire
 * afterwards.  On FAT there is nothing to close -- every write there is
 * already final the moment it reaches the platter -- so this succeeds without
 * doing anything, which is the honest answer rather than a refusal.
 *
 * Returns FS_E_OK or a negative FS_E_*.
 */
int		fs_sync(void);

/*
 * Prove the checkpoint writer against the mounted container, at boot, out
 * loud.  Silently does nothing when the volume is not APFS.
 */
void		fs_ckpt_selftest(void);

/*
 * Prove that writing a file MOVES its bytes: the run the live checkpoint still
 * names must read as it did.  Silently does nothing when the volume is not
 * APFS.
 */
void		fs_data_selftest(void);

/*
 * Prove that a file can get longer and stay longer.  Grows the file back to
 * the length the truncate test cut it to on the previous boot, and checks the
 * bytes read back.  Silently does nothing when the volume is not APFS.
 */
void		fs_grow_selftest(void);

/*
 * Prove that a file can get SHORTER: the length moves, the bytes below the cut
 * are untouched, the blocks come back, and a run that ends up entirely past
 * the new end loses its record rather than being left behind describing
 * nothing.
 *
 * Runs BEFORE the growth test, and takes over its persistence claim on the way
 * past: the file it finds is the one the previous boot grew, so the tail it
 * checks before cutting is proof that growth reached the platter.
 *
 * Silently does nothing when the volume is not APFS, and says so and stops if
 * the file is already at its shipped length -- which is what the first boot
 * after a fresh image sees.
 */
void		fs_trunc_selftest(void);

/*
 * Prove that a file can be MADE, and unmade, and that doing both costs the
 * volume nothing it does not get back.
 *
 * Runs last of the write tests, and it is the one that spans boots by
 * construction: it looks for the file the boot before left, checks the bytes in
 * it, removes it, makes it again and leaves it there.  So every boot after the
 * first does the whole cycle -- create, write, reboot, find, read, unlink --
 * and the volume ends each one exactly as it started, which is the claim that
 * matters: a node's free span only ever shrinks, and a create/unlink pair that
 * did not reuse the room it gave back would run the volume out of names.
 *
 * Silently does nothing when the volume is not APFS.
 */
void		fs_make_selftest(void);

/*
 * The same claim for a DIRECTORY, and one the file above cannot make: that a
 * directory this kernel wrote is a directory to the rest of the kernel.  A
 * name is made inside one that did not exist an instant earlier -- so the
 * lookup descended into it and the writer keyed an entry under it -- and the
 * removal is then asked for while that name is still there and must refuse.
 * The directory is left empty on the volume for the boot after.
 *
 * Silently does nothing when the volume is not APFS.
 */
void		fs_dirs_selftest(void);

/*
 * Prove that a RING-3 program wrote to this volume and that the bytes outlived
 * the machine being turned off.
 *
 * The one test here the kernel cannot satisfy by itself: it looks for the file
 * a real Apple shell redirected into during the PREVIOUS boot, which is a
 * claim about open(2), write(2), the checkpoint under them, and the disk.  The
 * first boot on a fresh volume skips, saying so.
 */
void		fs_shell_selftest(void);

/*
 * Prove that a B-tree node can be split without losing a record.  Silently
 * does nothing when the volume is not APFS.
 */
void		fs_split_selftest(void);

/*
 * Prove that the index above a node is corrected when the node stops starting
 * where the index says it does -- which is what a delete of a node's first
 * record does, and which the checker rejects.  Arranged on purpose, because
 * waiting for it is not a test.  Silently does nothing when the volume is not
 * APFS.
 */
void		fs_index_selftest(void);

/*
 * Prove that a node which has lost its last record leaves the tree -- the
 * parent stops naming it, the object map forgets its oid, its block goes back,
 * and both counts that describe the tree follow.  Arranged on purpose, like
 * the one above.  Silently does nothing when the volume is not APFS.
 */
void		fs_drop_selftest(void);

/*
 * And that a file whose inode record and data stream record a split has put in
 * different nodes is still removed whole -- the failure it guards against is
 * an unlink that answered success and left half a file behind.  Silently does
 * nothing when the volume is not APFS.
 */
void		fs_stream_selftest(void);

/*
 * And that a create whose leaf has no room splits it and carries on, rather
 * than refusing for a reason that has nothing to do with the name it was
 * asked for.  The full leaf is arranged by filling one.  Silently does
 * nothing when the volume is not APFS.
 */
void		fs_room_selftest(void);

/*
 * And that finding a record by descending on its key answers exactly what
 * reading every record in the tree answers -- the same record, out of the same
 * leaf, with the same tail behind it.  Silently does nothing when the volume
 * is not APFS.
 */
void		fs_seek_selftest(void);

/*
 * The volume generation, and what it has caught: how many handles were older
 * than the volume when used, and how many of those had a length that really
 * had moved.  The first number moving proves the check is alive; the second
 * stays at zero until files can grow.
 */
void		fs_handle_stats(void);

#endif /* !_SYS_FS_H_ */
