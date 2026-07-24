/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "ata_drv.h"
#include "bio.h"
#include "fs_fat.h"
#include "kmem.h"
#include "kprintf.h"

#define	FAT_SECTOR_BYTES	512u
#define	FAT_DIRENT_BYTES	32u

/* Directory-entry attribute bits we care about. */
#define	FAT_ATTR_VOLUME_ID	0x08
#define	FAT_ATTR_DIRECTORY	0x10
#define	FAT_ATTR_LONG_NAME	0x0F	/* RO|HID|SYS|VOL -- an LFN slot */

/* Windows-NT case hints (dirent offset 12): lower-case the name / extension. */
#define	FAT_CASE_NAME		0x08
#define	FAT_CASE_EXT		0x10

/* First-byte sentinels in a directory entry's name field. */
#define	FAT_DIRENT_END		0x00	/* no further entries          */
#define	FAT_DIRENT_FREE		0xE5	/* deleted entry               */

/* FAT16 cluster values. */
#define	FAT16_EOC_MIN		0xFFF8u	/* >= this == end of chain     */
#define	FAT16_BAD		0xFFF7u

/*
 * The one mounted volume.  Lock key: (c) const after fs_fat_init -- the FS is
 * read-only and mounted once at boot, so no lock is needed for the geometry;
 * ata_kread serialises the actual device access under the channel lock.
 */
static struct fat_vol {
	bool		fv_mounted;	/* (c) */
	uint8_t		fv_sec_per_clus;/* (c) */
	uint16_t	fv_root_entries;/* (c) */
	uint32_t	fv_fat_start;	/* (c) first FAT sector (LBA)      */
	uint32_t	fv_root_start;	/* (c) root-dir first sector       */
	uint32_t	fv_root_sectors;/* (c) root-dir sector span        */
	uint32_t	fv_data_start;	/* (c) cluster-2 first sector      */
	uint32_t	fv_total_sectors;/* (c) */
} g_fat;

/*
 * A directory to iterate.  FAT16 keeps the root directory in a fixed sector
 * span OUTSIDE the data area, while every subdirectory is an ordinary cluster
 * chain -- so the walker handles the two cases apart.
 */
struct fat_dir {
	bool		fd_is_root;
	uint32_t	fd_clus;	/* first cluster (subdir only) */
};

/* One resolved directory entry. */
struct fat_ent {
	uint32_t	fe_clus;	/* first cluster (0 if empty)  */
	uint32_t	fe_size;	/* byte length (0 for a dir)   */
	bool		fe_is_dir;
};

/* Visitor invoked per live 8.3 entry; nonzero return stops the walk. */
typedef int	(*fat_visit_fn)(const uint8_t *de, void *arg);

static uint16_t	rd16(const uint8_t *p, uint32_t off);
static uint32_t	rd32(const uint8_t *p, uint32_t off);
static int	read_sector(uint32_t lba, void *buf);
static uint32_t	fat_next(uint32_t clus);
static void	make_83(const char *basename, char out[11]);
static const char *path_basename(const char *path);
static void	de_to_name(const uint8_t *de, char out[FS_FAT_NAME_MAX]);
static uint32_t	synth_ino(const struct fat_ent *e);
static int	dir_walk(const struct fat_dir *d, fat_visit_fn fn, void *arg);
static int	dir_find(const struct fat_dir *d, const char name83[11],
		    struct fat_ent *out);
static int	resolve_dir(const char *path, struct fat_dir *out);
static int	resolve_entry(const char *path, struct fat_ent *out);

/* ---- little-endian field readers --------------------------------------- */

static uint16_t
rd16(const uint8_t *p, uint32_t off)
{

	return ((uint16_t)(p[off] | ((uint16_t)p[off + 1] << 8)));
}

static uint32_t
rd32(const uint8_t *p, uint32_t off)
{

	return ((uint32_t)p[off] | ((uint32_t)p[off + 1] << 8) |
	    ((uint32_t)p[off + 2] << 16) | ((uint32_t)p[off + 3] << 24));
}

static int
read_sector(uint32_t lba, void *buf)
{

	return (bio_read(0, (uint64_t)lba, 1, buf) == 0 ?
	    FS_FAT_E_OK : FS_FAT_E_IO);
}

/* ---- mount ------------------------------------------------------------- */

void
fs_fat_init(void)
{
	uint8_t		bpb[FAT_SECTOR_BYTES];
	uint32_t	clus_start;
	uint32_t	data_clusters;
	uint32_t	fat_size;
	uint32_t	root_bytes;
	uint32_t	total;
	uint16_t	bytes_per_sec;
	uint16_t	reserved;
	uint8_t		num_fats;

	g_fat.fv_mounted = false;

	if (read_sector(0, bpb) != FS_FAT_E_OK) {
		kprintf("fs_fat: no disk0 / read failed -- FS unavailable\n");
		return;
	}

	bytes_per_sec = rd16(bpb, 11);
	g_fat.fv_sec_per_clus = bpb[13];
	reserved              = rd16(bpb, 14);
	num_fats              = bpb[16];
	g_fat.fv_root_entries = rd16(bpb, 17);
	total                 = rd16(bpb, 19);
	fat_size              = rd16(bpb, 22);
	if (total == 0)
		total = rd32(bpb, 32);

	/*
	 * Only a 512-byte-sector FAT12/16 with a fixed-size root directory is
	 * supported.  A FAT32 BPB leaves fat_size_16 and root_entries zero
	 * (it uses the 32-bit fields at offset 36) -- refuse it rather than
	 * misread.  ata_kread reads 512-byte sectors, so a different sector
	 * size cannot be served.
	 */
	if (bytes_per_sec != FAT_SECTOR_BYTES || g_fat.fv_sec_per_clus == 0 ||
	    num_fats == 0 || fat_size == 0 || g_fat.fv_root_entries == 0) {
		kprintf("fs_fat: not a FAT12/16 volume (bps=%u spc=%u nfat=%u "
		    "fatsz=%u root=%u) -- FS unavailable\n",
		    (unsigned)bytes_per_sec, (unsigned)g_fat.fv_sec_per_clus,
		    (unsigned)num_fats, (unsigned)fat_size,
		    (unsigned)g_fat.fv_root_entries);
		return;
	}

	root_bytes = (uint32_t)g_fat.fv_root_entries * FAT_DIRENT_BYTES;
	g_fat.fv_fat_start    = reserved;
	g_fat.fv_root_start   = reserved + (uint32_t)num_fats * fat_size;
	g_fat.fv_root_sectors = (root_bytes + (FAT_SECTOR_BYTES - 1)) /
	    FAT_SECTOR_BYTES;
	g_fat.fv_data_start   = g_fat.fv_root_start + g_fat.fv_root_sectors;
	g_fat.fv_total_sectors = total;

	clus_start    = g_fat.fv_data_start;
	data_clusters = (total > clus_start) ?
	    (total - clus_start) / g_fat.fv_sec_per_clus : 0;

	g_fat.fv_mounted = true;

	kprintf("fs_fat: mounted FAT%s on disk0 -- %u sectors, %u clusters "
	    "(spc=%u), root@%u data@%u\n",
	    data_clusters < 4085 ? "12" : "16",
	    (unsigned)total, (unsigned)data_clusters,
	    (unsigned)g_fat.fv_sec_per_clus,
	    (unsigned)g_fat.fv_root_start, (unsigned)g_fat.fv_data_start);

	/* Self-test: confirm the canonical font is reachable in the root. */
	{
		struct fat_dir	root;
		struct fat_ent	e;
		char		name83[11];

		root.fd_is_root = true;
		root.fd_clus = 0;
		make_83("standard.flf", name83);
		if (dir_find(&root, name83, &e) == FS_FAT_E_OK)
			kprintf("fs_fat: self-test -- standard.flf found "
			    "(%u bytes, cluster %u)\n",
			    (unsigned)e.fe_size, (unsigned)e.fe_clus);
		else
			kprintf("fs_fat: self-test -- standard.flf NOT found "
			    "in root dir\n");
	}
}

int
fs_fat_ready(void)
{

	return (g_fat.fv_mounted ? 1 : 0);
}

/* ---- name handling ----------------------------------------------------- */

/* Last path component (handles both '/' and a bare name). */
static const char *
path_basename(const char *path)
{
	const char	*base;
	const char	*p;

	base = path;
	for (p = path; *p != '\0'; p++) {
		if (*p == '/')
			base = p + 1;
	}
	return (base);
}

/*
 * Pack a basename into the 11-byte FAT 8.3 form: up to 8 name chars, then up
 * to 3 extension chars, space-padded, uppercased.  Names that exceed 8.3 are
 * truncated (no ~1 mangling) -- a deliberate limitation, fine for the short
 * names this serves.
 */
static void
make_83(const char *basename, char out[11])
{
	const char	*dot;
	const char	*p;
	int		 i;

	for (i = 0; i < 11; i++)
		out[i] = ' ';

	dot = NULL;
	for (p = basename; *p != '\0'; p++) {
		if (*p == '.')
			dot = p;		/* last dot wins */
	}

	i = 0;
	for (p = basename; *p != '\0' && p != dot && i < 8; p++, i++)
		out[i] = (*p >= 'a' && *p <= 'z') ? (char)(*p - 32) : *p;
	if (dot != NULL) {
		i = 0;
		for (p = dot + 1; *p != '\0' && i < 3; p++, i++)
			out[8 + i] = (*p >= 'a' && *p <= 'z') ?
			    (char)(*p - 32) : *p;
	}
}

/*
 * Render an 8.3 entry's name into a NUL-terminated display string
 * ("STANDARD"+"FLF" -> "standard.flf"): trim the space padding and apply the
 * Windows-NT lower-case hint bits (offset 12) so an mkfs-lower-cased short
 * name reads back the way it was written.  "." and ".." pass through verbatim.
 */
static void
de_to_name(const uint8_t *de, char out[FS_FAT_NAME_MAX])
{
	uint8_t	ntcase;
	int	i;
	int	n;

	ntcase = de[12];
	n = 0;
	for (i = 0; i < 8 && de[i] != ' '; i++) {
		char	c;

		c = (char)de[i];
		if ((ntcase & FAT_CASE_NAME) != 0 && c >= 'A' && c <= 'Z')
			c = (char)(c + 32);
		out[n++] = c;
	}
	if (de[8] != ' ') {
		out[n++] = '.';
		for (i = 8; i < 11 && de[i] != ' '; i++) {
			char	c;

			c = (char)de[i];
			if ((ntcase & FAT_CASE_EXT) != 0 && c >= 'A' && c <= 'Z')
				c = (char)(c + 32);
			out[n++] = c;
		}
	}
	out[n] = '\0';
}

/*
 * A stable inode for an entry: its first cluster, which is unique per file on
 * a FAT volume.  Directories always carry a nonzero start cluster, so their
 * inodes are always distinct -- which is what a path walker like tree(1)
 * relies on to detect (and refuse) directory cycles.  An empty file has
 * cluster 0; give it a nonzero synthetic value so it never reads as "no inode".
 */
static uint32_t
synth_ino(const struct fat_ent *e)
{

	return (e->fe_clus != 0 ? e->fe_clus : 0x10000000u + e->fe_size);
}

/* ---- FAT chain --------------------------------------------------------- */

/* Next cluster after `clus`, or >= FAT16_EOC_MIN at end / on error. */
static uint32_t
fat_next(uint32_t clus)
{
	uint8_t		sec[FAT_SECTOR_BYTES];
	uint32_t	fat_off;
	uint32_t	fat_sec;

	fat_off = clus * 2u;
	fat_sec = g_fat.fv_fat_start + fat_off / FAT_SECTOR_BYTES;
	if (read_sector(fat_sec, sec) != FS_FAT_E_OK)
		return (FAT16_EOC_MIN);
	return (rd16(sec, fat_off % FAT_SECTOR_BYTES));
}

/* ---- directory walk ---------------------------------------------------- */

/*
 * Visit each live 8.3 entry of `d` in order, calling fn(de, arg).  Stops and
 * returns fn's value the first time it is nonzero; returns 0 if the directory
 * ended first, or a negative FS_FAT_E_* on I/O error.  Deleted (0xE5),
 * end-of-directory (0x00), and long-name slots are filtered here; the
 * volume-id label is passed through for the visitor to decide on.
 */
static int
dir_walk(const struct fat_dir *d, fat_visit_fn fn, void *arg)
{
	uint8_t		sec[FAT_SECTOR_BYTES];
	uint32_t	clus;
	uint32_t	nsec;

	if (!g_fat.fv_mounted)
		return (FS_FAT_E_NOMOUNT);

	if (d->fd_is_root) {
		clus = 0;
		nsec = g_fat.fv_root_sectors;
	} else {
		clus = d->fd_clus;
		nsec = g_fat.fv_sec_per_clus;
	}

	for (;;) {
		uint32_t	s;

		for (s = 0; s < nsec; s++) {
			uint32_t	e;
			uint32_t	lba;

			if (d->fd_is_root)
				lba = g_fat.fv_root_start + s;
			else
				lba = g_fat.fv_data_start +
				    (clus - 2) * g_fat.fv_sec_per_clus + s;
			if (read_sector(lba, sec) != FS_FAT_E_OK)
				return (FS_FAT_E_IO);

			for (e = 0; e < FAT_SECTOR_BYTES; e += FAT_DIRENT_BYTES) {
				const uint8_t	*de;
				int		 rv;

				de = sec + e;
				if (de[0] == FAT_DIRENT_END)
					return (0);	/* end of directory */
				if (de[0] == FAT_DIRENT_FREE)
					continue;
				if (de[11] == FAT_ATTR_LONG_NAME)
					continue;	/* long-name slot   */
				rv = fn(de, arg);
				if (rv != 0)
					return (rv);
			}
		}

		if (d->fd_is_root)
			break;			/* fixed region: one pass */
		clus = fat_next(clus);
		if (clus < 2 || clus >= FAT16_EOC_MIN)
			break;
	}
	return (0);
}

/* ---- lookup by name ---------------------------------------------------- */

struct find_arg {
	const char	*fa_name83;	/* 11 bytes to match */
	struct fat_ent	*fa_out;
};

static int
find_cb(const uint8_t *de, void *arg)
{
	struct find_arg	*fa;
	int		 i;

	fa = (struct find_arg *)arg;
	if ((de[11] & FAT_ATTR_VOLUME_ID) != 0)
		return (0);			/* skip the volume label */
	for (i = 0; i < 11; i++) {
		if ((char)de[i] != fa->fa_name83[i])
			return (0);
	}
	fa->fa_out->fe_clus = rd16(de, 26);
	fa->fa_out->fe_size = rd32(de, 28);
	fa->fa_out->fe_is_dir = (de[11] & FAT_ATTR_DIRECTORY) != 0;
	return (1);				/* found -> stop the walk */
}

static int
dir_find(const struct fat_dir *d, const char name83[11], struct fat_ent *out)
{
	struct find_arg	fa;
	int		rv;

	fa.fa_name83 = name83;
	fa.fa_out = out;
	rv = dir_walk(d, find_cb, &fa);
	if (rv < 0)
		return (rv);
	return (rv == 1 ? FS_FAT_E_OK : FS_FAT_E_NOTFOUND);
}

/* ---- path resolution --------------------------------------------------- */

/*
 * Resolve `path` to the directory it names.  A leading slash and empty / "."
 * components are skipped; each remaining component must name a subdirectory.
 * ".." is not supported (the read-only consumers never emit it).  "" or "/"
 * resolve to the root.
 */
static int
resolve_dir(const char *path, struct fat_dir *out)
{
	struct fat_dir	cur;
	const char	*p;

	cur.fd_is_root = true;
	cur.fd_clus = 0;

	p = path;
	while (*p != '\0') {
		char		comp[FS_FAT_NAME_MAX];
		char		name83[11];
		struct fat_ent	e;
		int		i;
		int		rv;

		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		i = 0;
		while (*p != '\0' && *p != '/') {
			if (i < FS_FAT_NAME_MAX - 1)
				comp[i++] = *p;
			p++;
		}
		comp[i] = '\0';
		if (comp[0] == '.' && comp[1] == '\0')
			continue;		/* "." -- stay put */

		make_83(comp, name83);
		rv = dir_find(&cur, name83, &e);
		if (rv != FS_FAT_E_OK)
			return (rv);
		if (!e.fe_is_dir)
			return (FS_FAT_E_NOTFOUND);	/* a file mid-path */
		cur.fd_is_root = false;
		cur.fd_clus = e.fe_clus;
	}
	*out = cur;
	return (FS_FAT_E_OK);
}

/*
 * Resolve `path` to its entry.  First: if the whole path names a directory
 * (including "/" -> the root), describe that directory -- so stat() and
 * opendir() work on a directory by its own path.  Otherwise resolve it as a
 * file: split off the last component, resolve the leading directories, and
 * look the component up there.  On any miss, fall back to the 8.3 basename in
 * the root directory -- the hatch that lets a binary's baked-in macOS path
 * (/usr/local/.../standard.flf), whose leading dirs are absent here, still
 * find a font placed in the root, while a real on-disk path resolves exactly.
 */
static int
resolve_entry(const char *path, struct fat_ent *out)
{
	char		buf[256];
	struct fat_dir	dir;
	struct fat_dir	root;
	char		name83[11];
	char		*base;
	size_t		i;

	/* The whole path names a directory (incl. "/")?  Describe it. */
	if (resolve_dir(path, &dir) == FS_FAT_E_OK) {
		out->fe_is_dir = true;
		out->fe_size = 0;
		out->fe_clus = dir.fd_is_root ? 0 : dir.fd_clus;
		return (FS_FAT_E_OK);
	}

	/* Otherwise resolve it as a file: parent directory + basename. */
	for (i = 0; path[i] != '\0'; i++) {
		if (i >= sizeof(buf) - 1)
			goto fallback;
		buf[i] = path[i];
	}
	buf[i] = '\0';

	/* Split at the last '/': everything before it is the parent path. */
	base = buf;
	for (i = 0; buf[i] != '\0'; i++) {
		if (buf[i] == '/')
			base = &buf[i + 1];
	}
	if (base == buf) {			/* no slash: a name in the root */
		dir.fd_is_root = true;
		dir.fd_clus = 0;
	} else {
		base[-1] = '\0';		/* terminate the parent path */
		if (resolve_dir(buf, &dir) != FS_FAT_E_OK)
			goto fallback;
	}
	if (*base == '\0')			/* trailing slash, no name */
		goto fallback;
	make_83(base, name83);
	if (dir_find(&dir, name83, out) == FS_FAT_E_OK)
		return (FS_FAT_E_OK);

fallback:
	root.fd_is_root = true;
	root.fd_clus = 0;
	make_83(path_basename(path), name83);
	return (dir_find(&root, name83, out));
}

/* ---- slurp ------------------------------------------------------------- */

int
fs_fat_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size)
{
	struct fat_ent	e;
	uint8_t		*buf;
	uint32_t	clus;
	uint32_t	clus_bytes;
	uint32_t	off;
	uint32_t	bufcap;
	int		rv;

	if (path == NULL || out_buf == NULL || out_size == NULL)
		return (FS_FAT_E_INVAL);
	if (!g_fat.fv_mounted)
		return (FS_FAT_E_NOMOUNT);

	rv = resolve_entry(path, &e);
	if (rv != FS_FAT_E_OK)
		return (rv);
	if (e.fe_is_dir)
		return (FS_FAT_E_NOTFOUND);	/* not a regular file */
	if (e.fe_size > FS_FAT_MAX_FILE)
		return (FS_FAT_E_TOOBIG);

	clus_bytes = (uint32_t)g_fat.fv_sec_per_clus * FAT_SECTOR_BYTES;

	/*
	 * Round the buffer up to a whole cluster so each cluster reads in
	 * directly; the caller is told the real byte size, so the rounding
	 * tail is simply never looked at.  A zero-length file still gets a
	 * 1-byte allocation so out_buf is never NULL on success.
	 */
	bufcap = e.fe_size == 0 ? 1u :
	    ((e.fe_size + clus_bytes - 1) / clus_bytes) * clus_bytes;
	buf = kmalloc(bufcap);
	if (buf == NULL)
		return (FS_FAT_E_NOMEM);

	clus = e.fe_clus;
	off  = 0;
	while (off < e.fe_size && clus >= 2 && clus < FAT16_EOC_MIN) {
		uint32_t	lba;

		lba = g_fat.fv_data_start +
		    (clus - 2) * g_fat.fv_sec_per_clus;
		if (bio_read(0, (uint64_t)lba, g_fat.fv_sec_per_clus,
		    buf + off) != 0) {
			kfree(buf);
			return (FS_FAT_E_IO);
		}
		off += clus_bytes;
		clus = fat_next(clus);
	}

	if (off < e.fe_size) {		/* chain ended before the file did */
		kfree(buf);
		return (FS_FAT_E_IO);
	}

	*out_buf  = buf;
	*out_size = e.fe_size;
	return (FS_FAT_E_OK);
}

/*
 * The ranged read behind fs_pread.  FAT has no extent map: the only way to
 * reach byte N is to follow the cluster chain from the start, which is what
 * the skip loop below does.  That makes a pread O(offset) in chain links --
 * cheap enough here (the links are FAT-sector reads, and the block cache has
 * the FAT resident after the first file), and it is the shape of the
 * filesystem rather than a shortcut in the reader.
 */
int
fs_fat_pread(const char *path, uint64_t off, uint8_t *buf, uint32_t len,
    uint32_t *out_got)
{
	struct fat_ent	 e;
	uint8_t		*bounce;
	uint32_t	 clus;
	uint32_t	 clus_bytes;
	uint32_t	 skip;
	uint32_t	 within;
	uint32_t	 done;
	uint32_t	 n;
	uint32_t	 i;
	int		 rv;

	if (path == NULL || buf == NULL || out_got == NULL)
		return (FS_FAT_E_INVAL);
	if (!g_fat.fv_mounted)
		return (FS_FAT_E_NOMOUNT);

	*out_got = 0;
	if (len == 0)
		return (FS_FAT_E_OK);

	rv = resolve_entry(path, &e);
	if (rv != FS_FAT_E_OK)
		return (rv);
	if (e.fe_is_dir)
		return (FS_FAT_E_NOTFOUND);
	if (off >= e.fe_size)		/* at or past EOF: zero bytes, no error */
		return (FS_FAT_E_OK);
	if ((uint64_t)len > e.fe_size - off)
		len = (uint32_t)(e.fe_size - off);

	clus_bytes = (uint32_t)g_fat.fv_sec_per_clus * FAT_SECTOR_BYTES;
	skip       = (uint32_t)(off / clus_bytes);
	within     = (uint32_t)(off % clus_bytes);

	clus = e.fe_clus;
	for (i = 0; i < skip; i++) {
		if (clus < 2 || clus >= FAT16_EOC_MIN)
			return (FS_FAT_E_IO);	/* chain ended before the file did */
		clus = fat_next(clus);
	}

	bounce = kmalloc(clus_bytes);
	if (bounce == NULL)
		return (FS_FAT_E_NOMEM);

	done = 0;
	while (done < len && clus >= 2 && clus < FAT16_EOC_MIN) {
		uint32_t	lba;

		lba = g_fat.fv_data_start + (clus - 2) * g_fat.fv_sec_per_clus;
		if (bio_read(0, (uint64_t)lba, g_fat.fv_sec_per_clus,
		    bounce) != 0) {
			kfree(bounce);
			return (FS_FAT_E_IO);
		}
		n = clus_bytes - within;
		if (n > len - done)
			n = len - done;
		for (i = 0; i < n; i++)
			buf[done + i] = bounce[within + i];
		done  += n;
		within = 0;
		clus   = fat_next(clus);
	}
	kfree(bounce);

	if (done < len)			/* chain ended before the file did */
		return (FS_FAT_E_IO);

	*out_got = done;
	return (FS_FAT_E_OK);
}

/* ---- stat -------------------------------------------------------------- */

int
fs_fat_stat2(const char *path, struct fs_fat_statbuf *out)
{
	struct fat_ent	e;
	int		rv;

	if (path == NULL || out == NULL)
		return (FS_FAT_E_INVAL);
	if (!g_fat.fv_mounted)
		return (FS_FAT_E_NOMOUNT);

	rv = resolve_entry(path, &e);
	if (rv != FS_FAT_E_OK)
		return (rv);
	out->fs_size = e.fe_is_dir ? 0 : e.fe_size;
	out->fs_ino = synth_ino(&e);
	out->fs_is_dir = e.fe_is_dir ? 1 : 0;
	return (FS_FAT_E_OK);
}

/* ---- readdir ----------------------------------------------------------- */

struct readdir_arg {
	uint32_t		 ra_target;	/* index wanted        */
	uint32_t		 ra_seen;	/* live entries so far */
	struct fs_fat_dirent	*ra_out;
	bool			 ra_got;
};

static int
readdir_cb(const uint8_t *de, void *arg)
{
	struct readdir_arg	*ra;
	struct fat_ent		 e;

	ra = (struct readdir_arg *)arg;
	if ((de[11] & FAT_ATTR_VOLUME_ID) != 0)
		return (0);			/* skip the volume label */
	if (ra->ra_seen != ra->ra_target) {
		ra->ra_seen++;
		return (0);
	}

	e.fe_clus = rd16(de, 26);
	e.fe_size = rd32(de, 28);
	e.fe_is_dir = (de[11] & FAT_ATTR_DIRECTORY) != 0;
	de_to_name(de, ra->ra_out->fde_name);
	ra->ra_out->fde_size = e.fe_is_dir ? 0 : e.fe_size;
	ra->ra_out->fde_ino = synth_ino(&e);
	ra->ra_out->fde_is_dir = e.fe_is_dir ? 1 : 0;
	ra->ra_got = true;
	return (1);				/* got it -> stop the walk */
}

int
fs_fat_readdir(const char *path, uint32_t index, struct fs_fat_dirent *out)
{
	struct readdir_arg	ra;
	struct fat_dir		dir;
	int			rv;

	if (path == NULL || out == NULL)
		return (FS_FAT_E_INVAL);
	if (!g_fat.fv_mounted)
		return (FS_FAT_E_NOMOUNT);

	rv = resolve_dir(path, &dir);
	if (rv != FS_FAT_E_OK)
		return (rv);

	ra.ra_target = index;
	ra.ra_seen = 0;
	ra.ra_out = out;
	ra.ra_got = false;
	rv = dir_walk(&dir, readdir_cb, &ra);
	if (rv < 0)
		return (rv);
	return (ra.ra_got ? 1 : 0);		/* 1 = filled, 0 = past end */
}
