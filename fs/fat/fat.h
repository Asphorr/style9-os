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
 * Minimal read-only FAT12/16 filesystem: open a file by name, read it,
 * stat it, and enumerate a directory, for the Darwin binaries' stdio and
 * directory walkers (figlet fonts, tree(1)).
 *
 * Names match by 8.3 short name (no long names); a path descends real
 * subdirectories.  open/stat try the literal path and fall back to the
 * basename in the root directory, so a binary's baked-in macOS path, whose
 * directories do not exist here, still finds a font placed in the root.
 * No writes.  Block I/O goes through the block cache (bio_read).
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
 * One directory entry, as fs_fat_readdir reports it; fs.c converts it to
 * struct fs_dirent (fs.h), the wire format.
 */
struct fs_fat_dirent {
	uint32_t	fde_ino;	/* stable inode (first cluster) */
	uint32_t	fde_size;	/* byte length (0 for a dir)    */
	uint8_t		fde_is_dir;	/* 1 if a subdirectory          */
	char		fde_name[FS_FAT_NAME_MAX];
};

/*
 * A file's metadata, as fs_fat_stat2 reports it; fs.c converts it to struct
 * fs_statbuf (fs.h), the wire format.
 *
 * Read from the volume: the write time and date, the access date, the
 * create time and date, and the read-only attribute.  Synthesised: the
 * permission bits, from that one attribute bit (owner, group and link count
 * are filled in by fs.c).  A timestamp of zero means none was recorded.
 */
struct fs_fat_statbuf {
	uint64_t	fs_mtime_ns;	/* last write   (0 if unrecorded) */
	uint64_t	fs_atime_ns;	/* last access, date only         */
	uint64_t	fs_btime_ns;	/* created                        */
	uint64_t	fs_alloced;	/* bytes on disk (whole clusters) */
	uint32_t	fs_size;	/* byte length (0 for a dir)    */
	uint32_t	fs_ino;		/* stable inode (first cluster) */
	uint16_t	fs_mode;	/* synthesised POSIX mode word  */
	uint8_t		fs_is_dir;	/* 1 if a subdirectory          */
};

/*
 * The modes FAT can express: one read-only bit and no owner.  Named here so
 * a 0755 read out of a FAT stat can be traced to where it was decided.
 */
#define	FS_FAT_MODE_DIR		0040755
#define	FS_FAT_MODE_FILE	0100644
#define	FS_FAT_MODE_DIR_RO	0040555
#define	FS_FAT_MODE_FILE_RO	0100444

/*
 * Probe the first ATA drive for a FAT volume and mount it.  Called once at
 * boot, after ata_drv_init and bio_init.  Logs the geometry, or a one-line
 * reason on failure, which leaves the FS unavailable.
 */
void	fs_fat_init(void);

/* Non-zero once a volume is mounted and the FS can serve files. */
int	fs_fat_ready(void);

/*
 * Read the whole file at `path' into a fresh kmalloc'd buffer, resolving
 * the literal path and falling back to the 8.3 basename in the root.
 * Returns FS_FAT_E_OK with the buffer (the caller kfree's it) in *out_buf
 * and the byte length in *out_size, or a negative FS_FAT_E_*.
 */
int	fs_fat_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size);

/*
 * Resolve `path' (as fs_fat_slurp does) to its starting cluster and byte
 * length, the expensive half of reading, paid once.  Directories are
 * refused.
 */
int	fs_fat_open(const char *path, uint64_t *id_out, uint64_t *size_out);

/*
 * Read at most `len' bytes of a resolved file (`id' = starting cluster,
 * `size' = its length) at offset `off' into `buf', the count in *out_got.
 * A read at or past end-of-file returns FS_FAT_E_OK with zero bytes; one
 * that runs off the end is short.
 */
int	fs_fat_pread(uint64_t id, uint64_t size, uint64_t off, uint8_t *buf,
	    uint32_t len, uint32_t *out_got);

/*
 * Metadata for a file or directory, resolved as fs_fat_slurp does.
 * Returns FS_FAT_E_OK and fills *out, or a negative FS_FAT_E_*.
 */
int	fs_fat_stat2(const char *path, struct fs_fat_statbuf *out);

/*
 * Fill *out with the `index'-th live entry of the directory at `path'
 * (exact resolution, no basename fallback).  Returns 1 when an entry was
 * written, 0 at end-of-directory, or a negative FS_FAT_E_*.  Stateless:
 * each call re-resolves and re-scans to `index'.
 */
int	fs_fat_readdir(const char *path, uint32_t index,
	    struct fs_fat_dirent *out);

#endif /* !_SYS_FS_FAT_H_ */
