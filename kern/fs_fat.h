/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_FS_FAT_H_
#define	_SYS_FS_FAT_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal read-only FAT12/16 filesystem.
 *
 * A real VFS rung: enough to OPEN a file by name off the ATA disk, READ its
 * bytes, STAT it, and ENUMERATE a directory -- so a Darwin binary's
 * fopen()/fread()/stat()/opendir() can pull data files (e.g. figlet fonts)
 * from storage instead of a baked-in blob, and a directory walker (tree(1))
 * can descend the volume.
 *
 * Names match by 8.3 short name (no long-name matching); a path descends real
 * subdirectories.  Two consumers want two behaviours, so the open/stat path
 * tries the literal path and FALLS BACK to the basename in the root directory:
 * that lets a binary's baked-in macOS path (whose leading directories do not
 * exist on our small disk) still find a font placed in the root, while a real
 * on-disk path resolves exactly.  No writes.  Block I/O goes straight to
 * ata_kread (no Mach round-trip from the kernel).
 */

/* Largest file the slurp path will read (bounds the kmalloc). */
#define	FS_FAT_MAX_FILE		(1u * 1024u * 1024u)

/* Longest display name fs_fat_readdir reports (8.3 needs 12; padded). */
#define	FS_FAT_NAME_MAX		64

#define	FS_FAT_E_OK		0
#define	FS_FAT_E_NOMOUNT	(-1)	/* no FAT volume mounted        */
#define	FS_FAT_E_NOTFOUND	(-2)	/* name absent / not a dir      */
#define	FS_FAT_E_IO		(-3)	/* block read failed            */
#define	FS_FAT_E_NOMEM		(-4)	/* kmalloc failed               */
#define	FS_FAT_E_TOOBIG		(-5)	/* file exceeds FS_FAT_MAX_FILE */
#define	FS_FAT_E_INVAL		(-6)	/* bad path / chain             */

/*
 * One directory entry, as fs_fat_readdir reports it.  This is also the wire
 * format copied out to libSystem's readdir (user/libsystem.c mirrors the
 * layout), so keep the field order stable.
 */
struct fs_fat_dirent {
	uint32_t	fde_ino;	/* stable inode (first cluster) */
	uint32_t	fde_size;	/* byte length (0 for a dir)    */
	uint8_t		fde_is_dir;	/* 1 if a subdirectory          */
	char		fde_name[FS_FAT_NAME_MAX];
};

/*
 * A file's metadata, as fs_fat_stat2 reports it.  Also the wire format behind
 * libSystem's stat$INODE64 (which turns it into Apple's struct stat); keep the
 * field order in sync with user/libsystem.c.
 */
struct fs_fat_statbuf {
	uint32_t	fs_size;	/* byte length (0 for a dir)    */
	uint32_t	fs_ino;		/* stable inode (first cluster) */
	uint8_t		fs_is_dir;	/* 1 if a subdirectory          */
};

/*
 * Probe the first ATA drive for a FAT volume and mount it.  Called once at
 * boot, after ata_drv_init and kmem_init.  Logs the geometry on success and a
 * one-line reason on failure; a failed mount just leaves the FS unavailable.
 */
void	fs_fat_init(void);

/* Non-zero once a volume is mounted and the FS can serve files. */
int	fs_fat_ready(void);

/*
 * Read the whole file named by `path` into a freshly kmalloc'd buffer.  The
 * literal path is resolved through real subdirectories; on a miss it falls
 * back to the 8.3 basename in the root directory.  On success returns
 * FS_FAT_E_OK, stores the buffer in *out_buf (the caller kfree's it) and the
 * real byte length in *out_size.  Returns a negative FS_FAT_E_* otherwise.
 */
int	fs_fat_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size);

/*
 * Report a file-or-directory's metadata without reading it -- the existence +
 * size + type probe behind the Darwin stat path.  Same path resolution as
 * fs_fat_slurp.  Returns FS_FAT_E_OK and fills *out, or a negative FS_FAT_E_*.
 */
int	fs_fat_stat2(const char *path, struct fs_fat_statbuf *out);

/*
 * Fill *out with the `index`-th live entry of the directory named by `path`
 * (resolved through real subdirectories -- NO basename fallback, since a
 * directory listing must be exact).  Returns 1 when an entry was written, 0 at
 * end-of-directory, or a negative FS_FAT_E_* on error.  Enumeration is
 * stateless: each call re-resolves and re-scans to `index`, which is fine for
 * the small read-only directories this serves and keeps no per-fd cursor in
 * the kernel.
 */
int	fs_fat_readdir(const char *path, uint32_t index,
	    struct fs_fat_dirent *out);

#endif /* !_SYS_FS_FAT_H_ */
