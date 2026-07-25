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
 * allocator, which the APFS writer does not have (see fs/fs_apfs.h for why
 * that boundary is where it is), and both return FS_E_NOALLOC having changed
 * nothing.  A backend with no write support at all returns FS_E_ROFS, which
 * is a different answer and means a different thing: not "this write was too
 * ambitious" but "not on this volume".
 */
int		fs_pwrite(struct fs_handle *h, uint64_t off,
		    const uint8_t *buf, uint32_t len, uint32_t *out_put);

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
 * The volume generation, and what it has caught: how many handles were older
 * than the volume when used, and how many of those had a length that really
 * had moved.  The first number moving proves the check is alive; the second
 * stays at zero until files can grow.
 */
void		fs_handle_stats(void);

#endif /* !_SYS_FS_H_ */
