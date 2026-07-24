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
#include "fs_apfs.h"
#include "kmem.h"
#include "kprintf.h"

/*
 * APFS container probe.  See fs_apfs.h for what the format is doing; this
 * file is the mechanics of getting at it.
 *
 * Block I/O goes through the block cache (kern/bio.c) -- no Mach round trip
 * from inside the kernel.  It speaks 512-byte sectors, so one APFS block is
 * APFS_BLOCK_SIZE / 512 of them.  The cache earns its keep here more than it
 * would for a simpler filesystem: a B-tree descent re-reads the same interior
 * nodes on every lookup, and this reader deliberately re-walks the tree rather
 * than carry Apple's name hash, so the same few blocks are asked for
 * constantly.
 */

#define	ATA_SECTOR_BYTES	512
#define	APFS_SECTORS_PER_BLOCK	(APFS_BLOCK_SIZE / ATA_SECTOR_BYTES)

/*
 * Mounted container state.  (m) = written once by fs_apfs_init before any
 * reader exists, read-only afterwards.
 */
static struct {
	uint64_t	ac_block_count;		/* (m) */
	uint64_t	ac_xid;			/* (m) newest checkpoint    */
	uint64_t	ac_omap_oid;		/* (m) container object map */
	uint64_t	ac_fs_oid;		/* (m) volume 0 superblock  */
	uint64_t	ac_xp_desc_base;	/* (m) */
	uint64_t	ac_vol_omap_tree;	/* (m) volume omap B-tree   */
	uint64_t	ac_root_tree_bno;	/* (m) file-system B-tree   */
	uint64_t	ac_num_files;		/* (m) */
	uint64_t	ac_num_dirs;		/* (m) */
	uint32_t	ac_xp_desc_blocks;	/* (m) */
	bool		ac_drec_hashed;		/* (m) hashed dirent keys   */
	bool		ac_mounted;		/* (m) */
} g_apfs;

static size_t
str_len(const char *s)
{
	size_t	n;

	for (n = 0; s[n] != '\0'; n++)
		continue;
	return (n);
}

/* The kernel has no string.h; these are the two pieces this file needs. */
static void
mem_copy(uint8_t *dst, const uint8_t *src, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		dst[i] = src[i];
}

static void
mem_zero(uint8_t *dst, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		dst[i] = 0;
}

uint64_t
fs_apfs_fletcher64(const void *p, uint32_t len)
{
	const uint32_t	*w;
	uint64_t	 sum1;
	uint64_t	 sum2;
	uint32_t	 c1;
	uint32_t	 c2;
	uint32_t	 i;

	/*
	 * The modulus is 2^32-1 (not 2^32), which is what makes this Fletcher
	 * rather than a plain running sum: it keeps a word of zeroes from
	 * being indistinguishable from a missing word.
	 */
	w = (const uint32_t *)p;
	sum1 = 0;
	sum2 = 0;
	for (i = 0; i < len / 4; i++) {
		sum1 = (sum1 + w[i]) % 0xFFFFFFFFU;
		sum2 = (sum2 + sum1) % 0xFFFFFFFFU;
	}
	c1 = (uint32_t)~((sum1 + sum2) % 0xFFFFFFFFU);
	c2 = (uint32_t)~((sum1 + c1) % 0xFFFFFFFFU);
	return (((uint64_t)c2 << 32) | c1);
}

/*
 * Read one APFS block with no checksum check.  Only the probe path wants
 * this: the very first read cannot be verified until we know the block size.
 */
static int
read_block_raw(uint64_t bno, void *buf)
{

	if (bio_read(0, bno * APFS_SECTORS_PER_BLOCK, APFS_SECTORS_PER_BLOCK,
	    buf) != 0)
		return (FS_APFS_E_IO);
	return (FS_APFS_E_OK);
}

int
fs_apfs_read_block(uint64_t bno, void *buf)
{
	const struct apfs_obj_phys	*o;
	int				 rv;

	rv = read_block_raw(bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	o = (const struct apfs_obj_phys *)buf;
	if (fs_apfs_fletcher64((const uint8_t *)buf + 8, APFS_BLOCK_SIZE - 8) !=
	    o->o_cksum)
		return (FS_APFS_E_CKSUM);
	return (FS_APFS_E_OK);
}

/*
 * Is this block a container superblock we can believe?  Checksum first (it
 * is the only thing that makes the rest of the block meaningful), then the
 * object type, then the magic.
 */
static bool
block_is_nxsb(const void *buf)
{
	const struct apfs_nx_superblock	*nx;

	nx = (const struct apfs_nx_superblock *)buf;
	if (fs_apfs_fletcher64((const uint8_t *)buf + 8, APFS_BLOCK_SIZE - 8) !=
	    nx->nx_o.o_cksum)
		return (false);
	if ((nx->nx_o.o_type & APFS_OBJ_TYPE_MASK) != APFS_OBJ_NX_SUPERBLOCK)
		return (false);
	return (nx->nx_magic == APFS_NX_MAGIC);
}

/*
 * Scan the checkpoint descriptor ring for the newest superblock that still
 * checksums, and adopt it.  The ring holds superblocks interleaved with
 * checkpoint-map blocks, so most slots are legitimately not superblocks;
 * only a slot that IS one and fails its checksum would be a torn write, and
 * even that is not fatal -- an older checkpoint is still a consistent
 * filesystem, which is the entire point of committing this way.
 */
static int
adopt_newest_checkpoint(const struct apfs_nx_superblock *anchor, void *scratch)
{
	const struct apfs_nx_superblock	*nx;
	uint64_t			 base;
	uint64_t			 best_xid;
	uint32_t			 blocks;
	uint32_t			 found;
	uint32_t			 i;

	base = anchor->nx_xp_desc_base;
	blocks = anchor->nx_xp_desc_blocks & 0x7FFFFFFFU;
	if (blocks == 0 || blocks > 1024)
		return (FS_APFS_E_INVAL);

	best_xid = 0;
	found = 0;
	for (i = 0; i < blocks; i++) {
		if (read_block_raw(base + i, scratch) != FS_APFS_E_OK)
			return (FS_APFS_E_IO);
		if (!block_is_nxsb(scratch))
			continue;
		found++;
		nx = (const struct apfs_nx_superblock *)scratch;
		if (nx->nx_o.o_xid <= best_xid)
			continue;
		best_xid              = nx->nx_o.o_xid;
		g_apfs.ac_xid         = nx->nx_o.o_xid;
		g_apfs.ac_omap_oid    = nx->nx_omap_oid;
		g_apfs.ac_fs_oid      = nx->nx_fs_oid[0];
		g_apfs.ac_block_count = nx->nx_block_count;
	}
	kprintf("apfs: checkpoint ring @%llu (%u blocks): %u superblock(s)\n",
	    (unsigned long long)base, (unsigned)blocks, (unsigned)found);
	if (best_xid == 0)
		return (FS_APFS_E_INVAL);
	g_apfs.ac_xp_desc_base   = base;
	g_apfs.ac_xp_desc_blocks = blocks;
	return (FS_APFS_E_OK);
}

/*
 * Where a B-tree node keeps its three regions.  Pulled out because every
 * tree in the format is read this way and the value base is the one piece
 * that is easy to get subtly wrong: offsets run BACKWARDS from the end of
 * the node, and a root node reserves the last 40 bytes for its btree_info.
 */
struct btree_layout {
	const uint8_t	*bl_toc;
	const uint8_t	*bl_keys;
	const uint8_t	*bl_vals;	/* one past the last value byte */
	uint32_t	 bl_nkeys;
	uint16_t	 bl_flags;
	uint16_t	 bl_level;
	bool		 bl_fixed;
};

static void
btree_layout(const void *node, struct btree_layout *out)
{
	const struct apfs_btree_node_phys	*n;
	const uint8_t				*base;
	uint32_t				 toc_off;
	uint32_t				 toc_len;

	n = (const struct apfs_btree_node_phys *)node;
	base = (const uint8_t *)node;
	toc_off = n->btn_table_space.nl_off;
	toc_len = n->btn_table_space.nl_len;

	out->bl_flags = n->btn_flags;
	out->bl_level = n->btn_level;
	out->bl_nkeys = n->btn_nkeys;
	out->bl_fixed = (n->btn_flags & APFS_BTNODE_FIXED_KV_SIZE) != 0;
	out->bl_toc   = base + APFS_BTNODE_HDR_SIZE + toc_off;
	out->bl_keys  = out->bl_toc + toc_len;
	out->bl_vals  = base + APFS_BLOCK_SIZE -
	    (((n->btn_flags & APFS_BTNODE_ROOT) != 0) ?
	    APFS_BTREE_INFO_SIZE : 0);
}

/*
 * Read entry `i`'s key and value offsets out of the table of contents.  A
 * fixed-KV tree stores bare 16-bit offsets; a variable-KV tree stores
 * offset+length pairs, whose lengths we do not need here.
 */
static void
btree_entry_off(const struct btree_layout *bl, uint32_t i, uint32_t *koff,
    uint32_t *voff)
{
	const struct apfs_kvoff	*fixed;
	const struct apfs_kvloc	*var;

	if (bl->bl_fixed) {
		fixed = (const struct apfs_kvoff *)bl->bl_toc;
		*koff = fixed[i].k;
		*voff = fixed[i].v;
	} else {
		var = (const struct apfs_kvloc *)bl->bl_toc;
		*koff = var[i].k.nl_off;
		*voff = var[i].v.nl_off;
	}
}

/* As above, but also reports the lengths a variable-KV tree records. */
static void
btree_entry_loc(const struct btree_layout *bl, uint32_t i, uint32_t *koff,
    uint32_t *klen, uint32_t *voff, uint32_t *vlen)
{
	const struct apfs_kvloc	*var;

	btree_entry_off(bl, i, koff, voff);
	if (bl->bl_fixed) {
		*klen = 0;
		*vlen = 0;
		return;
	}
	var = (const struct apfs_kvloc *)bl->bl_toc;
	*klen = var[i].k.nl_len;
	*vlen = var[i].v.nl_len;
}

int
fs_apfs_omap_lookup(uint64_t tree_bno, uint64_t oid, uint64_t xid,
    uint64_t *paddr_out)
{
	const struct apfs_omap_key	*k;
	const struct apfs_omap_val	*v;
	struct btree_layout		 bl;
	uint8_t				*node;
	uint64_t			 next;
	uint64_t			 best_xid;
	uint32_t			 koff;
	uint32_t			 voff;
	uint32_t			 i;
	int				 depth;
	int				 rv;

	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (FS_APFS_E_NOMEM);

	rv = FS_APFS_E_INVAL;
	/*
	 * Bounded descent.  A malformed tree must not be able to spin the
	 * kernel: the depth cap is the backstop, since a cycle in the child
	 * pointers is exactly what a corrupt image would produce.
	 */
	for (depth = 0; depth < 16; depth++) {
		rv = fs_apfs_read_block(tree_bno, node);
		if (rv != FS_APFS_E_OK)
			break;
		btree_layout(node, &bl);

		next = 0;
		best_xid = 0;
		for (i = 0; i < bl.bl_nkeys; i++) {
			btree_entry_off(&bl, i, &koff, &voff);
			k = (const struct apfs_omap_key *)(bl.bl_keys + koff);
			if (k->ok_xid > xid)
				continue;	/* newer than this checkpoint */
			if ((bl.bl_flags & APFS_BTNODE_LEAF) != 0) {
				/*
				 * Leaf: take the exact oid, newest version
				 * that this transaction can see.
				 */
				if (k->ok_oid != oid || k->ok_xid < best_xid)
					continue;
				v = (const struct apfs_omap_val *)
				    (bl.bl_vals - voff);
				best_xid = k->ok_xid;
				next = v->ov_paddr;
			} else {
				/*
				 * Interior: keys are sorted, so the child to
				 * follow is the last one whose key does not
				 * exceed what we are looking for.  Its value
				 * is the child's block number.
				 */
				if (k->ok_oid > oid)
					continue;
				next = *(const uint64_t *)(bl.bl_vals - voff);
			}
		}
		if (next == 0) {
			rv = FS_APFS_E_INVAL;
			break;
		}
		if ((bl.bl_flags & APFS_BTNODE_LEAF) != 0) {
			*paddr_out = next;
			rv = FS_APFS_E_OK;
			break;
		}
		tree_bno = next;
		rv = FS_APFS_E_INVAL;	/* in case the cap runs out */
	}

	kfree(node);
	return (rv);
}

/*
 * Follow the container object map to volume 0's superblock, then that
 * volume's own object map to the root of its file-system B-tree.  Two omap
 * hops, because the two live at different levels: the container's map finds
 * volumes, and each volume's map finds that volume's trees.
 */
static int
mount_volume(void *scratch)
{
	const struct apfs_omap_phys	*om;
	const struct apfs_superblock	*sb;
	uint64_t			 apsb_bno;
	uint64_t			 vol_omap_tree;
	uint64_t			 ctr_omap_tree;
	int				 rv;

	/* The container omap oid is PHYSICAL: it is already a block number. */
	rv = fs_apfs_read_block(g_apfs.ac_omap_oid, scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);
	om = (const struct apfs_omap_phys *)scratch;
	ctr_omap_tree = om->om_tree_oid;

	rv = fs_apfs_omap_lookup(ctr_omap_tree, g_apfs.ac_fs_oid, g_apfs.ac_xid,
	    &apsb_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);

	rv = fs_apfs_read_block(apsb_bno, scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);
	sb = (const struct apfs_superblock *)scratch;
	if (sb->apfs_magic != APFS_APSB_MAGIC)
		return (FS_APFS_E_INVAL);

	kprintf("apfs: volume \"%s\" @%llu -- %llu files, %llu dirs\n",
	    (const char *)sb->apfs_volname, (unsigned long long)apsb_bno,
	    (unsigned long long)sb->apfs_num_files,
	    (unsigned long long)sb->apfs_num_directories);

	g_apfs.ac_num_files = sb->apfs_num_files;
	g_apfs.ac_num_dirs  = sb->apfs_num_directories;
	/*
	 * A case- or normalization-insensitive volume hashes dirent names
	 * into the key, which changes the key's shape (and its ordering).
	 */
	g_apfs.ac_drec_hashed = (sb->apfs_incompat &
	    (APFS_INCOMPAT_CASE_INSENSITIVE |
	    APFS_INCOMPAT_NORM_INSENSITIVE)) != 0;

	/* The volume's omap oid is physical too; its root tree oid is not. */
	rv = fs_apfs_read_block(sb->apfs_omap_oid, scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);
	om = (const struct apfs_omap_phys *)scratch;
	vol_omap_tree = om->om_tree_oid;

	/*
	 * Re-read the volume superblock: the omap read above reused scratch,
	 * so sb is stale.  Cheap, and clearer than juggling a third buffer.
	 */
	rv = fs_apfs_read_block(apsb_bno, scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);
	sb = (const struct apfs_superblock *)scratch;

	rv = fs_apfs_omap_lookup(vol_omap_tree, sb->apfs_root_tree_oid,
	    g_apfs.ac_xid, &g_apfs.ac_root_tree_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	g_apfs.ac_vol_omap_tree = vol_omap_tree;
	return (FS_APFS_E_OK);
}

/* ---- file-system tree ----------------------------------------------------- */

/*
 * Callback fired for every leaf record, in tree order.  Returning false
 * stops the walk -- a lookup that has found its answer should not keep
 * reading blocks.
 */
typedef bool (*apfs_rec_fn)(uint64_t oid, uint32_t type, const uint8_t *key,
    uint32_t klen, const uint8_t *val, uint32_t vlen, void *arg);

/*
 * Walk the volume's file-system tree in order.
 *
 * Unlike the container's object map, this tree's interior nodes point at
 * children by VIRTUAL oid, so every descent costs an object-map lookup --
 * that indirection is the price copy-on-write charges for being able to
 * rewrite a node without touching its parent.
 *
 * The walk visits everything rather than binary-searching, which is what
 * lets this reader stay free of Apple's name hash (see fs_apfs.h).  For the
 * directory sizes a Darwin binary here actually opens, whole-tree order is
 * cheap; a real implementation would descend on the key instead.
 */
static bool
btree_walk(uint64_t bno, apfs_rec_fn fn, void *arg, int depth, bool *stopped)
{
	struct btree_layout	 bl;
	uint8_t			*node;
	uint64_t		 child_oid;
	uint64_t		 child_bno;
	uint32_t		 koff;
	uint32_t		 klen;
	uint32_t		 voff;
	uint32_t		 vlen;
	uint32_t		 i;
	bool			 ok;

	if (depth > 8)			/* corrupt tree must not spin us */
		return (false);
	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (false);
	if (fs_apfs_read_block(bno, node) != FS_APFS_E_OK) {
		kfree(node);
		return (false);
	}
	btree_layout(node, &bl);

	ok = true;
	for (i = 0; i < bl.bl_nkeys && !*stopped; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		if ((bl.bl_flags & APFS_BTNODE_LEAF) != 0) {
			const uint8_t	*k;
			uint64_t	 raw;

			k = bl.bl_keys + koff;
			raw = *(const uint64_t *)k;
			if (!fn(raw & APFS_J_OBJ_ID_MASK,
			    (uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT),
			    k, klen, bl.bl_vals - voff, vlen, arg))
				*stopped = true;
			continue;
		}
		child_oid = *(const uint64_t *)(bl.bl_vals - voff);
		if (fs_apfs_omap_lookup(g_apfs.ac_vol_omap_tree, child_oid,
		    g_apfs.ac_xid, &child_bno) != FS_APFS_E_OK) {
			ok = false;
			break;
		}
		if (!btree_walk(child_bno, fn, arg, depth + 1, stopped)) {
			ok = false;
			break;
		}
	}
	kfree(node);
	return (ok);
}

/*
 * Name inside a directory-record key.  The fixed part is the 8-byte record
 * header plus either a 4-byte length-and-hash (hashed volumes) or a 2-byte
 * length; the name follows, NUL-terminated, and the recorded length counts
 * that NUL.
 */
static const char *
drec_name(const uint8_t *key, uint32_t klen, uint32_t *len_out)
{
	uint32_t	n;

	if (g_apfs.ac_drec_hashed) {
		if (klen < 13)
			return (NULL);
		n = *(const uint32_t *)(key + 8) & APFS_DREC_LEN_MASK;
		if (n == 0 || n > klen - 12)
			return (NULL);
		*len_out = n - 1;		/* drop the trailing NUL */
		return ((const char *)key + 12);
	}
	if (klen < 11)
		return (NULL);
	n = *(const uint16_t *)(key + 8);
	if (n == 0 || n > klen - 10)
		return (NULL);
	*len_out = n - 1;
	return ((const char *)key + 10);
}

struct dirent_search {
	const char	*ds_name;
	size_t		 ds_namelen;
	uint64_t	 ds_parent;
	uint64_t	 ds_found;
	bool		 ds_is_dir;
};

static bool
dirent_match(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, void *arg)
{
	const struct apfs_drec_val	*dv;
	struct dirent_search		*ds;
	const char			*name;
	uint32_t			 nlen;
	size_t				 i;

	ds = arg;
	if (type != APFS_TYPE_DIR_REC || oid != ds->ds_parent)
		return (true);
	if (vlen < sizeof(*dv))
		return (true);
	name = drec_name(key, klen, &nlen);
	if (name == NULL || nlen != ds->ds_namelen)
		return (true);
	for (i = 0; i < ds->ds_namelen; i++)
		if (name[i] != ds->ds_name[i])
			return (true);

	dv = (const struct apfs_drec_val *)val;
	ds->ds_found  = dv->dv_file_id;
	ds->ds_is_dir = (dv->dv_flags & 0x0F) == APFS_DT_DIR;
	return (false);				/* found: stop the walk */
}

int
fs_apfs_lookup(const char *path, uint64_t *oid_out, int *is_dir_out)
{
	struct dirent_search	ds;
	const char		*p;
	const char		*comp;
	uint64_t		 oid;
	bool			 is_dir;
	bool			 stopped;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);

	oid = APFS_ROOT_DIR_INO;
	is_dir = true;
	for (p = path; *p != '\0'; ) {
		while (*p == '/')
			p++;
		if (*p == '\0')
			break;
		comp = p;
		while (*p != '\0' && *p != '/')
			p++;

		if (!is_dir)			/* a file has no children */
			return (FS_APFS_E_NOTFOUND);
		ds.ds_name    = comp;
		ds.ds_namelen = (size_t)(p - comp);
		ds.ds_parent  = oid;
		ds.ds_found   = 0;
		ds.ds_is_dir  = false;
		stopped = false;
		if (!btree_walk(g_apfs.ac_root_tree_bno, dirent_match, &ds, 0,
		    &stopped))
			return (FS_APFS_E_IO);
		if (ds.ds_found == 0)
			return (FS_APFS_E_NOTFOUND);
		oid    = ds.ds_found;
		is_dir = ds.ds_is_dir;
	}

	*oid_out = oid;
	if (is_dir_out != NULL)
		*is_dir_out = is_dir ? 1 : 0;
	return (FS_APFS_E_OK);
}

/* ---- inodes, sizes, and bytes -------------------------------------------- */

/*
 * What an inode record tells us.  ii_private_id is the one field worth
 * explaining: file extents are keyed on it rather than on the inode's own
 * object id, and the two differ once hard links exist -- several names, one
 * stream of bytes.  They are equal for every file on a freshly written volume,
 * which is exactly why keying on the wrong one works right up until it
 * silently doesn't.
 */
struct inode_info {
	uint64_t	ii_oid;
	uint64_t	ii_private_id;
	uint64_t	ii_size;
	uint16_t	ii_mode;
	bool		ii_found;
};

static bool
inode_pick(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, void *arg)
{
	const struct apfs_inode_val	*iv;
	const struct apfs_xf_blob	*blob;
	const struct apfs_x_field	*xf;
	const struct apfs_dstream	*ds;
	struct inode_info		*ii;
	uint32_t			 nexts;
	uint32_t			 ent;
	uint32_t			 data;
	uint32_t			 i;

	(void)key;
	(void)klen;
	ii = arg;
	if (type != APFS_TYPE_INODE || oid != ii->ii_oid)
		return (true);
	if (vlen < sizeof(*iv))
		return (true);

	iv = (const struct apfs_inode_val *)val;
	ii->ii_private_id = iv->ai_private_id;
	ii->ii_mode       = iv->ai_mode;
	ii->ii_found      = true;

	/*
	 * No extended fields at all is normal, not an error: that is what an
	 * inode with nothing to say beyond its fixed part looks like.  Its
	 * size stays 0, which for a directory is the right answer.
	 */
	if (vlen < sizeof(*iv) + sizeof(*blob))
		return (false);
	blob  = (const struct apfs_xf_blob *)(val + sizeof(*iv));
	nexts = blob->xb_num_exts;
	ent   = sizeof(*iv) + sizeof(*blob);
	data  = ent + nexts * sizeof(*xf);
	if (data > vlen)
		return (false);

	for (i = 0; i < nexts; i++) {
		xf = (const struct apfs_x_field *)(val + ent +
		    i * sizeof(*xf));
		if (xf->xf_size > vlen - data)
			break;			/* truncated blob; stop */
		if (xf->xf_type == APFS_INO_EXT_TYPE_DSTREAM &&
		    xf->xf_size >= sizeof(*ds)) {
			ds = (const struct apfs_dstream *)(val + data);
			ii->ii_size = ds->ds_size;
		}
		/* Every datum is padded up to a multiple of 8. */
		data += ((uint32_t)xf->xf_size + 7u) & ~7u;
		if (data > vlen)
			break;
	}
	return (false);
}

/* Read the inode record for `oid`.  Returns FS_APFS_E_*. */
static int
inode_info(uint64_t oid, struct inode_info *ii)
{
	bool	stopped;

	ii->ii_oid        = oid;
	ii->ii_private_id = oid;
	ii->ii_size       = 0;
	ii->ii_mode       = 0;
	ii->ii_found      = false;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_pick, ii, 0, &stopped))
		return (FS_APFS_E_IO);
	return (ii->ii_found ? FS_APFS_E_OK : FS_APFS_E_NOTFOUND);
}

int
fs_apfs_stat(const char *path, struct fs_apfs_statbuf *out)
{
	struct inode_info	ii;
	uint64_t		oid;
	int			is_dir;
	int			rv;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	rv = fs_apfs_lookup(path, &oid, &is_dir);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = inode_info(oid, &ii);
	if (rv != FS_APFS_E_OK)
		return (rv);

	out->afs_size   = is_dir ? 0 : ii.ii_size;
	out->afs_ino    = oid;
	out->afs_mode   = ii.ii_mode;
	out->afs_is_dir = is_dir ? 1 : 0;
	return (FS_APFS_E_OK);
}

/*
 * Copying a file's extents into a buffer.  er_bounce holds the one partial
 * block a file that does not end on a block boundary needs -- allocated once
 * by the caller rather than per extent.
 */
struct extent_read {
	uint64_t	 er_id;		/* the dstream this belongs to */
	uint8_t		*er_buf;
	uint8_t		*er_bounce;
	uint64_t	 er_size;
	uint64_t	 er_got;
	int		 er_rv;
};

static bool
extent_copy(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, void *arg)
{
	const struct apfs_file_extent_val	*fe;
	struct extent_read			*er;
	uint64_t				 logical;
	uint64_t				 len;
	uint64_t				 phys;
	uint64_t				 off;
	uint64_t				 dst;
	uint64_t				 n;

	er = arg;
	if (type != APFS_TYPE_FILE_EXTENT || oid != er->er_id)
		return (true);
	/* Key is the record header plus the byte offset this run covers. */
	if (klen < 16 || vlen < sizeof(*fe))
		return (true);

	logical = *(const uint64_t *)(key + 8);
	fe      = (const struct apfs_file_extent_val *)val;
	len     = fe->fe_len_and_flags & APFS_FILE_EXTENT_LEN_MASK;
	phys    = fe->fe_phys_block_num;

	if (logical >= er->er_size)
		return (true);
	if (phys == 0)
		return (true);		/* a hole: the buffer is already zero */

	for (off = 0; off < len; off += APFS_BLOCK_SIZE) {
		dst = logical + off;
		/*
		 * An extent is an ALLOCATED run and can reach past the end of
		 * the file; the tail of its last block is not ours to copy.
		 */
		if (dst >= er->er_size)
			break;
		n = er->er_size - dst;
		if (n >= APFS_BLOCK_SIZE) {
			if (read_block_raw(phys + off / APFS_BLOCK_SIZE,
			    er->er_buf + dst) != FS_APFS_E_OK) {
				er->er_rv = FS_APFS_E_IO;
				return (false);
			}
			n = APFS_BLOCK_SIZE;
		} else {
			if (read_block_raw(phys + off / APFS_BLOCK_SIZE,
			    er->er_bounce) != FS_APFS_E_OK) {
				er->er_rv = FS_APFS_E_IO;
				return (false);
			}
			mem_copy(er->er_buf + dst, er->er_bounce, (size_t)n);
		}
		er->er_got += n;
	}
	return (true);
}

int
fs_apfs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size)
{
	struct fs_apfs_statbuf	 st;
	struct inode_info	 ii;
	struct extent_read	 er;
	uint8_t			*buf;
	uint8_t			*bounce;
	uint64_t		 oid;
	int			 is_dir;
	int			 rv;
	bool			 stopped;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	rv = fs_apfs_lookup(path, &oid, &is_dir);
	if (rv != FS_APFS_E_OK)
		return (rv);
	if (is_dir)
		return (FS_APFS_E_NOTFOUND);
	rv = inode_info(oid, &ii);
	if (rv != FS_APFS_E_OK)
		return (rv);
	st.afs_size = ii.ii_size;
	if (st.afs_size > FS_APFS_MAX_FILE)
		return (FS_APFS_E_TOOBIG);

	/* kmalloc(0) is not a thing worth defining; an empty file gets a byte. */
	buf = kmalloc(st.afs_size != 0 ? (size_t)st.afs_size : 1);
	if (buf == NULL)
		return (FS_APFS_E_NOMEM);
	/*
	 * Zero first.  A sparse file's holes have no blocks to read, so their
	 * bytes are whatever the heap left behind unless we put zeroes there.
	 */
	mem_zero(buf, (size_t)st.afs_size);

	if (st.afs_size == 0) {
		*out_buf  = buf;
		*out_size = 0;
		return (FS_APFS_E_OK);
	}

	bounce = kmalloc(APFS_BLOCK_SIZE);
	if (bounce == NULL) {
		kfree(buf);
		return (FS_APFS_E_NOMEM);
	}

	er.er_id     = ii.ii_private_id;
	er.er_buf    = buf;
	er.er_bounce = bounce;
	er.er_size   = st.afs_size;
	er.er_got    = 0;
	er.er_rv     = FS_APFS_E_OK;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, extent_copy, &er, 0, &stopped))
		er.er_rv = FS_APFS_E_IO;
	kfree(bounce);

	if (er.er_rv != FS_APFS_E_OK) {
		kfree(buf);
		return (er.er_rv);
	}
	*out_buf  = buf;
	*out_size = (uint32_t)st.afs_size;
	return (FS_APFS_E_OK);
}

struct readdir_search {
	struct fs_apfs_dirent	*rs_out;
	uint64_t		 rs_dir;
	uint32_t		 rs_want;
	uint32_t		 rs_seen;
	bool			 rs_hit;
};

static bool
readdir_pick(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, void *arg)
{
	const struct apfs_drec_val	*dv;
	struct readdir_search		*rs;
	const char			*name;
	uint32_t			 nlen;
	uint32_t			 i;

	rs = arg;
	if (type != APFS_TYPE_DIR_REC || oid != rs->rs_dir)
		return (true);
	if (vlen < sizeof(*dv))
		return (true);
	name = drec_name(key, klen, &nlen);
	if (name == NULL)
		return (true);
	if (rs->rs_seen++ != rs->rs_want)
		return (true);

	dv = (const struct apfs_drec_val *)val;
	if (nlen > FS_APFS_NAME_MAX)
		nlen = FS_APFS_NAME_MAX;
	for (i = 0; i < nlen; i++)
		rs->rs_out->ade_name[i] = name[i];
	rs->rs_out->ade_name[nlen] = '\0';
	rs->rs_out->ade_ino    = dv->dv_file_id;
	rs->rs_out->ade_size   = 0;		/* the inode record has it */
	rs->rs_out->ade_is_dir = (dv->dv_flags & 0x0F) == APFS_DT_DIR;
	rs->rs_hit = true;
	return (false);
}

int
fs_apfs_readdir(const char *path, uint32_t index, struct fs_apfs_dirent *out)
{
	struct readdir_search	rs;
	struct inode_info	ii;
	uint64_t		oid;
	int			is_dir;
	int			rv;
	bool			stopped;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	rv = fs_apfs_lookup(path, &oid, &is_dir);
	if (rv != FS_APFS_E_OK)
		return (rv);
	if (!is_dir)
		return (FS_APFS_E_NOTFOUND);

	rs.rs_out  = out;
	rs.rs_dir  = oid;
	rs.rs_want = index;
	rs.rs_seen = 0;
	rs.rs_hit  = false;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, readdir_pick, &rs, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!rs.rs_hit)
		return (0);

	/*
	 * A directory entry carries a name and an object id but no length --
	 * that is in the inode, one more pass over the tree.  Deliberately not
	 * fatal if it is missing: a name we can report with an unknown size
	 * beats failing the whole enumeration.
	 */
	if (!out->ade_is_dir && inode_info(out->ade_ino, &ii) == FS_APFS_E_OK)
		out->ade_size = ii.ii_size;
	return (1);
}

/*
 * Mount-time listing.  Reading a container is only half the claim; walking
 * out of it by name is the other half, so the banner shows the tree it
 * actually resolved rather than asserting it could.  It also remembers the
 * first small regular file it saw, which fs_apfs_init then reads: a size out
 * of an inode proves the metadata path, and only bytes off the disk prove the
 * extent path.
 */
#define	APFS_PROBE_MAX	(64u * 1024u)	/* keep the boot-time read cheap */

struct mount_probe {
	char		mp_path[256];
	uint64_t	mp_size;
	bool		mp_have;
};

static void
list_dir(const char *path, int depth, struct mount_probe *mp)
{
	struct fs_apfs_dirent	 de;
	char			 child[256];
	size_t			 base;
	size_t			 i;
	uint32_t		 idx;

	for (idx = 0; idx < 64; idx++) {
		if (fs_apfs_readdir(path, idx, &de) != 1)
			return;
		kprintf("apfs:   ");
		for (i = 0; i < (size_t)depth * 2; i++)
			kprintf(" ");
		if (de.ade_is_dir)
			kprintf("%s/\n", de.ade_name);
		else
			kprintf("%s  %llu bytes\n", de.ade_name,
			    (unsigned long long)de.ade_size);

		/* Full path of this entry; needed to descend or to read it. */
		base = str_len(path);
		if (base + 1 + str_len(de.ade_name) + 1 > sizeof(child))
			continue;
		for (i = 0; i < base; i++)
			child[i] = path[i];
		if (base > 0 && child[base - 1] != '/')
			child[base++] = '/';
		for (i = 0; de.ade_name[i] != '\0'; i++)
			child[base + i] = de.ade_name[i];
		child[base + i] = '\0';

		if (de.ade_is_dir) {
			if (depth < 2)
				list_dir(child, depth + 1, mp);
			continue;
		}
		if (!mp->mp_have && de.ade_size > 0 &&
		    de.ade_size <= APFS_PROBE_MAX) {
			for (i = 0; i <= base + str_len(de.ade_name); i++)
				mp->mp_path[i] = child[i];
			mp->mp_size = de.ade_size;
			mp->mp_have = true;
		}
	}
}

/*
 * Read one file at mount and report what came back.  The sum is over every
 * byte, which makes it trivially reproducible on the host that wrote the
 * image -- the point is to be checkable, not to be a good checksum.
 */
static void
probe_read(const struct mount_probe *mp)
{
	uint8_t		*buf;
	uint32_t	 size;
	uint32_t	 sum;
	uint32_t	 i;
	int		 rv;

	rv = fs_apfs_slurp(mp->mp_path, &buf, &size);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: read \"%s\" failed (%d)\n", mp->mp_path, rv);
		return;
	}
	sum = 0;
	for (i = 0; i < size; i++)
		sum += buf[i];
	kprintf("apfs: read \"%s\" -- %u bytes, byte sum 0x%08x\n",
	    mp->mp_path, (unsigned)size, (unsigned)sum);
	kfree(buf);
}

void
fs_apfs_init(void)
{
	struct apfs_nx_superblock	*anchor;
	struct mount_probe		 probe;
	uint8_t				*scratch;
	int				 rv;

	g_apfs.ac_mounted = false;

	/*
	 * Two block buffers, off the heap rather than the 16 KiB kernel
	 * stack: the anchor superblock has to stay live while the ring scan
	 * reuses the other one.
	 */
	anchor  = kmalloc(APFS_BLOCK_SIZE);
	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (anchor == NULL || scratch == NULL) {
		kfree(anchor);
		kfree(scratch);
		kprintf("apfs: out of memory for block buffers\n");
		return;
	}

	if (read_block_raw(0, anchor) != FS_APFS_E_OK) {
		kprintf("apfs: no disk0 / read failed -- APFS unavailable\n");
		goto out;
	}
	if (anchor->nx_magic != APFS_NX_MAGIC) {
		kprintf("apfs: no container on disk0 (magic 0x%08x != NXSB)\n",
		    (unsigned)anchor->nx_magic);
		goto out;
	}
	if (anchor->nx_block_size != APFS_BLOCK_SIZE) {
		kprintf("apfs: block size %u unsupported (want %u)\n",
		    (unsigned)anchor->nx_block_size, APFS_BLOCK_SIZE);
		goto out;
	}
	if (!block_is_nxsb(anchor)) {
		kprintf("apfs: block 0 fails its Fletcher-64 -- refusing\n");
		goto out;
	}

	kprintf("apfs: container %llu blocks x %u B (%llu MiB), "
	    "feat=0x%llx incompat=0x%llx\n",
	    (unsigned long long)anchor->nx_block_count,
	    (unsigned)anchor->nx_block_size,
	    (unsigned long long)(anchor->nx_block_count * APFS_BLOCK_SIZE /
	    (1024 * 1024)),
	    (unsigned long long)anchor->nx_features,
	    (unsigned long long)anchor->nx_incompat);

	rv = adopt_newest_checkpoint(anchor, scratch);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: no usable checkpoint superblock (%d)\n", rv);
		goto out;
	}

	kprintf("apfs: container xid %llu, omap oid %llu, volume oid %llu\n",
	    (unsigned long long)g_apfs.ac_xid,
	    (unsigned long long)g_apfs.ac_omap_oid,
	    (unsigned long long)g_apfs.ac_fs_oid);

	rv = mount_volume(scratch);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: volume 0 unreadable (%d) -- APFS unavailable\n",
		    rv);
		goto out;
	}

	g_apfs.ac_mounted = true;
	kprintf("apfs: mounted -- fs B-tree root @%llu, volume omap @%llu, "
	    "%s dirent keys\n",
	    (unsigned long long)g_apfs.ac_root_tree_bno,
	    (unsigned long long)g_apfs.ac_vol_omap_tree,
	    g_apfs.ac_drec_hashed ? "hashed" : "plain");

	probe.mp_have = false;
	probe.mp_size = 0;
	list_dir("/", 0, &probe);
	if (probe.mp_have)
		probe_read(&probe);

out:
	kfree(anchor);
	kfree(scratch);
}

int
fs_apfs_ready(void)
{

	return (g_apfs.ac_mounted ? 1 : 0);
}
