/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_FS_APFS_H_
#define	_SYS_FS_APFS_H_

#include <stdint.h>

/*
 * Read-only APFS -- the filesystem a Darwin personality ought to be reading.
 *
 * Everything else on this rung is already Apple-shaped (Mach-O images, a
 * clean-room dyld, libSystem, launchd, Mach IPC), and FAT was only ever the
 * placeholder that let a binary find a data file at all.  APFS is the real
 * thing, and unlike the rest of the Apple surface here it does not have to be
 * reverse engineered: Apple published the on-disk format.
 *
 * Three ideas carry the whole format, and the code below is organised around
 * them:
 *
 *	1. EVERY metadata block starts with an obj_phys header carrying a
 *	   Fletcher-64 checksum of the rest of the block, plus the object's id
 *	   (oid) and the transaction that wrote it (xid).  Nothing is trusted
 *	   without checking that checksum -- a half-written block is exactly
 *	   what a copy-on-write filesystem expects to find after a crash.
 *
 *	2. There is no single fixed superblock.  Block 0 is only an anchor;
 *	   the live container superblock is whichever copy in the CHECKPOINT
 *	   DESCRIPTOR ring carries the highest xid and still checksums.  That
 *	   ring is how APFS commits atomically: write the new state elsewhere,
 *	   then land one new superblock.  Mounting means finding the newest
 *	   valid one, which is what makes an interrupted write a no-op rather
 *	   than corruption.
 *
 *	3. Objects are addressed indirectly.  A PHYSICAL oid is a block
 *	   number, but a VIRTUAL oid has to be translated through an object
 *	   map (omap) B-tree first, because copy-on-write moves objects
 *	   without changing their identity.
 *
 * This rung implements (1) and (2): probe the container, verify it, and find
 * the live superblock.  Volume and file access build on top.
 */

/* 'NXSB' as it appears little-endian at offset 32 of the container block. */
#define	APFS_NX_MAGIC		0x4253584EU

/* Only block size we accept; the one every real container uses. */
#define	APFS_BLOCK_SIZE		4096

#define	APFS_NX_MAX_FILE_SYSTEMS	100

/*
 * obj_phys.o_type packs a type in the low 16 bits with storage-class and
 * flag bits above it.
 */
#define	APFS_OBJ_TYPE_MASK	0x0000FFFFU
#define	APFS_OBJ_STORAGE_MASK	0xC0000000U
#define	APFS_OBJ_VIRTUAL	0x00000000U
#define	APFS_OBJ_EPHEMERAL	0x80000000U
#define	APFS_OBJ_PHYSICAL	0x40000000U

#define	APFS_OBJ_NX_SUPERBLOCK	0x00000001U
#define	APFS_OBJ_BTREE_ROOT	0x00000002U
#define	APFS_OBJ_BTREE_NODE	0x00000003U
#define	APFS_OBJ_OMAP		0x0000000BU
#define	APFS_OBJ_FS		0x0000000DU	/* volume superblock */

/*
 * Header every metadata block begins with.  o_cksum covers the block from
 * o_oid onward, so it is deliberately NOT part of what it protects.
 */
struct apfs_obj_phys {
	uint64_t	o_cksum;
	uint64_t	o_oid;
	uint64_t	o_xid;
	uint32_t	o_type;
	uint32_t	o_subtype;
};

/*
 * Container superblock.  Field offsets are dictated by the published format;
 * the kernel is amd64-only and the format is little-endian, so the struct is
 * read straight out of the block with no byte swapping.  The static asserts
 * below pin the offsets that were verified against a real container.
 */
struct apfs_nx_superblock {
	struct apfs_obj_phys	nx_o;
	uint32_t		nx_magic;
	uint32_t		nx_block_size;
	uint64_t		nx_block_count;
	uint64_t		nx_features;
	uint64_t		nx_readonly_compat;
	uint64_t		nx_incompat;
	uint8_t			nx_uuid[16];
	uint64_t		nx_next_oid;
	uint64_t		nx_next_xid;
	uint32_t		nx_xp_desc_blocks;
	uint32_t		nx_xp_data_blocks;
	uint64_t		nx_xp_desc_base;
	uint64_t		nx_xp_data_base;
	uint32_t		nx_xp_desc_next;
	uint32_t		nx_xp_data_next;
	uint32_t		nx_xp_desc_index;
	uint32_t		nx_xp_desc_len;
	uint32_t		nx_xp_data_index;
	uint32_t		nx_xp_data_len;
	uint64_t		nx_spaceman_oid;
	uint64_t		nx_omap_oid;
	uint64_t		nx_reaper_oid;
	uint32_t		nx_test_type;
	uint32_t		nx_max_file_systems;
	uint64_t		nx_fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
};

_Static_assert(sizeof(struct apfs_obj_phys) == 32,
    "obj_phys is a 32-byte on-disk header");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_magic) == 32,
    "nx_magic sits at +32 -- verified against a real container");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_xp_desc_base)
    == 112, "nx_xp_desc_base sits at +112");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_omap_oid)
    == 160, "nx_omap_oid sits at +160");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_fs_oid)
    == 184, "nx_fs_oid[] starts at +184");

/*
 * Object map.  A PHYSICAL oid is already a block number, but a VIRTUAL one
 * is not: copy-on-write moves an object without changing its identity, so
 * the mapping oid -> block lives in this B-tree and is looked up by
 * (oid, xid).  Keying on the transaction too is what lets several versions
 * of the same object coexist -- that is how snapshots work.
 */
struct apfs_omap_phys {
	struct apfs_obj_phys	om_o;
	uint32_t		om_flags;
	uint32_t		om_snap_count;
	uint32_t		om_tree_type;
	uint32_t		om_snapshot_tree_type;
	uint64_t		om_tree_oid;
	uint64_t		om_snapshot_tree_oid;
	uint64_t		om_most_recent_snap;
	uint64_t		om_pending_revert_min;
	uint64_t		om_pending_revert_max;
};

struct apfs_omap_key {
	uint64_t	ok_oid;
	uint64_t	ok_xid;
};

struct apfs_omap_val {
	uint32_t	ov_flags;
	uint32_t	ov_size;
	uint64_t	ov_paddr;
};

/*
 * B-tree node.  One shape serves every tree in the format; only the key and
 * value types differ.  The body after this 56-byte header is laid out as:
 *
 *	[ table of contents ]	at btn_table_space.off, one entry per key
 *	[ keys ]		growing UP from the end of the table space
 *	[ free space ]
 *	[ values ]		growing DOWN from the end of the NODE
 *	[ btree_info, 40 B ]	present only in a root node
 *
 * So a value offset is subtracted from the end of the node -- minus the
 * trailing info struct when the node is also the root.  Key and value
 * offsets are 16-bit and relative, which is why a node cannot exceed 64 KiB.
 */
#define	APFS_BTNODE_ROOT		0x0001
#define	APFS_BTNODE_LEAF		0x0002
#define	APFS_BTNODE_FIXED_KV_SIZE	0x0004

#define	APFS_BTNODE_HDR_SIZE		56
#define	APFS_BTREE_INFO_SIZE		40

struct apfs_nloc {
	uint16_t	nl_off;
	uint16_t	nl_len;
};

struct apfs_btree_node_phys {
	struct apfs_obj_phys	btn_o;
	uint16_t		btn_flags;
	uint16_t		btn_level;
	uint32_t		btn_nkeys;
	struct apfs_nloc	btn_table_space;
	struct apfs_nloc	btn_free_space;
	struct apfs_nloc	btn_key_free_list;
	struct apfs_nloc	btn_val_free_list;
};

/* Fixed-size-KV table entry; the variable-size form is a pair of nlocs. */
struct apfs_kvoff {
	uint16_t	k;
	uint16_t	v;
};

struct apfs_kvloc {
	struct apfs_nloc	k;
	struct apfs_nloc	v;
};

/* 'APSB' little-endian at offset 32 of a volume superblock. */
#define	APFS_APSB_MAGIC		0x42535041U
#define	APFS_VOLNAME_LEN	256

struct apfs_wrapped_meta_crypto_state {
	uint16_t	wmcs_major_version;
	uint16_t	wmcs_minor_version;
	uint32_t	wmcs_cpflags;
	uint32_t	wmcs_persistent_class;
	uint32_t	wmcs_key_os_version;
	uint16_t	wmcs_key_revision;
	uint16_t	wmcs_unused;
};

struct apfs_modified_by {
	uint8_t		am_id[32];
	uint64_t	am_timestamp;
	uint64_t	am_last_xid;
};

/*
 * Volume superblock.  A container holds up to nx_max_file_systems of these,
 * each an independent filesystem sharing the container's free space -- the
 * "space sharing" that lets macOS ship System and Data as separate volumes
 * without partitioning.  We mount volume 0.
 */
struct apfs_superblock {
	struct apfs_obj_phys	apfs_o;
	uint32_t		apfs_magic;
	uint32_t		apfs_fs_index;
	uint64_t		apfs_features;
	uint64_t		apfs_readonly_compat;
	uint64_t		apfs_incompat;
	uint64_t		apfs_unmount_time;
	uint64_t		apfs_fs_reserve_block_count;
	uint64_t		apfs_fs_quota_block_count;
	uint64_t		apfs_fs_alloc_count;
	struct apfs_wrapped_meta_crypto_state	apfs_meta_crypto;
	uint32_t		apfs_root_tree_type;
	uint32_t		apfs_extentref_tree_type;
	uint32_t		apfs_snap_meta_tree_type;
	uint64_t		apfs_omap_oid;
	uint64_t		apfs_root_tree_oid;
	uint64_t		apfs_extentref_tree_oid;
	uint64_t		apfs_snap_meta_tree_oid;
	uint64_t		apfs_revert_to_xid;
	uint64_t		apfs_revert_to_sblock_oid;
	uint64_t		apfs_next_obj_id;
	uint64_t		apfs_num_files;
	uint64_t		apfs_num_directories;
	uint64_t		apfs_num_symlinks;
	uint64_t		apfs_num_other_fsobjects;
	uint64_t		apfs_num_snapshots;
	uint64_t		apfs_total_blocks_alloced;
	uint64_t		apfs_total_blocks_freed;
	uint8_t			apfs_vol_uuid[16];
	uint64_t		apfs_last_mod_time;
	uint64_t		apfs_fs_flags;
	struct apfs_modified_by	apfs_formatted_by;
	struct apfs_modified_by	apfs_modified_by[8];
	uint8_t			apfs_volname[APFS_VOLNAME_LEN];
};

_Static_assert(sizeof(struct apfs_omap_key) == 16, "omap key is 16 bytes");
_Static_assert(sizeof(struct apfs_omap_val) == 16, "omap value is 16 bytes");
_Static_assert(__builtin_offsetof(struct apfs_omap_phys, om_tree_oid) == 48,
    "om_tree_oid sits at +48");
_Static_assert(sizeof(struct apfs_btree_node_phys) == APFS_BTNODE_HDR_SIZE,
    "a B-tree node header is 56 bytes");
_Static_assert(sizeof(struct apfs_modified_by) == 48,
    "apfs_modified_by is 48 bytes");
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_omap_oid) == 128,
    "apfs_omap_oid sits at +128");
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_root_tree_oid)
    == 136, "apfs_root_tree_oid sits at +136");
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_num_files)
    == 184, "apfs_num_files sits at +184");
/*
 * The offset that proves the whole struct: on a container formatted with
 * -L style9, reading a string here yields exactly "style9".
 */
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_volname) == 704,
    "apfs_volname sits at +704");

#define	FS_APFS_E_OK		0
#define	FS_APFS_E_NOMOUNT	(-1)	/* no APFS container mounted    */
#define	FS_APFS_E_IO		(-2)	/* block read failed            */
#define	FS_APFS_E_NOMEM		(-3)	/* kmalloc failed               */
#define	FS_APFS_E_INVAL		(-4)	/* not APFS / unsupported shape */
#define	FS_APFS_E_CKSUM		(-5)	/* Fletcher-64 mismatch         */

/*
 * Probe the first ATA drive for an APFS container and adopt the newest valid
 * checkpoint superblock.  Called once at boot, after ata_drv_init and
 * kmem_init.  Logs the container geometry on success and a one-line reason on
 * failure; a failed probe simply leaves APFS unavailable.
 */
void	fs_apfs_init(void);

/* Non-zero once a container is mounted. */
int	fs_apfs_ready(void);

/*
 * Translate a virtual object id to its block number through the object-map
 * B-tree rooted at `tree_bno`, taking the newest version no later than `xid`.
 * Returns FS_APFS_E_OK and stores the block in *paddr_out, or a negative
 * FS_APFS_E_*.  Both the container omap (which finds volumes) and each
 * volume's own omap (which finds its trees) are read with this.
 */
int	fs_apfs_omap_lookup(uint64_t tree_bno, uint64_t oid, uint64_t xid,
	    uint64_t *paddr_out);

/*
 * Read APFS block `bno` into `buf` (which must hold a whole block) and verify
 * its Fletcher-64.  Returns FS_APFS_E_OK, or a negative FS_APFS_E_*.  Exposed
 * because every layer above -- omap, B-trees, volume superblocks -- reads
 * blocks exactly this way.
 */
int	fs_apfs_read_block(uint64_t bno, void *buf);

/*
 * APFS Fletcher-64 over `len` bytes at `p` (`len` a multiple of 4).  Callers
 * pass block+8 / blocksize-8: the stored checksum is not part of its own sum.
 */
uint64_t	fs_apfs_fletcher64(const void *p, uint32_t len);

#endif /* !_SYS_FS_APFS_H_ */
