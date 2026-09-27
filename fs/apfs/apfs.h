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
 * APFS, the Darwin personality's filesystem: a reader and a copy-on-write
 * writer (see "writing" below) for Apple's published on-disk format.
 *
 * Three ideas carry the format:
 *
 *	1. Every metadata block starts with an obj_phys header: a Fletcher-64
 *	   of the rest of the block, the object's id (oid) and the transaction
 *	   that wrote it (xid).  Nothing is trusted unchecked -- a half-written
 *	   block is what a copy-on-write filesystem expects after a crash.
 *
 *	2. There is no fixed superblock.  Block 0 is only an anchor; the live
 *	   container superblock is the newest copy in the checkpoint
 *	   descriptor ring that still checksums.  A commit writes the new
 *	   state elsewhere and then lands one superblock, so an interrupted
 *	   write is a no-op rather than corruption.
 *
 *	3. Objects are addressed indirectly.  A physical oid is a block
 *	   number; a virtual oid is translated through an object map (omap)
 *	   B-tree, because copy-on-write moves objects without renaming them.
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
#define	APFS_OBJ_SPACEMAN	0x00000005U
#define	APFS_OBJ_SPACEMAN_CIB	0x00000007U	/* chunk-info block   */
#define	APFS_OBJ_SPACEMAN_CAB	0x00000008U	/* chunk-info address */
#define	APFS_OBJ_OMAP		0x0000000BU
#define	APFS_OBJ_CHECKPOINT_MAP	0x0000000CU
#define	APFS_OBJ_FS		0x0000000DU	/* volume superblock */
#define	APFS_OBJ_NX_REAPER	0x00000011U

/* o_subtype of a B-tree that is one of the space manager's free queues. */
#define	APFS_OBJ_SPACEMAN_FREE_QUEUE	0x00000009U

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
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_spaceman_oid)
    == 152, "nx_spaceman_oid sits at +152");

/*
 * Ephemeral objects.  An ephemeral oid is neither a block number nor an
 * omap key: the object lives in memory while mounted and is written to the
 * checkpoint data area when a checkpoint commits, so its block belongs to
 * the checkpoint.  Checkpoint-map blocks, just before the checkpoint's
 * superblock in the descriptor ring, say where each one went.  Nothing a
 * file read touches is ephemeral; the space manager is, and it alone knows
 * which blocks are free.
 */
struct apfs_checkpoint_mapping {
	uint32_t	cpm_type;
	uint32_t	cpm_subtype;
	uint32_t	cpm_size;	/* bytes; one block in every case seen */
	uint32_t	cpm_pad;
	uint64_t	cpm_fs_oid;
	uint64_t	cpm_oid;
	uint64_t	cpm_paddr;
};

_Static_assert(sizeof(struct apfs_checkpoint_mapping) == 40,
    "a checkpoint mapping is 40 bytes on disk");

/* CHECKPOINT_MAP_LAST: this block is the last map of its checkpoint. */
#define	APFS_CPM_LAST		0x00000001U

struct apfs_checkpoint_map_phys {
	struct apfs_obj_phys		cpm_o;
	uint32_t			cpm_flags;
	uint32_t			cpm_count;
	struct apfs_checkpoint_mapping	cpm_map[];
};

_Static_assert(__builtin_offsetof(struct apfs_checkpoint_map_phys, cpm_map)
    == 40, "the mappings start at +40");

/* Ceiling on mappings in one block, from the block size.  (4096-40)/40. */
#define	APFS_CPM_MAX_PER_BLOCK	((APFS_BLOCK_SIZE - 40) / 40)

/*
 * The space manager.  Two devices (main and tier2; only main is non-empty
 * here), each divided into chunks of sm_blocks_per_chunk blocks, so that a
 * chunk's allocation bitmap is exactly one block: 32768 bits, 32768 blocks.
 *
 * Allocating is a bitmap edit; freeing is an insert into a free-queue
 * B-tree keyed by the freeing transaction, and the blocks become available
 * again only once that transaction is old enough.
 */
struct apfs_spaceman_device {
	uint64_t	sm_block_count;
	uint64_t	sm_chunk_count;
	uint32_t	sm_cib_count;	/* chunk-info blocks  */
	uint32_t	sm_cab_count;	/* chunk-info address blocks */
	uint64_t	sm_free_count;
	uint32_t	sm_addr_offset;	/* into this struct's own block */
	uint32_t	sm_reserved;
	uint64_t	sm_reserved2;
};

_Static_assert(sizeof(struct apfs_spaceman_device) == 48,
    "a spaceman device record is 48 bytes on disk");

struct apfs_spaceman_free_queue {
	uint64_t	sfq_count;		/* blocks queued for release */
	uint64_t	sfq_tree_oid;		/* the B-tree holding them   */
	uint64_t	sfq_oldest_xid;		/* nothing older is queued   */
	uint16_t	sfq_tree_node_limit;
	uint16_t	sfq_pad16;
	uint32_t	sfq_pad32;
	uint64_t	sfq_reserved;
};

_Static_assert(sizeof(struct apfs_spaceman_free_queue) == 40,
    "a free-queue record is 40 bytes on disk");

#define	APFS_SD_MAIN		0
#define	APFS_SD_TIER2		1
#define	APFS_SD_COUNT		2

#define	APFS_SFQ_IP		0	/* internal pool */
#define	APFS_SFQ_MAIN		1
#define	APFS_SFQ_TIER2		2
#define	APFS_SFQ_COUNT		3

/*
 * Only as far as the internal pool's bitmap ring; the rest of the block is
 * not used here.  A prefix overlaid on the block, never written by itself.
 */
struct apfs_spaceman {
	struct apfs_obj_phys		sm_o;
	uint32_t			sm_block_size;
	uint32_t			sm_blocks_per_chunk;
	uint32_t			sm_chunks_per_cib;
	uint32_t			sm_cibs_per_cab;
	struct apfs_spaceman_device	sm_dev[APFS_SD_COUNT];
	uint32_t			sm_flags;
	uint32_t			sm_ip_bm_tx_multiplier;
	uint64_t			sm_ip_block_count;
	uint32_t			sm_ip_bm_size_in_blocks;
	uint32_t			sm_ip_bm_block_count;
	uint64_t			sm_ip_bm_base;
	uint64_t			sm_ip_base;
	uint64_t			sm_fs_reserve_block_count;
	uint64_t			sm_fs_reserve_alloc_count;
	struct apfs_spaceman_free_queue	sm_fq[APFS_SFQ_COUNT];

	/*
	 * The internal pool (sm_ip_base) holds the chunk bitmaps and
	 * chunk-info blocks, which cannot live in the space they account
	 * for.  The pool's own usage bitmap is copied, never overwritten, so
	 * it lives in a ring of sm_ip_bm_block_count slots at sm_ip_bm_base.
	 * One slot is live; the free ones are a list threaded through the u16
	 * table at sm_ip_bm_free_next_offset, from sm_ip_bm_free_head to
	 * sm_ip_bm_free_tail, 0xFFFF ending it.  A checkpoint writes its
	 * bitmap into the head slot and returns the one it replaced to the
	 * tail, so earlier checkpoints' bitmaps survive as long as the ring
	 * is deep.
	 *
	 * The xid and bitmap tables say which slot is live and for which xid.
	 * All three offsets are byte offsets into this block.
	 */
	uint16_t			sm_ip_bm_free_head;
	uint16_t			sm_ip_bm_free_tail;
	uint32_t			sm_ip_bm_xid_offset;
	uint32_t			sm_ip_bitmap_offset;
	uint32_t			sm_ip_bm_free_next_offset;
};

/*
 * Offsets read off obj/style9.apfs.  The check that they are right, not
 * just plausible: the two free-queue tree oids read through them are the
 * ones the checkpoint map lists as free-queue B-trees.
 */
_Static_assert(__builtin_offsetof(struct apfs_spaceman, sm_dev) == 48,
    "sm_dev[] starts at +48");
_Static_assert(__builtin_offsetof(struct apfs_spaceman, sm_flags) == 144,
    "sm_flags sits at +144");
_Static_assert(__builtin_offsetof(struct apfs_spaceman, sm_ip_bm_base) == 168,
    "sm_ip_bm_base sits at +168");
_Static_assert(__builtin_offsetof(struct apfs_spaceman, sm_ip_bm_free_head)
    == 320, "sm_ip_bm_free_head sits at +320 -- measured on a real spaceman, "
    "where head=4 tail=2 while ring slot 3 was live");
_Static_assert(__builtin_offsetof(struct apfs_spaceman, sm_ip_bm_xid_offset)
    == 324, "sm_ip_bm_xid_offset sits at +324");
_Static_assert(__builtin_offsetof(struct apfs_spaceman, sm_fq) == 200,
    "sm_fq[] starts at +200");

/*
 * Where the bitmaps are.  sm_dev[].sm_addr_offset is a byte offset into the
 * space manager's own block, of an array of block numbers: the chunk-info
 * blocks themselves, or, with chunk-info address blocks, one more level of
 * indirection (never reached at this container size).  Each chunk-info
 * block describes up to sm_chunks_per_cib chunks, and each chunk names the
 * one block holding its bitmap, one bit per block.  Both live in the
 * internal pool.
 *
 * Measured rather than assumed:
 *
 *	A clear bit means the block is free; a set bit means it is in use.
 *
 *	ci_bitmap_addr == 0 means the chunk has no bitmap because every block
 *	  in it is free.  A fresh container is mostly these.
 */
struct apfs_chunk_info {
	uint64_t	ci_xid;		/* transaction that last changed it */
	uint64_t	ci_addr;	/* first block of the chunk         */
	uint32_t	ci_block_count;
	uint32_t	ci_free_count;
	uint64_t	ci_bitmap_addr;	/* 0 = wholly free, no bitmap       */
};

_Static_assert(sizeof(struct apfs_chunk_info) == 32,
    "a chunk_info is 32 bytes on disk");

struct apfs_chunk_info_block {
	struct apfs_obj_phys	cib_o;
	uint32_t		cib_index;
	uint32_t		cib_chunk_info_count;
	struct apfs_chunk_info	cib_chunk_info[];
};

_Static_assert(__builtin_offsetof(struct apfs_chunk_info_block,
    cib_chunk_info) == 40, "chunk_info[] starts at +40");

/* Ceiling from the block size: (4096-40)/32. */
#define	APFS_CI_MAX_PER_CIB	((APFS_BLOCK_SIZE - 40) / 32)

/*
 * Object map: virtual oid -> block, a B-tree looked up by (oid, xid).
 * Keying on the transaction lets several versions of one object coexist,
 * which is how snapshots work.
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

/*
 * The trailing btree_info holds flags, node size, key size, value size,
 * longest key, longest value, then the key and node counts.  The writer
 * keeps the counts true, by offset: an insert moves the key count only, a
 * split moves the node count only (its records were already counted).
 * Either one wrong draws "Catalog: wrong key count in info footer".
 */
#define	APFS_BTREE_INFO_KEYCOUNT	24
#define	APFS_BTREE_INFO_NODECOUNT	32

/*
 * The longest key and value are high-water marks: apfsck accepts a footer
 * claiming more than any record needs and refuses one claiming less
 * ("Catalog: wrong maximum key size in info footer"; measured with
 * tools/apfspoke.py).  So they are raised when a record exceeds them and
 * never lowered, which would take a whole-tree walk after every delete.
 */
#define	APFS_BTREE_INFO_LONGKEY		16
#define	APFS_BTREE_INFO_LONGVAL		20

/*
 * "No such offset", in a node's free-list heads.  A zero would name the first
 * byte of the key area, which a checker walking the chain reads as a hole
 * header sitting on top of a live record.
 */
#define	APFS_BTOFF_INVALID		0xFFFFU

/*
 * A free-queue record: which transaction released the run, where it starts,
 * and how many blocks it is.  The count is stored as an ordinary value --
 * except when it is one, where the table of contents holds 0xFFFF in place of
 * a value offset and no value is stored at all.  Both forms appear in a
 * container as it comes from mkapfs.
 */
struct apfs_spaceman_free_queue_key {
	uint64_t	sfqk_xid;
	uint64_t	sfqk_paddr;
};

_Static_assert(sizeof(struct apfs_spaceman_free_queue_key) == 16,
    "a free-queue key is 16 bytes -- measured, with a fixed-KV node");

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

/*
 * File-system records.  A volume keeps inodes, directory entries, extents
 * and extended attributes in ONE B-tree, told apart by the record type
 * packed into the top 4 bits of the key's first word -- the low 60 bits are
 * the object id.  Records sort by object id FIRST and type second, which is
 * the opposite of what a raw 64-bit compare of that word would do; any
 * search has to unpack before comparing.
 */
#define	APFS_J_OBJ_ID_MASK	0x0FFFFFFFFFFFFFFFULL
#define	APFS_J_OBJ_TYPE_SHIFT	60

#define	APFS_TYPE_EXTENT	2	/* physical extent, in the ref tree */
#define	APFS_TYPE_INODE		3
#define	APFS_TYPE_XATTR		4
#define	APFS_TYPE_DSTREAM_ID	6
#define	APFS_TYPE_FILE_EXTENT	8
#define	APFS_TYPE_DIR_REC	9

/* The root directory's object id is fixed by the format. */
#define	APFS_ROOT_DIR_INO	2

/*
 * So is the private directory's.  No path reaches it; it holds files whose
 * last name was taken while a descriptor still had them open (apfsck calls
 * them orphans), until the last close.  What an orphan must look like, found
 * one apfsck refusal at a time:
 *
 *	- its entry here is named "0x%llx-dead", the object id in lower-case
 *	  hex, or "Orphan inode: wrong name";
 *	- the inode's ai_parent_id is not this directory, or "Inode record:
 *	  parent is private directory".  It names the root, the one parent
 *	  that cannot be removed while it waits (a dangling one reads as
 *	  "Inode record: free inode number in use" once rmdir'd);
 *	- the link count is zero, or "Orphan inode: has a link count".
 *
 * No ai_internal_flags bit is wanted.
 */
#define	APFS_PRIV_DIR_INO	3

/* "0x" + 16 hex digits + "-dead" + NUL -- no name of ours collides. */
#define	APFS_ORPHAN_NAME_MAX	24

/*
 * Directory-entry keys come in two shapes, picked by the volume's
 * incompatible feature flags.  A case- or normalization-insensitive volume
 * stores a 22-bit hash of the name with its length and orders a directory's
 * entries by that hash; a plain volume stores the length and orders by name.
 * fs/apfs/apfs.c computes the hash and descends on the key; a name it cannot
 * fold (anything outside ASCII) is found by reading every record instead,
 * which is exact but costlier.
 */
#define	APFS_INCOMPAT_CASE_INSENSITIVE		0x00000001ULL
#define	APFS_INCOMPAT_NORM_INSENSITIVE		0x00000008ULL
#define	APFS_DREC_LEN_MASK			0x000003FFU

/*
 * The hash takes the 22 bits above the 10-bit length.  Which hash, and how
 * it was recovered (no specification says), is beside the code in apfs.c.
 */
#define	APFS_DREC_HASH_SHIFT			10
#define	APFS_DREC_HASH_BITS			0x003FFFFFU

/* j_drec_val_t.flags low bits: the dirent type, BSD DT_* numbering. */
#define	APFS_DT_DIR		4
#define	APFS_DT_REG		8

/*
 * Packed: the on-disk record is 18 bytes, natural alignment would make it
 * 24, and a length check against 24 would reject every real entry.
 */
struct apfs_drec_val {
	uint64_t	dv_file_id;
	uint64_t	dv_date_added;
	uint16_t	dv_flags;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_drec_val) == 18,
    "a directory-entry record is 18 bytes before its extended fields");

/*
 * Inode record.  The file's size is not here but in a dstream extended
 * field after the 92-byte fixed part, since a directory needs none.
 * Packed: ai_uncompressed_size sits at +84, not 8-byte aligned.
 */
struct apfs_inode_val {
	uint64_t	ai_parent_id;
	uint64_t	ai_private_id;
	uint64_t	ai_create_time;
	uint64_t	ai_mod_time;
	uint64_t	ai_change_time;
	uint64_t	ai_access_time;
	uint64_t	ai_internal_flags;
	int32_t		ai_nchildren_or_nlink;
	uint32_t	ai_default_protection_class;
	uint32_t	ai_write_generation_counter;
	uint32_t	ai_bsd_flags;
	uint32_t	ai_owner;
	uint32_t	ai_group;
	uint16_t	ai_mode;
	uint16_t	ai_pad1;
	uint64_t	ai_uncompressed_size;
} __attribute__((packed));

/*
 * ai_mode is a POSIX mode_t as stored.  Its type also says what
 * ai_nchildren_or_nlink counts: children on a directory, links otherwise.
 */
#define	APFS_S_IFMT	0170000
#define	APFS_S_IFDIR	0040000
#define	APFS_S_IFREG	0100000

/*
 * Every inode in this container carries NO_RSRC_FORK, as on any volume not
 * made by a Mac; a created inode without it would claim a resource fork it
 * has no record for.
 */
#define	APFS_INODE_NO_RSRC_FORK		0x0000000000008000ULL

_Static_assert(sizeof(struct apfs_inode_val) == 92,
    "the fixed part of an inode record is 92 bytes, extended fields follow");
_Static_assert(__builtin_offsetof(struct apfs_inode_val, ai_mode) == 80,
    "ai_mode sits at +80");

/*
 * Extended fields: a record that needs more than its fixed part appends a
 * count, that many descriptors, then their data, each padded to a multiple
 * of 8.  A file's length lives in the DSTREAM field, so an inode without
 * one names something with no bytes.
 */
#define	APFS_INO_EXT_TYPE_NAME		4
#define	APFS_INO_EXT_TYPE_DSTREAM	8

/*
 * The flags those two carry, copied from the inodes already in this
 * container: a name is not copied when a file is cloned, a dstream is a
 * system field.  Nothing enforces them, so they were read, not reasoned.
 */
#define	APFS_XF_DO_NOT_COPY		0x02
#define	APFS_XF_SYSTEM_FIELD		0x20

struct apfs_xf_blob {
	uint16_t	xb_num_exts;
	uint16_t	xb_used_data;
};

struct apfs_x_field {
	uint8_t		xf_type;
	uint8_t		xf_flags;
	uint16_t	xf_size;
};

struct apfs_dstream {
	uint64_t	ds_size;		/* the file's real length */
	uint64_t	ds_alloced_size;
	uint64_t	ds_default_crypto_id;
	uint64_t	ds_total_bytes_written;
	uint64_t	ds_total_bytes_read;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_xf_blob) == 4, "xf_blob is 4 bytes");
_Static_assert(sizeof(struct apfs_x_field) == 4, "an x_field is 4 bytes");
_Static_assert(sizeof(struct apfs_dstream) == 40, "a dstream is 40 bytes");

/*
 * File extent.  The key is the record header plus the byte offset in the
 * file; the value gives the run's length and first block.  The length is
 * allocated length and may overshoot the file's size (the tail of the last
 * block is garbage), and a physical block of zero is a hole, which reads as
 * zeroes.  Packed: values sit wherever the node's value area puts them.
 */
#define	APFS_FILE_EXTENT_LEN_MASK	0x00FFFFFFFFFFFFFFULL

struct apfs_file_extent_val {
	uint64_t	fe_len_and_flags;
	uint64_t	fe_phys_block_num;
	uint64_t	fe_crypto_id;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_file_extent_val) == 24,
    "a file-extent record is 24 bytes");

/*
 * Physical extent, in the volume's extent reference tree: the reverse of a
 * file extent, saying who owns a run and how many references it has, which
 * is what makes blocks shared between clones countable.  Moving a file's
 * bytes therefore edits both trees.
 *
 * The key is the run's first block, so relocating a run moves its record
 * within the tree; a file extent's key (the file offset) stays put.
 *
 * pe_len_and_kind: length in BLOCKS in the low 60 bits, kind in the top 4.
 * The file extent for the same run counts BYTES.
 */
#define	APFS_PEXT_LEN_MASK	0x0FFFFFFFFFFFFFFFULL
#define	APFS_PEXT_KIND_SHIFT	60
#define	APFS_PEXT_KIND_NEW	1

struct apfs_phys_ext_val {
	uint64_t	pe_len_and_kind;
	uint64_t	pe_owning_obj_id;
	int32_t		pe_refcnt;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_phys_ext_val) == 20,
    "a physical-extent record is 20 bytes");

/* Longest name fs_apfs_readdir reports. */
#define	FS_APFS_NAME_MAX	255

/* Largest file the slurp path will read (bounds the kmalloc). */
#define	FS_APFS_MAX_FILE	(4u * 1024u * 1024u)

/*
 * One directory entry as fs_apfs_readdir reports it: the fields of
 * fs_fat_dirent, with APFS's 64-bit inode number and size.
 */
struct fs_apfs_dirent {
	uint64_t	ade_ino;
	uint64_t	ade_size;
	uint8_t		ade_is_dir;
	char		ade_name[FS_APFS_NAME_MAX + 1];
};

/*
 * A file's metadata, as fs_apfs_stat reports it.  Sizes are 64-bit as in
 * APFS; the Darwin syscall layer narrows them where its wire format is
 * 32-bit.  Timestamps are APFS nanoseconds since the Unix epoch.  afs_nlink
 * is the file's link count (at least 1), and 1 for a directory, whose field
 * counts children instead.  afs_alloced is the dstream's allocated size,
 * what st_blocks means: what the volume spent, not the file's length.
 */
struct fs_apfs_statbuf {
	uint64_t	afs_size;	/* byte length (0 for a directory) */
	uint64_t	afs_ino;	/* object id                       */
	uint64_t	afs_alloced;	/* bytes on disk (dstream)         */
	uint64_t	afs_mtime_ns;
	uint64_t	afs_atime_ns;
	uint64_t	afs_ctime_ns;
	uint64_t	afs_btime_ns;
	uint32_t	afs_nlink;
	uint32_t	afs_uid;
	uint32_t	afs_gid;
	uint16_t	afs_mode;	/* BSD mode bits from the inode    */
	uint8_t		afs_is_dir;
};

#define	FS_APFS_E_OK		0
#define	FS_APFS_E_NOMOUNT	(-1)	/* no APFS container mounted    */
#define	FS_APFS_E_IO		(-2)	/* block read failed            */
#define	FS_APFS_E_NOMEM		(-3)	/* kmalloc failed               */
#define	FS_APFS_E_INVAL		(-4)	/* not APFS / unsupported shape */
#define	FS_APFS_E_CKSUM		(-5)	/* Fletcher-64 mismatch         */
#define	FS_APFS_E_NOTFOUND	(-6)	/* name absent / not a dir      */
#define	FS_APFS_E_TOOBIG	(-7)	/* file exceeds FS_APFS_MAX_FILE */
#define	FS_APFS_E_NOALLOC	(-8)	/* beyond what this writer does  */
#define	FS_APFS_E_EXIST		(-9)	/* the name is already taken     */
#define	FS_APFS_E_ISDIR		(-10)	/* ...and it is a directory      */
#define	FS_APFS_E_SPREAD	(-11)	/* records span more leaves than
					   this edit can hold at once   */
#define	FS_APFS_E_NOTDIR	(-12)	/* ...and it is NOT a directory  */
#define	FS_APFS_E_NOTEMPTY	(-13)	/* a directory that still holds a
					   name, which nothing may remove */
#define	FS_APFS_E_GONE		(-14)	/* a checkpoint the free queue has
					   let go of; no longer readable  */

/*
 * Probe the first ATA drive for an APFS container and adopt the newest valid
 * checkpoint superblock.  Called once at boot, after ata_drv_init, bio_init
 * and kmem_init.  Logs the container geometry on success and a one-line
 * reason on failure; a failed probe leaves APFS unavailable.
 */
void	fs_apfs_init(void);

/* Non-zero once a container is mounted. */
int	fs_apfs_ready(void);

/*
 * Print the mount's counters: tree reads (keyed descents counted apart from
 * whole-tree walks, since one costs the tree's depth and the other the
 * volume's size), nodes, records and key compares; then space, free queues,
 * checkpoints, views, the internal pool and the tree's shape.
 */
void	fs_apfs_stats(void);

/*
 * Take a run of free blocks, confirm the disk agrees, and give it back.
 * The run is never kept: a block marked in use that nothing references is
 * invalid to apfsck.  Proves the live checkpoint does not see the take until
 * a checkpoint publishes it, that a released run stays held for
 * APFS_FQ_KEEP checkpoints and then comes free, that the free queue's node
 * reuses its own holes, and that a second chunk can be used.
 */
void	fs_apfs_alloc_selftest(void);

/*
 * Close the open transaction: write a checkpoint.
 *
 * Releases what the free queue has held long enough, flushes the allocation
 * metadata, re-emits the ephemeral objects into the next free slots of the
 * data ring, writes a checkpoint map naming where they landed, and closes it
 * with a superblock carrying the next xid.  The superblock's landing is the
 * commit: before it the container is the previous checkpoint entire, after
 * it the new one, edits of the open transaction included.  Block zero is
 * then made a copy of that superblock, without which fsck calls the
 * container interrupted rather than clean.  Cache flushes before and after
 * the superblock, and after block zero, make that order the disk's; on
 * FS_APFS_E_OK the checkpoint is on the platter.
 *
 * Returns FS_APFS_E_OK, or a negative FS_APFS_E_* with nothing committed --
 * except two failures past the superblock write, when the checkpoint has
 * happened and this kernel continues from it: a refused flush after the
 * superblock is returned as FS_APFS_E_IO (the platter is not promised),
 * and a failure to update block zero is reported and not propagated.
 *
 * The caller must hold the volume lock: this moves state the readers use.
 */
int	fs_apfs_checkpoint(void);

/*
 * The two questions a checkpoint policy asks; the policy lives elsewhere.
 *
 * fs_apfs_dirty: is there an open transaction -- edits since the last
 * checkpoint, readable through the writer's own view but reachable from no
 * superblock yet?  A sync of a clean container can skip the disk.
 *
 * fs_apfs_ckpt_due: is the open transaction as large as the container can
 * safely absorb?  The bound is the free queue's single node (see the
 * definition).  A policy that checkpoints when this fires, and otherwise
 * for its own reasons, never runs the queue into its overflow fallback.
 *
 * Both read mount state under the volume lock.
 */
int	fs_apfs_dirty(void);
int	fs_apfs_ckpt_due(void);

/*
 * Write two checkpoints and interrogate the disk after each: block zero
 * moved, the ring's newest superblock is the one just written, the one it
 * replaced still reads with its own xid, and every object the new map names
 * is where it says.  Two, because a writer that forgets to advance its
 * cursors passes the first and overwrites it with the second.
 */
void	fs_apfs_ckpt_selftest(void);

/*
 * Write to a file and prove the bytes moved: the run the previous checkpoint
 * names reads exactly as it did, the new run carries the write.
 * Restores what it found.  The path is passed in so the one file the write
 * tests use is named in one place.
 */
void	fs_apfs_data_selftest(const char *path);

/*
 * Make a file longer: `ino` names the inode record carrying its length, `id`
 * the dstream its extents are keyed on.  A file with slack in its last block
 * grows into it and only the length changes; otherwise a run is taken,
 * zeroed, and entered in both trees that name a file's blocks (lengthening
 * the last run when the new one touches it).  A full leaf is split once and
 * the grow retried.  Returns FS_APFS_E_OK or a negative FS_APFS_E_*.
 */
int	fs_apfs_grow(uint64_t ino, uint64_t id, uint64_t new_size);

/*
 * Make a file shorter; `ino` and `id` as for fs_apfs_grow.  A run reaching
 * past the new end is shortened, a run entirely past it loses its record in
 * both trees, and the blocks go to the free queue, because checkpoints still
 * on the platter name them.  Cutting inside a block moves only the length.
 * Refuses, out loud, more runs past the new end than one call cuts
 * (FS_APFS_E_NOALLOC) and records spread over more leaves than one edit
 * holds (FS_APFS_E_SPREAD).
 */
int	fs_apfs_truncate(uint64_t ino, uint64_t id, uint64_t new_size);

/*
 * B-tree nodes split since boot, so a test can insist a node really ran out
 * of room rather than the insert having had space all along.
 */
uint64_t fs_apfs_splits(void);

/*
 * Appends that lengthened an existing run instead of adding one, so a test
 * can tell coalescing from quietly spending a record per block.
 */
uint64_t fs_apfs_merges(void);

/*
 * Checkpoints written since boot.  A checkpoint is when the free queue lets
 * go, making holes a first-fit allocator may prefer, so a test claiming
 * appends merge can only judge a window no checkpoint landed in; checkpoints
 * are written by policy, and this is how it finds out.
 */
uint64_t fs_apfs_ckpts(void);

/*
 * Records a truncate shortened, and records it took out of a tree.  Apart,
 * because shortening edits a length in place while dropping shrinks the
 * B-tree, and a test must see the harder half happen.
 */
uint64_t fs_apfs_shortens(void);
uint64_t fs_apfs_drops(void);

/*
 * Put a name into the directory whose object id is `dir`, with an empty file
 * under it, and report the object id it was given.  `now` is the time to
 * stamp, in nanoseconds since the Unix epoch; this layer has no clock.
 *
 * The file gets a dstream with no bytes and no blocks, since an inode with
 * no dstream can never be lengthened.  Returns FS_APFS_E_EXIST if the name
 * is taken, and refuses a name this kernel cannot hash (anything outside
 * ASCII) rather than guess at Apple's folding.
 */
int	fs_apfs_create(uint64_t dir, const char *name, uint64_t now,
	    uint16_t perm, uint64_t *ino_out);

/*
 * Take a name back out, with the file under it.  The blocks go back by
 * truncating to nothing first; then the three records a create made are
 * removed and its two counts undone.  Refuses a directory (FS_APFS_E_ISDIR)
 * and, out loud, an inode with more than one link: this kernel makes no
 * hard links, and removing one of several names is a different operation.
 */
int	fs_apfs_unlink(uint64_t dir, const char *name, uint64_t now);

/*
 * The same two for a directory, sharing their code with the file versions.
 * On disk a directory's inode has no data stream (apfsck objects to one),
 * its entry is typed a directory (apfsck checks the two agree), its child
 * count starts at zero, and it is counted in apfs_num_directories, which
 * unlike apfs_num_files is checked.
 *
 * fs_apfs_rmdir removes a directory that holds nothing, asked of the tree,
 * not of the child count in its record.  Refuses a name that is not a
 * directory (FS_APFS_E_NOTDIR) and one that still holds a name
 * (FS_APFS_E_NOTEMPTY): removing it would leave entries whose parent is gone.
 */
int	fs_apfs_mkdir(uint64_t dir, const char *name, uint64_t now,
	    uint16_t perm, uint64_t *ino_out);
int	fs_apfs_rmdir(uint64_t dir, const char *name, uint64_t now);

/*
 * Move a name, within one directory or between two, taking what is under it
 * -- and over whatever stands at the destination, in the same edit: POSIX
 * wants rename atomic, and a state with the name absent is the one readers
 * must not see.
 *
 * Over a free name, the same inode ends up with another name in another
 * place and the tree holds as many records as before.  The inode record is
 * rebuilt, not amended: it carries the name and the parent, apfsck checks
 * both against the entry, and a name of another length changes its length.
 *
 * A file standing at the destination is orphaned into the private directory
 * (as fs_apfs_orphan does, no extent touched) and its object id returned
 * through *victim_out (which may be NULL; 0 when nothing stood there).
 * Whether it is still open is the caller's question: the caller reaps it at
 * once or leaves it for the last close.  An empty directory standing there
 * is simply removed, and *victim_out stays 0.
 *
 * Refuses FS_APFS_E_NOTFOUND for a name that is not there; FS_APFS_E_ISDIR
 * and FS_APFS_E_NOTDIR when the two ends are not the same kind of thing;
 * FS_APFS_E_NOTEMPTY for a directory that still holds a name, asked of the
 * tree as rmdir asks; and FS_APFS_E_INVAL for the root, an inode with more
 * than one link on either end, or a directory moved into itself.  Renaming
 * a name to itself succeeds and changes nothing; renaming it to another
 * spelling of itself is a real move, so the volume keeps the case asked for.
 */
int	fs_apfs_rename(uint64_t odir, const char *oname, uint64_t ndir,
	    const char *nname, uint64_t now, uint64_t *victim_out);

/*
 * Take a name away from a file that something still holds open.  The entry
 * moves to the private directory under the name the format wants there, and
 * the inode record is rebuilt as for a rename, with the root as parent and a
 * link count of zero.  The bytes are not touched.  Afterwards no path
 * reaches the file and every open descriptor still does.
 *
 * Refuses FS_APFS_E_ISDIR for a directory, whose children would be left
 * under an unreachable parent.  *ino_out gets the object id, the only handle
 * on the file afterwards.
 */
int	fs_apfs_orphan(uint64_t dir, const char *name, uint64_t now,
	    uint64_t *ino_out);

/*
 * Let an orphan go when its last descriptor closes: the work of an unlink,
 * asked for by object id (its private-directory name is derived from it).
 */
int	fs_apfs_reap(uint64_t ino, uint64_t now);

/*
 * Reap everything the private directory still holds.  Called after mounting:
 * orphans left there mean the last boot ended between orphaning a file and
 * reaping it, and they can simply be finished now.  *n_out gets how many,
 * zero after a clean shutdown.
 */
int	fs_apfs_reap_all(uint64_t now, uint32_t *n_out);

/*
 * Files orphaned, and orphans reaped.  They need not balance within a boot:
 * a file orphaned by a kernel that then stopped is reaped by the next one.
 */
uint64_t fs_apfs_orphans(void);
uint64_t fs_apfs_reaps(void);

/*
 * apfs-orphan: writes a file with a length, takes its name away, and checks
 * the length is intact, the path no longer resolves and the object id still
 * does.  Then reaps it and checks the volume is back where it started: an
 * orphan never reaped is a leak apfsck calls valid.
 */
void	fs_apfs_orphan_selftest(uint64_t now);

/*
 * Files made and unmade, and record ends laid into room a delete gave back.
 * The third matters: a node's free span only shrinks, so create and unlink
 * without hole reuse would lose a record's worth of room per cycle, and the
 * volume would stop taking names while reporting thousands of bytes free.
 */
uint64_t fs_apfs_makes(void);
uint64_t fs_apfs_kills(void);
uint64_t fs_apfs_holes(void);

/*
 * The same two for directories.  An error alone proves nothing about a
 * refusal -- a writer that half-did the work and then failed answers the
 * same -- so these say whether the work happened.
 */
uint64_t fs_apfs_dirmakes(void);
uint64_t fs_apfs_dirkills(void);

/*
 * Names moved, and of those, moves onto a taken name.  A rename onto a free
 * name changes no other total, so this is the only number that says it
 * happened; the second tells a replacement from a refusal.
 */
uint64_t fs_apfs_moves(void);
uint64_t fs_apfs_clobbers(void);

/*
 * apfs-move: a name moved within a directory and between two.  A file with a
 * length is moved to a longer name and a shorter one: the length lives in an
 * extended field after the name, and a rebuild that dropped it would leave a
 * valid empty file.  A directory is moved with a child in it.  The refusals
 * are asked for too -- a file onto a directory's name, a missing name, a
 * directory into itself or under its own child, into a non-directory --
 * since a refusal leaves nothing on the volume for a checker to find.
 */
void	fs_apfs_move_selftest(uint64_t now);

/*
 * apfs-clobber: two files with different bytes, one moved onto the other.
 * The name answers with the newcomer's bytes and the occupant waits in the
 * private directory with its own intact (only a reader can tell, since
 * either outcome is a valid volume); the reap returns its blocks.  Then an
 * empty directory replaced outright, a full one refused NOTEMPTY, a file
 * onto a directory ISDIR, a directory onto a file NOTDIR, each refusal
 * leaving no mark.  `now` is the wall clock, which this layer lacks.
 */
void	fs_apfs_clobber_selftest(uint64_t now);

/*
 * The extent reference tree's growth: index levels gained (it starts as a
 * single root node), leaves split under that index, and emptied leaves
 * taken back out.
 */
uint64_t fs_apfs_extref_grows(void);
uint64_t fs_apfs_extref_splits(void);
uint64_t fs_apfs_extref_drops(void);

/*
 * apfs-extref: the extent reference tree outgrows its root and keeps
 * answering.  Two files grow a block at a time in alternation, so no append
 * merges and each is a fresh record; the tree gains a level, a leaf splits,
 * and every record still resolves.  Then both files are cut to nothing,
 * deleting back across the leaves it made.  `now` is the wall clock.
 */
void	fs_apfs_extref_selftest(uint64_t now);

/*
 * Split a leaf on purpose and prove nothing was lost: the same records, in the
 * same order, reachable through the index afterwards.  Asked for directly
 * because a sequential writer no longer fills a node -- it coalesces instead.
 */
void	fs_apfs_split_selftest(void);

/*
 * Make a node stop starting where its parent says it does, and prove the
 * writer notices.  The leaf holding one file's inode record is split at that
 * record, so it becomes that half's index key; the file is unlinked while a
 * second file keeps the half from emptying.  Both files are removed again.
 * `now` is the wall clock in nanoseconds.
 */
void	fs_apfs_index_selftest(uint64_t now);

/*
 * Make a node lose its last record, and prove it leaves the tree rather than
 * staying empty ("B-tree: keys are out of order" from apfsck, since the index
 * still files it under a key it no longer has).  A new file has the highest
 * key on the volume, so splitting its leaf at its inode record leaves that
 * record alone in a node; unlinking the file does the rest.  `now` as above.
 */
void	fs_apfs_drop_selftest(uint64_t now);

/*
 * Put a file's inode record and data stream record in different nodes, by
 * splitting the leaf between them, and prove unlinking takes both: the tree
 * is asked for the stream record afterwards, since an unlink that leaves it
 * behind still reports success.  `now` as above.
 */
void	fs_apfs_stream_selftest(uint64_t now);

/*
 * Fill a leaf and prove a create makes its own room: names go into a
 * directory until the writer has split, plus a couple more; every one must
 * still resolve to its inode and every node start where its parent says.
 * All are removed again.  `now` as above.
 */
void	fs_apfs_room_selftest(uint64_t now);

/*
 * Prove that descending on a key finds what reading every record finds.
 * Every record is sought by its own key and must come back from its leaf
 * with the rest of the tree after it, in order; absent keys must land on the
 * record after them; and the leaf an insert would use must be the one a
 * whole-tree walk names.  Reads only.
 */
void	fs_apfs_seek_selftest(void);

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
 * Resolve an absolute path to its object id, reporting whether it names a
 * directory.  A leading '/' is optional and repeated separators are ignored;
 * "" and "/" both name the root.  Returns FS_APFS_E_OK or a negative
 * FS_APFS_E_* (NOTFOUND for a missing component, or for descending through
 * something that is not a directory).
 */
int	fs_apfs_lookup(const char *path, uint64_t *oid_out, int *is_dir_out);

/*
 * Fill *out with the `index`-th entry of the directory named by `path`.
 * Returns 1 when an entry was written, 0 at end-of-directory, or a negative
 * FS_APFS_E_*.  Enumeration is stateless -- each call re-resolves and
 * re-scans -- matching how fs_fat_readdir behaves and keeping no per-fd
 * cursor in the kernel.
 */
int	fs_apfs_readdir(const char *path, uint32_t index,
	    struct fs_apfs_dirent *out);

/*
 * Report a file-or-directory's metadata without reading its bytes.  Returns
 * FS_APFS_E_OK and fills *out, or a negative FS_APFS_E_*.
 */
int	fs_apfs_stat(const char *path, struct fs_apfs_statbuf *out);

/*
 * Read the whole file named by `path` into a freshly kmalloc'd buffer.  On
 * success returns FS_APFS_E_OK, stores the buffer in *out_buf (the caller
 * kfree's it) and its byte length in *out_size.  Holes read back as zeroes.
 * Returns FS_APFS_E_TOOBIG rather than attempting an unbounded allocation.
 */
int	fs_apfs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size);

/*
 * Resolve `path` to the three things reading and writing it need: the dstream
 * id its extents are keyed on, its byte length, and its inode object id.  The
 * expensive half of reading, paid once instead of per call.  Directories are
 * refused.  Returns FS_APFS_E_OK or a negative FS_APFS_E_*.
 *
 * The dstream id and the inode id are equal on a freshly written volume and
 * diverge as soon as anything is hard-linked, so both are reported: one finds
 * the bytes, the other finds the record that describes them.
 */
int	fs_apfs_open(const char *path, uint64_t *id_out, uint64_t *size_out,
	    uint64_t *ino_out);

/*
 * Read at most `len` bytes of the resolved file (`id`, `size`) starting at
 * file offset `off` into the caller's buffer, reporting the count delivered
 * through *out_got.  Reads that begin at or past end-of-file return
 * FS_APFS_E_OK with zero bytes; reads that run off the end come back short.
 * Holes read back as zeroes.
 */
int	fs_apfs_pread(uint64_t id, uint64_t size, uint64_t off, uint8_t *buf,
	    uint32_t len, uint32_t *out_got);

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

/*
 * Write a metadata block, sealing it first: the Fletcher-64 over the block
 * from offset 8 is stored in the header.  Exported for fs/fs_txn.c, which
 * writes the blocks a transaction collected.  Not for file data, which has
 * no header: its first eight bytes would become a checksum.
 */
int	fs_apfs_write_block(uint64_t bno, void *buf);

/*
 * Read and write a block with no obj_phys header: no checksum verified or
 * sealed.  File data is one such block; an allocation bitmap is another, and
 * its first eight bytes are the state of the chunk's first 64 blocks, which
 * the checked path would reject and the sealed path destroy.
 */
int	fs_apfs_read_block_raw(uint64_t bno, void *buf);
int	fs_apfs_write_block_raw(uint64_t bno, const void *buf);

/* ---- writing ------------------------------------------------------- */

/*
 * The writer is copy-on-write, like Apple's: a changed block, metadata or
 * file data, is written to a newly allocated block, the object map is
 * pointed at the copy, and the old block goes to the free queue.  Edits of
 * the open transaction are readable at once and become durable when
 * fs_apfs_checkpoint lands a superblock naming them; until then a crash
 * mounts the previous checkpoint intact.
 *
 * FS_APFS_E_NOALLOC marks a change this writer does not make: on a container
 * it cannot allocate in, a hole filled or a file extended by a write, or an
 * edit wider than its fixed bounds.
 */

/*
 * Write `len` bytes of the resolved file (`id`, `size`) at file offset `off`,
 * reporting the count written through *out_put.  Each run touched moves to
 * new blocks; partial blocks are read-modify-written, so the bytes around
 * the request survive.
 *
 * Refuses rather than shortens: a write past `size` returns
 * FS_APFS_E_NOALLOC before anything is written, and so does one reaching a
 * hole or a range no extent record covers -- an absent record is silence,
 * not an error, so coverage is checked run by run.
 */
int	fs_apfs_pwrite(uint64_t id, uint64_t size, uint64_t off,
	    const uint8_t *buf, uint32_t len, uint32_t *out_put);

/*
 * Stamp an inode's modification and change times, in nanoseconds since the
 * Unix epoch, as APFS stores them.  The leaf holding the record is copied,
 * patched and sealed into a new block of the open transaction.
 */
int	fs_apfs_touch(uint64_t oid, uint64_t mtime_ns);

/*
 * Set an inode's permission bits, and its change time with them.  Only the
 * low twelve bits are the caller's: the type bits must match the directory
 * entry's type, which apfsck checks.
 */
int	fs_apfs_chmod(uint64_t oid, uint16_t perm, uint64_t now_ns);

/*
 * The current length of the file whose inode object id is `ino`, without
 * resolving a path: one descent, cheap enough to refresh a stale handle on
 * demand.
 */
int	fs_apfs_size(uint64_t ino, uint64_t *size_out);

/*
 * Views: the published past, read back.
 *
 * A checkpoint's superblock stays in the descriptor ring until the ring
 * comes round, and the blocks a later transaction stopped using are held by
 * the free queue for APFS_FQ_KEEP checkpoints.  For that long a checkpoint is
 * a complete, older volume on the platter that nothing points at.
 *
 * A view points at it.  fs_apfs_view_open walks the checkpoint's spine
 * (superblock, container object map, volume superblock, volume object map,
 * file-system root) and records what a reader descends from: the volume
 * object map's tree and the root's block, as of that xid.  Between
 * fs_apfs_view_enter and fs_apfs_view_leave every reader here -- lookup,
 * stat, readdir, slurp, pread -- answers with the volume as that checkpoint
 * left it.
 *
 * A view is not a snapshot: a snapshot holds its blocks, while this object
 * map replaces rather than keeps versions (see omap_replace_cow).  A view
 * borrows blocks for as long as the free queue holds them anyway, a window
 * that slides as the live volume publishes.  A checkpoint the queue has let
 * go of is refused with FS_APFS_E_GONE, since its blocks may belong to
 * something else and file data has no header to say so.  The window's floor
 * is recorded from what the queue actually released, not computed (see
 * fq_floor in apfs.c for why the two can differ).
 *
 * While a view is entered this file writes, allocates and releases nothing,
 * and says so if asked to.  Views do not nest, and are entered and left
 * within one holding of the volume lock, so the writer never sees one.
 */
struct fs_apfs_view {
	uint64_t	av_xid;		/* the checkpoint it reads          */
	uint64_t	av_omap_tree;	/* its volume object map B-tree     */
	uint64_t	av_root_bno;	/* its file-system tree root        */
};

/*
 * Resolve the checkpoint `xid` into a view.  FS_APFS_E_NOTFOUND when no such
 * checkpoint is published -- the open transaction's xid included, since it
 * has no superblock yet -- or when the ring no longer holds its superblock;
 * FS_APFS_E_GONE when the free queue has let its blocks go; FS_APFS_E_INVAL
 * when the spine it walks names a block newer than the checkpoint, which is
 * a block reused out from under it.  Reads only.
 */
int	fs_apfs_view_open(uint64_t xid, struct fs_apfs_view *out);

/*
 * Make every reader answer as of `v` until fs_apfs_view_leave.  Re-checks
 * the floor, since a view resolved earlier may have slid out of the window
 * meanwhile: FS_APFS_E_GONE then, and nothing is entered.  The caller keeps
 * `v` alive until it leaves, and holds the volume lock across both.
 */
int	fs_apfs_view_enter(const struct fs_apfs_view *v);
void	fs_apfs_view_leave(void);

/* The xid of the entered view, or 0 when readers answer for the live volume. */
uint64_t fs_apfs_view_xid(void);

/*
 * Every checkpoint a view could be opened on right now, oldest first: those
 * the ring still holds a superblock for and the free queue still holds the
 * blocks of.  At most `cap` are stored; the count stored comes back through
 * `n_out`.  Reads only.
 */
int	fs_apfs_view_list(uint64_t *xids, uint32_t cap, uint32_t *n_out);

/* The newest published checkpoint -- what a fresh mount would adopt. */
uint64_t fs_apfs_xid(void);

/*
 * The container's size and free space, in blocks.  Read without the volume
 * lock: a count a writer is moving is as true as the next one.
 */
void	fs_apfs_space(uint64_t *blocks, uint64_t *bfree);

/*
 * Prove the published past reads back exactly, and exactly as far back as
 * promised: a file's earlier lengths and bytes through views of the
 * checkpoints that held them, a name's absence through a view from before it
 * was made, the live mount left untouched by all of it, and the window found
 * to be APFS_FQ_KEEP + 1 checkpoints wide with the one beyond it refused.
 * Restores what it found.
 */
void	fs_apfs_view_selftest(uint64_t now);

#endif /* !_SYS_FS_APFS_H_ */
