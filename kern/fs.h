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

/* A file-or-directory's metadata, without reading it. */
struct fs_statbuf {
	uint64_t	fs_size;
	uint64_t	fs_ino;
	uint8_t		fs_is_dir;
};

_Static_assert(sizeof(struct fs_dirent) == 280,
    "fs_dirent is a wire format shared with user/libsystem.c");
_Static_assert(sizeof(struct fs_statbuf) == 24,
    "fs_statbuf is a wire format shared with user/libsystem.c");

#define	FS_E_OK		0
#define	FS_E_NOMOUNT	(-1)	/* nothing mounted            */
#define	FS_E_NOTFOUND	(-2)	/* no such path               */
#define	FS_E_IO		(-3)	/* the disk or the tree lied  */
#define	FS_E_NOMEM	(-4)	/* out of kernel heap         */
#define	FS_E_TOOBIG	(-5)	/* file too large to slurp    */

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
 * -- no cursor, no reference count, nothing to close.  It stays valid as long
 * as the file does, which on a read-only volume is forever.
 */
#define	FS_HANDLE_NONE	0
#define	FS_HANDLE_APFS	1
#define	FS_HANDLE_FAT	2

struct fs_handle {
	uint64_t	fh_id;		/* the backend's name for the bytes */
	uint64_t	fh_size;
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
int		fs_pread(const struct fs_handle *h, uint64_t off, uint8_t *buf,
		    uint32_t len, uint32_t *out_got);

/* Metadata for a path.  Returns FS_E_OK and fills *out, or a negative FS_E_*. */
int		fs_stat(const char *path, struct fs_statbuf *out);

/*
 * Fill *out with the `index`-th entry of a directory.  Returns 1 when an entry
 * was written, 0 at end-of-directory, or a negative FS_E_*.  Stateless: each
 * call re-resolves and re-scans, so the kernel keeps no per-fd cursor.
 */
int		fs_readdir(const char *path, uint32_t index,
		    struct fs_dirent *out);

#endif /* !_SYS_FS_H_ */
