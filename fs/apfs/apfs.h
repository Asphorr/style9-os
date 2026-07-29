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
 * Read-only, and that is the whole of it: no allocator, no checkpoint writer,
 * no space manager.  Mounting a copy-on-write filesystem for reading is a much
 * smaller claim than writing one, and it is the claim a Darwin personality
 * needs -- a binary opens its data files, it does not reformat the disk.
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
 * THE EPHEMERAL OBJECTS, AND WHY THEY NEED A MAP OF THEIR OWN
 *
 * A physical oid is a block number and a virtual one is a question for the
 * object map, but an EPHEMERAL oid is neither.  Those objects live in memory
 * while the container is mounted and are written to the checkpoint data area
 * only when a checkpoint commits, so their block number is a property of the
 * checkpoint rather than of the object.  The checkpoint says where they went
 * in checkpoint-map blocks, which sit in the descriptor ring immediately
 * before the superblock that closes the same checkpoint.
 *
 * That is why a reader that only wants files can skip them -- nothing an open
 * or a read ever touches is ephemeral -- and why anything that wants to know
 * about SPACE cannot.  The space manager is ephemeral, and it is the only
 * thing that knows which blocks are free.
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
 * THE SPACE MANAGER
 *
 * Two devices (main and tier2; only main is ever non-empty here), each divided
 * into chunks of sm_blocks_per_chunk blocks.  One chunk's allocation bitmap is
 * exactly one block -- 32768 bits for 32768 blocks -- which is the arithmetic
 * the whole layout is built around.
 *
 * The free queues are the asymmetry that makes this filesystem harder to write
 * than to read.  Allocating is an edit to a bitmap; freeing is an INSERT into
 * one of these B-trees, keyed by the transaction that did the freeing, and the
 * blocks do not become available again until that transaction is old enough.
 * A writer that does not maintain xid has nothing honest to put in that key.
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
 * Only as far as the free queues -- everything past them is internal-pool
 * ring bookkeeping this kernel has no use for yet.  The struct is read out of
 * a block, so a short definition reads a prefix; it must never be written.
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
	 * THE INTERNAL POOL'S OWN BOOKKEEPING
	 *
	 * The pool at sm_ip_base holds the blocks that describe allocation --
	 * the chunk bitmaps and the chunk-info blocks -- because those cannot
	 * live in the space they themselves account for.  Which pool blocks
	 * are in use is a bitmap like any other, except that it must be
	 * copied rather than overwritten for the same reason everything else
	 * must: a checkpoint that has not committed yet is still readable.
	 *
	 * So there is a RING of them, sm_ip_bm_block_count slots long, at
	 * sm_ip_bm_base.  One slot is live; the free ones are a linked list
	 * threaded through the u16 table at sm_ip_bm_free_next_offset, from
	 * sm_ip_bm_free_head to sm_ip_bm_free_tail, with 0xFFFF for the end.
	 * A checkpoint takes the head, writes its bitmap there, and returns
	 * the slot it replaced to the tail -- so the ring is a FIFO and the
	 * previous checkpoints' bitmaps survive for as long as it is deep.
	 *
	 * The two small tables say which slot is live and which xid it
	 * belongs to.  All three offsets are byte offsets into this block.
	 */
	uint16_t			sm_ip_bm_free_head;
	uint16_t			sm_ip_bm_free_tail;
	uint32_t			sm_ip_bm_xid_offset;
	uint32_t			sm_ip_bitmap_offset;
	uint32_t			sm_ip_bm_free_next_offset;
};

/*
 * Pinned against a real container rather than against the published layout:
 * every one of these was read out of obj/style9.apfs before it was written
 * here, and the two free-queue tree oids the struct reports are the same two
 * oids the checkpoint map lists as free-queue B-trees, which is the check
 * that the offsets are right rather than merely plausible.
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
 * WHERE THE BITMAPS ACTUALLY ARE
 *
 * sm_dev[].sm_addr_offset is an offset INTO THE SPACE MANAGER'S OWN BLOCK, at
 * an array of block numbers.  With no chunk-info address blocks those are the
 * chunk-info blocks themselves; with them there is one more level of
 * indirection, which a container this size never reaches.
 *
 * Each chunk-info block describes up to sm_chunks_per_cib chunks, and each
 * chunk names the single block holding its allocation bitmap -- one bit per
 * block, and with 32768 blocks to a chunk that bitmap is exactly one 4 KiB
 * block, which is what the whole geometry is arranged to make true.
 *
 * Two conventions here are measured rather than assumed, because both are the
 * kind of thing that is equally plausible either way round:
 *
 *	A CLEAR bit means the block is free; a set bit means it is in use.
 *
 *	ci_bitmap_addr == 0 means the chunk has no bitmap because every block
 *	  in it is free.  A fresh container is mostly these.
 *
 * The bitmaps and the chunk-info blocks live in the space manager's internal
 * pool (sm_ip_base), and they have to: the blocks that record what is
 * allocated cannot themselves be tracked by the records they hold.
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

/*
 * Inside that trailing btree_info: flags, node size, key size, value size,
 * longest key, longest value, then the two counts.  Two of those are written
 * here, by offset rather than through a struct, because they are the fields a
 * writer has to keep true and the rest are the tree's shape.
 *
 * Which of the two moves says what happened.  An INSERT adds a record, so the
 * key count moves and the node count does not; a SPLIT adds a node holding
 * records that were already counted, so the node count moves and the key
 * count does not.  Getting that backwards is not a rounding error -- apfsck
 * answers "Catalog: wrong key count in info footer" either way.
 */
#define	APFS_BTREE_INFO_KEYCOUNT	24
#define	APFS_BTREE_INFO_NODECOUNT	32

/*
 * And two more that a writer has to keep true, which nothing here touched
 * until a rename put a name longer than any on the image into the tree:
 *
 *	Catalog: wrong maximum key size in info footer.
 *
 * They are HIGH-WATER MARKS rather than measurements.  A footer claiming MORE
 * than any record needs is accepted and one claiming less is refused, which
 * was measured with tools/apfspoke.py on a copy of this container rather than
 * assumed -- so they are raised when a record exceeds them and never lowered.
 * Lowering would mean walking the whole tree after every delete to find the
 * new maximum, to tighten a bound no reader needs tight.
 *
 * A create could always have produced this and never did: every name the tests
 * made was shorter than "standard.flf", which the image came with.
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
 * And so is the private directory's, which is what makes a Unix unlink
 * possible on this volume at all.
 *
 * A file whose last name is taken away while a descriptor is still open on it
 * must keep its bytes until that descriptor closes.  Somewhere it has to live
 * in the meantime, and the format says where: this directory, which every
 * volume is formatted with and which no path ever reaches, holds the files
 * that have no name left.  A checker knows what it is looking at in there and
 * calls them ORPHANS.
 *
 * What one has to look like was measured with apfsck rather than read off a
 * layout, one refusal at a time:
 *
 *	- the ENTRY under this directory is named "0x%llx-dead", the object id
 *	  in lower-case hex, or the answer is "Orphan inode: wrong name";
 *	- the inode record's ai_parent_id must NOT be this directory, or the
 *	  answer is "Inode record: parent is private directory".  It names the
 *	  root here, which is the only parent that cannot be removed out from
 *	  under it while it waits -- a dangling one reads as "Inode record:
 *	  free inode number in use" the moment the old directory is rmdir'd;
 *	- the link count must be ZERO, or the answer is "Orphan inode: has a
 *	  link count".  Which is the truth: the entry in here is not a name,
 *	  it is a place to wait, and nothing can open it by walking a path.
 *
 * No flag in ai_internal_flags is wanted, which was measured too.
 */
#define	APFS_PRIV_DIR_INO	3

/* "0x" + 16 hex digits + "-dead" + NUL -- no name of ours collides. */
#define	APFS_ORPHAN_NAME_MAX	24

/*
 * Directory-entry keys come in two shapes and the volume's incompatible
 * feature flags pick which.  A case- or normalization-insensitive volume
 * stores a 22-bit hash of the name alongside its length, and orders entries
 * within a directory BY THAT HASH; a plain volume stores just the length and
 * orders by name.
 *
 * Computing that hash means reproducing Apple's case folding, which is why
 * this reader once refused to and read every record in the tree instead.  It
 * no longer does: the hash is recovered and computed (see fs/apfs/apfs.c), and a
 * name is found by descending on its key like anything else.  The old way is
 * still the fallback for a name this kernel cannot fold -- anything outside
 * ASCII -- and it is still exact, just costlier.
 */
#define	APFS_INCOMPAT_CASE_INSENSITIVE		0x00000001ULL
#define	APFS_INCOMPAT_NORM_INSENSITIVE		0x00000008ULL
#define	APFS_DREC_LEN_MASK			0x000003FFU

/*
 * ...and what a writer needs that a reader did not: the hash occupies the rest
 * of that word, twenty-two bits above the ten the length uses.  Which hash it
 * is, and how it was recovered, is in fs/apfs/apfs.c beside the code that
 * computes it -- the answer was not in any specification.
 */
#define	APFS_DREC_HASH_SHIFT			10
#define	APFS_DREC_HASH_BITS			0x003FFFFFU

/* j_drec_val_t.flags low bits: the dirent type, BSD DT_* numbering. */
#define	APFS_DT_DIR		4
#define	APFS_DT_REG		8

/*
 * Packed, and it matters: the on-disk record is 18 bytes, but the natural
 * alignment of a struct ending in a uint16 after two uint64s would round
 * sizeof up to 24 -- and a length check against that silently rejects every
 * real directory entry.
 */
struct apfs_drec_val {
	uint64_t	dv_file_id;
	uint64_t	dv_date_added;
	uint16_t	dv_flags;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_drec_val) == 18,
    "a directory-entry record is 18 bytes before its extended fields");

/*
 * Inode record.  Note what is NOT here: the file's size.  That lives in a
 * dstream extended field appended after this struct, because a plain
 * directory has no need of one -- so the fixed part stops at 92 bytes and
 * the extended fields follow.  The struct is packed: uncompressed_size sits
 * at offset 84, which is not 8-byte aligned.
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
 * ai_mode is an ordinary POSIX mode_t written straight to disk, so its type
 * field is spelled the way POSIX froze it.  The reader needs exactly one of
 * these bits: ai_nchildren_or_nlink means CHILDREN on a directory and LINKS
 * on anything else, and the mode is what tells the two apart.
 */
#define	APFS_S_IFMT	0170000
#define	APFS_S_IFDIR	0040000
#define	APFS_S_IFREG	0100000

/*
 * ai_internal_flags, of which a writer needs exactly one.  Every file and
 * directory in this container carries NO_RSRC_FORK, which is what a volume
 * made by anything other than a Mac says; a created inode that did not would be
 * claiming a resource fork it has no record for.
 */
#define	APFS_INODE_NO_RSRC_FORK		0x0000000000008000ULL

_Static_assert(sizeof(struct apfs_inode_val) == 92,
    "the fixed part of an inode record is 92 bytes, extended fields follow");
_Static_assert(__builtin_offsetof(struct apfs_inode_val, ai_mode) == 80,
    "ai_mode sits at +80");

/*
 * Extended fields.  A record that needs more than its fixed part appends a
 * small blob of them: a count, then that many descriptors, then the data they
 * describe, each datum padded up to a multiple of 8.  This is how APFS keeps a
 * directory's inode from carrying a file's worth of empty fields -- and it is
 * why a file's LENGTH is not where one would look for it.  Length lives in the
 * DSTREAM field, so an inode without one names something with no bytes.
 */
#define	APFS_INO_EXT_TYPE_NAME		4
#define	APFS_INO_EXT_TYPE_DSTREAM	8

/*
 * And the two flag values those two carry, read off the inodes already in this
 * container rather than chosen: a name is not copied when a file is cloned, a
 * dstream belongs to the system.  The fields are descriptive, not enforced --
 * which is why they had to be measured rather than reasoned about.
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
 * File extent.  The key is the record header followed by the byte offset
 * within the file; the value gives that run's length and the block it starts
 * at.  Two things a reader has to respect: the run is ALLOCATED length and may
 * overshoot the file's real size (the tail of the last block is garbage), and
 * a physical block of zero means a HOLE -- a sparse region that was never
 * written and reads back as zeroes rather than as anything on disk.
 *
 * Packed for the same reason the records above are: these sit at whatever
 * offset the node's value area put them, which is not 8-byte aligned.
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
 * Physical extent, in the volume's EXTENT REFERENCE tree -- the other tree
 * that names a file's blocks, and the reason moving a file's bytes is not a
 * one-tree edit.  The file-system tree answers "where are this file's bytes";
 * this one answers the reverse, "who owns this run and how many references
 * does it have", which is what makes a block shared between clones countable.
 *
 * The KEY is the run's first block, so relocating a run changes the key and
 * therefore where the record sorts -- unlike a file extent, whose key is the
 * offset within the file and does not move at all.
 *
 * pe_len_and_kind packs the length in blocks into the low 60 bits and the kind
 * into the top 4.  Note "in BLOCKS": the file extent above counts BYTES, and
 * the two describing the same run in different units is exactly the sort of
 * thing that reads as correct and is not.
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
 * One directory entry as fs_apfs_readdir reports it.  Deliberately the same
 * shape fs_fat_dirent has, so the Darwin readdir path can be pointed at
 * either filesystem without changing its wire format.
 */
struct fs_apfs_dirent {
	uint64_t	ade_ino;
	uint64_t	ade_size;
	uint8_t		ade_is_dir;
	char		ade_name[FS_APFS_NAME_MAX + 1];
};

/*
 * A file's metadata, as fs_apfs_stat reports it.  Sizes are 64-bit because
 * APFS's are; the Darwin syscall layer narrows them where its own wire format
 * is 32-bit, which is the right place for that decision to be visible.
 *
 * Everything below afs_mode comes out of the inode record's FIXED part, which
 * the tree walk was already reading and this struct was already throwing away
 * -- the timestamps are APFS's own nanoseconds since the Unix epoch, and the
 * link count is nchildren for a directory (Apple stores both in one field and
 * tells them apart by the mode).  afs_alloced is the dstream's allocated size,
 * which is what st_blocks means: what the volume spent, not what the file
 * says it is.
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
#define	FS_APFS_E_NOALLOC	(-8)	/* would need a block allocator  */
#define	FS_APFS_E_EXIST		(-9)	/* the name is already taken     */
#define	FS_APFS_E_ISDIR		(-10)	/* ...and it is a directory      */
#define	FS_APFS_E_SPREAD	(-11)	/* records span more leaves than
					   this edit can hold at once   */
#define	FS_APFS_E_NOTDIR	(-12)	/* ...and it is NOT a directory  */
#define	FS_APFS_E_NOTEMPTY	(-13)	/* a directory that still holds a
					   name, which nothing may remove */

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
 * What reading the file-system tree has cost so far: reads started and how
 * many of them descended on a key rather than visiting every record, B-tree
 * nodes read, records handed to a callback, keys compared.  The two kinds are
 * counted apart because the difference between them is the whole point -- a
 * read that descends costs the depth of the tree and a read that does not
 * costs the size of the volume.
 */
void	fs_apfs_stats(void);

/*
 * Take a run of free blocks, confirm the disk agrees, and give it back.
 *
 * Stops one step short of a real allocation on purpose, and the step it stops
 * at was found by trying: a container holding a block marked in use that
 * nothing references is not valid, and apfsck rejects it outright.  An
 * allocation in this format is half an operation; the other half is whatever
 * points at the block, and neither half is valid alone.  What this proves is
 * the half that exists -- the search, the bitmap edit, both counters, the
 * sealing, and that all of it survives a round trip through the drive.
 */
void	fs_apfs_alloc_selftest(void);

/*
 * Close the container's current transaction: write a checkpoint.
 *
 * Re-emits the checkpoint's ephemeral objects into the next free slots of the
 * data ring, writes a checkpoint map naming where they landed, and closes the
 * whole thing with a superblock carrying the next transaction id.  The
 * superblock's landing is the commit: before it the container is the previous
 * checkpoint entire, after it the new one entire.  Block zero is then made a
 * copy of that superblock, which is what makes the difference between a
 * container fsck calls clean and one it calls interrupted.
 *
 * What it does NOT do yet is change any object tree, so the checkpoint it
 * writes says what the previous one said, one xid later.  That is the whole
 * scope for now: the mechanism has to be provable on its own before
 * copy-on-write can hang the top of its chain off it.
 *
 * Returns FS_APFS_E_OK, or a negative FS_APFS_E_* with nothing written -- the
 * one exception being a failure to update block zero, which is reported and
 * not propagated, because by then the checkpoint has happened.
 *
 * The caller must hold the volume lock: this moves state the readers use.
 */
int	fs_apfs_checkpoint(void);

/*
 * Write two checkpoints and interrogate the disk after each: block zero moved,
 * the ring's newest superblock is the one just written, the checkpoint it
 * replaced still reads and still says its own xid, and every object the new
 * map names is where it says at the new xid.  Two, because a writer that
 * commits correctly but forgets to advance its own cursors passes the first
 * and overwrites it with the second.
 */
void	fs_apfs_ckpt_selftest(void);

/*
 * Write to a file and prove the bytes MOVED: the run the live checkpoint still
 * names has to read exactly as it did, while the new run carries the write.
 * Restores what it found.  The path is passed in so that the one file the
 * write tests use is named in one place.
 */
void	fs_apfs_data_selftest(const char *path);

/*
 * Make a file longer: `ino` names the inode record carrying its length, `id`
 * the dstream its extents are keyed on.  A file with slack in its last block
 * grows into it and nothing but the length changes; otherwise a run is taken,
 * zeroed, and handed to both trees that name a file's blocks.  Refuses when
 * the leaf it would insert into has no room -- splitting a node is not done
 * here.  Returns FS_APFS_E_OK or a negative FS_APFS_E_*.
 */
int	fs_apfs_grow(uint64_t ino, uint64_t id, uint64_t new_size);

/*
 * And make one shorter, with the same two names for the same two things.  A
 * run that reaches past the new end is shortened; a run entirely past it loses
 * its record in both trees that name a file's blocks; the blocks go to the
 * free queue, because checkpoints still on the platter name them.  Cutting
 * inside a block moves the length and nothing else.  Refuses, out loud, a file
 * whose runs are spread over more leaves than its inode's own.
 */
int	fs_apfs_truncate(uint64_t ino, uint64_t id, uint64_t new_size);

/*
 * How many B-tree nodes this kernel has split since it booted.  Exposed so a
 * test can insist that a node really did run out of room and grow rather than
 * the insert quietly having had space all along.
 */
uint64_t fs_apfs_splits(void);

/*
 * And how many appends lengthened a run that was already there instead of
 * giving the file another one.  Two runs that touch are one run; a test that
 * cannot see the difference cannot tell a filesystem that coalesces from one
 * that is quietly spending a record per block.
 */
uint64_t fs_apfs_merges(void);

/*
 * How many records a truncate has shortened, and how many it has taken out of
 * a tree altogether.  Two counters and not one, because they are two different
 * operations wearing the same name: shortening edits a length in place, and
 * dropping is the first thing this kernel does that makes a B-tree smaller.  A
 * test that could not tell them apart would pass on a truncate that never once
 * reached the harder half.
 */
uint64_t fs_apfs_shortens(void);
uint64_t fs_apfs_drops(void);

/*
 * Put a name into the directory whose object id is `dir`, with an empty file
 * under it, and report the object id it was given.  `now` is the moment to
 * stamp, in nanoseconds since the Unix epoch, passed in for the same reason
 * fs_apfs_touch takes one: this file knows the on-disk format, not the clock.
 *
 * The file is created with a dstream holding no bytes and no blocks, because an
 * inode with no dstream at all is one nothing can ever lengthen.  Returns
 * FS_APFS_E_EXIST if the name is taken, and refuses a name this kernel cannot
 * hash -- anything outside ASCII -- rather than guessing at Apple's folding.
 */
int	fs_apfs_create(uint64_t dir, const char *name, uint64_t now,
	    uint16_t perm, uint64_t *ino_out);

/*
 * And take a name back out, with the file under it.
 *
 * The blocks are given back by cutting the file to nothing first, which is the
 * truncate above doing what it already does; what is left for this is the three
 * records a create made and the two counts it moved.  Refuses a directory
 * (FS_APFS_E_ISDIR) and refuses an inode with more than one link, out loud,
 * because this kernel makes neither and unlinking one of several is a different
 * operation from removing the last name a file has.
 */
int	fs_apfs_unlink(uint64_t dir, const char *name, uint64_t now);

/*
 * The same two calls again for a DIRECTORY, and they are the same two calls:
 * one function each, with a question in it, rather than a pair that agrees
 * today.  What differs on disk is smaller than it looks and none of it is
 * optional -- a directory's inode carries no data stream at all (a checker
 * that finds one says so), its entry is typed a directory (as does one that
 * finds the two disagreeing), its child count starts at zero, and it is
 * counted in apfs_num_directories, which unlike apfs_num_files is checked.
 *
 * fs_apfs_rmdir removes a directory that holds nothing.  "Holds nothing" is
 * asked of the TREE and not of the count in the directory's own record: the
 * count is a claim and the records are the thing.  Refuses a name that is not
 * a directory (FS_APFS_E_NOTDIR) and one that still holds a name
 * (FS_APFS_E_NOTEMPTY); the second is not politeness towards POSIX, since a
 * directory removed out from under its children leaves entries whose parent
 * is gone, and the checker says so.
 */
int	fs_apfs_mkdir(uint64_t dir, const char *name, uint64_t now,
	    uint16_t perm, uint64_t *ino_out);
int	fs_apfs_rmdir(uint64_t dir, const char *name, uint64_t now);

/*
 * Move a name, within one directory or between two, taking what is under it.
 *
 * Nothing is created and nothing destroyed: the same inode ends up with a
 * different name, in a different place, and the tree holds exactly as many
 * records as it did.  What makes it a rung of its own is that the inode RECORD
 * has to follow -- it carries the name and the parent, apfsck checks both
 * against the entry that names it, and a name of a different length makes the
 * record a different length, so it is rebuilt rather than amended.
 *
 * Refuses FS_APFS_E_NOTFOUND for a name that is not there, FS_APFS_E_EXIST for
 * a destination that is taken (replacing what is already there is a rung up),
 * and FS_APFS_E_INVAL for the root, for an inode with more than one link, and
 * for a directory moved into itself -- which would take every name inside it
 * out of the volume.  Renaming a name to itself succeeds and changes nothing;
 * renaming one to another spelling of itself is a real move, so the case a
 * caller asks for is the case the volume keeps.
 */
int	fs_apfs_rename(uint64_t odir, const char *oname, uint64_t ndir,
	    const char *nname, uint64_t now);

/*
 * Take a name away from a file that something still holds open.
 *
 * The entry leaves its directory for the private one, under the name the
 * format wants there; the inode record follows it, as it does for a rename,
 * except that what it is rebuilt with is the ROOT for a parent and ZERO for a
 * link count.  The bytes are not touched.  Afterwards no path reaches the
 * file and every open descriptor still does, which is what Unix promises and
 * what this kernel used to say out loud that it did not keep.
 *
 * Refuses FS_APFS_E_ISDIR for a directory: an open directory whose name is
 * taken away is a real case and a different one, since a directory in here
 * would still have children whose parent is unreachable.
 *
 * *ino_out gets the object id, because that is the only handle on the file
 * afterwards -- there is no name left to ask about.
 */
int	fs_apfs_orphan(uint64_t dir, const char *name, uint64_t now,
	    uint64_t *ino_out);

/*
 * And let one go, when the last descriptor on it closes.
 *
 * The same work as an unlink -- the bytes, the three records, the counters --
 * asked for by object id, since the caller has no name and the one in the
 * private directory is derived rather than remembered.
 */
int	fs_apfs_reap(uint64_t ino, uint64_t now);

/*
 * Everything the private directory is still holding, let go at once.
 *
 * Called after mounting, and the case it exists for is a crash: an orphan is
 * created by a kernel that means to reap it and gets to only if it lives that
 * long.  A volume that comes up with names in here is a volume whose last boot
 * ended between the two, and the format's answer -- the reason the directory
 * exists rather than a log -- is that they can simply be finished now.
 *
 * *n_out gets how many, which is zero on every clean boot and the whole point
 * of the number on the others.
 */
int	fs_apfs_reap_all(uint64_t now, uint32_t *n_out);

/*
 * How many names have been taken from files still open, and how many of those
 * files have since been let go.
 *
 * They are counted apart because they are not the same event and need not
 * balance within one boot: a file orphaned by a kernel that then stopped is
 * reaped by the next one, and the pair of numbers across two boots is how a
 * test says so.
 */
uint64_t fs_apfs_orphans(void);
uint64_t fs_apfs_reaps(void);

/*
 * apfs-orphan: a file kept alive by nothing but a promise.
 *
 * Arranges it, like the rungs above: writes a file with a length, takes its
 * name away, and then asks the questions that only hold if the bytes survived
 * -- the length is what it was, the path no longer resolves, the object id
 * still does.  Then lets it go and checks the volume is back where it started,
 * because an orphan that is never reaped is a leak that apfsck calls valid.
 */
void	fs_apfs_orphan_selftest(uint64_t now);

/*
 * How many files have been made and unmade, and how many record ends have been
 * laid into room a delete gave back.
 *
 * The third is the one worth having.  A node's free span only ever shrinks, so
 * a create and an unlink that did not reuse the holes between them would cost a
 * record's worth of room per cycle and the volume would stop taking names after
 * about fifteen boots -- while still reporting thousands of bytes free.  A test
 * that watches this number watches for that.
 */
uint64_t fs_apfs_makes(void);
uint64_t fs_apfs_kills(void);
uint64_t fs_apfs_holes(void);

/*
 * And the same two for directories, counted apart from files on purpose.  A
 * refusal is the thing worth watching here: a test that asks for a directory
 * it must not get -- one that is not empty, one that is not a directory --
 * proves nothing by being answered with an error, because an error is also
 * what a writer that half-did the work and then failed would answer.  These
 * say whether the work happened.
 */
uint64_t fs_apfs_dirmakes(void);
uint64_t fs_apfs_dirkills(void);

/*
 * Names moved.  Worth its own counter for the reason above and one more: a
 * rename is the only writer here whose success moves no total at all -- the
 * same records, the same key count, the same file and directory counts -- so
 * this is the only number that says it happened.
 */
uint64_t fs_apfs_moves(void);

/*
 * apfs-move: a name moved, within a directory and between two.
 *
 * Arranges what it checks, like the two above.  A file is given a length and
 * then moved to a longer name and a shorter one, because the length lives in
 * an extended field packed after the name and a rename that rebuilt the record
 * instead of carrying it across would leave a valid empty file behind.  A
 * directory is moved with a child in it, which nothing touches.  And the
 * refusals are asked for -- onto a name that is taken, of a name that is not
 * there, of a directory into itself, under its own child, and into something
 * that is not a directory -- because a refusal leaves no trace on the volume
 * for a checker to find afterwards.
 */
void	fs_apfs_move_selftest(uint64_t now);

/*
 * Split a leaf on purpose and prove nothing was lost: the same records, in the
 * same order, reachable through the index afterwards.  Asked for directly
 * because a sequential writer no longer fills a node -- it coalesces instead.
 */
void	fs_apfs_split_selftest(void);

/*
 * Make a node stop starting where its parent says it does, and prove the
 * writer notices.  Arranged rather than waited for, out of two files that take
 * turns: the leaf holding one's inode record is split AT that record so it
 * becomes the key the index files that half under, the file is unlinked, and
 * the other is what keeps that half from emptying -- so it stays on the volume
 * and is the next boot's victim.  Takes the wall clock in nanoseconds, because
 * this file has no clock of its own.
 */
void	fs_apfs_index_selftest(uint64_t now);

/*
 * Make a node lose its last record, and prove it leaves the tree rather than
 * staying in it holding nothing -- which apfsck calls "B-tree: keys are out of
 * order", since the index above still files it under a key it no longer has.
 * A freshly made file has the highest key on the volume, so splitting the leaf
 * that holds its inode record AT that record puts it alone in a node; unlinking
 * it is then the whole of the arrangement.  Takes the wall clock in
 * nanoseconds, for the same reason as the test above.
 */
void	fs_apfs_drop_selftest(uint64_t now);

/*
 * Put a file's inode record and its data stream record in DIFFERENT nodes on
 * purpose -- by splitting the leaf that holds both between them -- and prove
 * that unlinking it takes both.  Checked by asking the tree for the stream
 * record afterwards, because the unlink that left one behind reported success.
 * Takes the wall clock in nanoseconds.
 */
void	fs_apfs_stream_selftest(uint64_t now);

/*
 * Fill a leaf on purpose and prove a create makes its own room.  Names go into
 * a directory one at a time until the writer has had to split, and a couple
 * more afterwards; then every one of them has to still resolve to the inode it
 * was made as, and every node has to still start where its parent says it does.
 * All of them are removed again.  Takes the wall clock in nanoseconds, for the
 * same reason the two tests above do.
 */
void	fs_apfs_room_selftest(uint64_t now);

/*
 * Prove that descending on a key finds what reading every record finds.  Every
 * record on the volume is sought by its own key and has to come back out of
 * the leaf it lives in with the rest of the tree behind it, in order; keys
 * that are not on the volume have to land on the record after them; and the
 * leaf an insert would use has to be the one the old whole-tree answer names.
 * Reads only, so it can run at any point and changes nothing.
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
 * Write a METADATA block back, sealing it first: the Fletcher-64 is computed
 * over the block from offset 8 and stored in the header, so the result cannot
 * be part of its own input.  Exposed because fs/fs_txn.c writes the blocks a
 * transaction collected and sealing is not knowledge worth having twice.
 *
 * Not for file data, which carries no header and needs no seal -- writing
 * data through here would overwrite its first eight bytes with a checksum.
 */
int	fs_apfs_write_block(uint64_t bno, void *buf);

/*
 * Read and write a block that has NO obj_phys header, so there is no checksum
 * to verify on the way in and none to seal on the way out.
 *
 * File data is the obvious such block, but it is not the interesting one: an
 * allocation bitmap is also nothing but bytes, and the eight where a metadata
 * block keeps its Fletcher-64 are the allocation state of the chunk's first
 * sixty-four blocks.  Reading one through the checked path rejects it and
 * writing one through the sealed path destroys it, which is why the
 * distinction is exported rather than kept private to the reader.
 */
int	fs_apfs_read_block_raw(uint64_t bno, void *buf);
int	fs_apfs_write_block_raw(uint64_t bno, const void *buf);

/* ---- writing ------------------------------------------------------- */

/*
 * WHAT THIS WRITER IS, AND WHAT IT DELIBERATELY IS NOT.
 *
 * APFS is a copy-on-write filesystem: Apple's implementation writes a changed
 * metadata block to a NEW location, updates the object map to point at it,
 * and publishes the result by writing a fresh checkpoint superblock with a
 * higher transaction id.  Nothing is overwritten, so an interrupted write
 * leaves the previous checkpoint intact and the volume mounts as it was.
 *
 * This writer does none of that.  It mutates IN PLACE: a changed block is
 * written back where it already lived.  That forfeits exactly one property --
 * crash safety, since a power loss mid-write leaves a block half-updated with
 * no older version to fall back to -- and buys the absence of the two hardest
 * pieces of APFS, the space manager (allocation bitmap chunks plus free-queue
 * B-trees keyed by xid) and the recursive object-map rewrite that copy-on-
 * write forces.  A volume this writer has touched is still a VALID APFS
 * volume at rest: checksums are recomputed, the tree shape is untouched, and
 * an independent implementation reads it back.  It is simply not a volume
 * that was written the way Apple writes one.
 *
 * The consequence is a hard boundary, and the API states it rather than
 * papering over it: nothing here can allocate a block.  A write may only land
 * on blocks the file already owns.  Growing a file, filling a hole, creating
 * a file, and adding a directory entry all need an allocator and all return
 * FS_APFS_E_NOALLOC.  Overwriting bytes that are already there is the whole
 * of what works, and it works completely.
 */

/*
 * Overwrite `len` bytes of the resolved file (`id`, `size`) at file offset
 * `off`, reporting the count written through *out_put.
 *
 * Refuses rather than truncates: a write that would extend the file past
 * `size`, or that lands anywhere the file has no block (a hole, or a range no
 * extent record covers), returns FS_APFS_E_NOALLOC having written nothing it
 * cannot account for.  Coverage is verified by counting bytes actually
 * written and comparing against the request -- an absent extent record is
 * silence, not an error, and silence must not read as success.
 *
 * Partial blocks are read-modify-written, so the bytes around the request
 * survive it.
 */
int	fs_apfs_pwrite(uint64_t id, uint64_t size, uint64_t off,
	    const uint8_t *buf, uint32_t len, uint32_t *out_put);

/*
 * Stamp an inode's modification and change times, in nanoseconds since the
 * Unix epoch, which is how APFS stores them.
 *
 * This is the first thing here to rewrite METADATA, and therefore the first
 * to recompute a Fletcher-64 rather than merely check one: the B-tree leaf
 * holding the record is re-read, patched, re-sealed and written back.  Note
 * the transaction id is left alone -- bumping it would oblige us to publish a
 * new checkpoint, which is the next rung and not this one.
 */
int	fs_apfs_touch(uint64_t oid, uint64_t mtime_ns);

/*
 * fs_apfs_chmod: the permission bits of an inode that already exists, and the
 * change time that any Unix moves with them.  Only the low twelve bits are
 * the caller's to set -- the type nibble belongs to the format, and the
 * checker tests it against the type in the directory entry.
 */
int	fs_apfs_chmod(uint64_t oid, uint16_t perm, uint64_t now_ns);

/*
 * The current length of the file whose inode object id is `ino`, without
 * resolving a path.  One tree walk instead of one per path component, which
 * is what makes refreshing a stale handle affordable enough to do on demand
 * rather than caching a length and hoping.
 */
int	fs_apfs_size(uint64_t ino, uint64_t *size_out);

#endif /* !_SYS_FS_APFS_H_ */
