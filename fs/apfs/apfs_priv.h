/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef	_FS_APFS_PRIV_H_
#define	_FS_APFS_PRIV_H_

/*
 * APFS internals shared by apfs.c and apfs_test.c, and by nothing else: the
 * mounted container's state, the shapes the tree is read through, and the
 * few functions a test needs to arrange a case the public interface cannot
 * ask for (split this node, grow the tree a level, which leaf holds this
 * key).  The writing machinery -- leaf edit, object-map edit, checkpoint
 * builder -- is deliberately not here: a test that assembled its own
 * transaction would be checking its own arithmetic, not the writer's.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apfs.h"

/*
 * Ephemeral objects one checkpoint may name before the mount stops
 * recording them.  A container has the reaper, the space manager and one
 * B-tree per free queue, plus a handful per mounted volume; 32 is far above
 * what a single-volume container needs.
 */
#define	APFS_EPH_MAX		32

/*
 * Mounted container state.  (m) = written once by fs_apfs_init before any
 * reader exists, read-only afterwards.  (c) = set at mount, then moved on by
 * the writers and fs_apfs_checkpoint, under the volume lock (fs_lock in
 * fs/fs.c) that every path reading these holds.
 */
struct apfs_mount {
	uint64_t	ac_block_count;		/* (m) */
	uint64_t	ac_xid;			/* (c) newest checkpoint    */
	uint64_t	ac_omap_oid;		/* (c) container object map */
	uint64_t	ac_fs_oid;		/* (m) volume 0 superblock  */
	uint64_t	ac_xp_desc_base;	/* (m) */
	uint64_t	ac_vol_omap_tree;	/* (c) volume omap B-tree   */
	uint64_t	ac_root_tree_bno;	/* (c) file-system B-tree   */
	uint64_t	ac_ctr_omap_tree;	/* (c) container omap B-tree */
	uint64_t	ac_vol_sb_bno;		/* (c) volume superblock    */
	uint64_t	ac_vol_omap_bno;	/* (c) volume omap object   */
	uint64_t	ac_root_tree_oid;	/* (m) its VIRTUAL oid      */
	uint64_t	ac_extref_bno;		/* (c) extent reference tree */
	uint64_t	ac_fs_alloc_count;	/* (c) blocks this volume owns */
	/*
	 * The volume's next object id, which numbers inodes; the container's
	 * nx_next_oid numbers B-tree nodes, a separate namespace.  apfsck takes
	 * it as a claim that every id at or above it is unused, so a create
	 * must advance it.
	 */
	uint64_t	ac_next_ino;		/* (c) */
	uint64_t	ac_num_files;		/* (c) */
	uint64_t	ac_num_dirs;		/* (c) */
	uint32_t	ac_xp_desc_blocks;	/* (m) */
	uint32_t	ac_xp_desc_index;	/* (c) this checkpoint's first */
	uint32_t	ac_xp_desc_len;		/* (c) ...and how many         */
	uint64_t	ac_spaceman_oid;	/* (m) ephemeral               */
	bool		ac_drec_hashed;		/* (m) hashed dirent keys   */
	bool		ac_dirty;		/* (c) a checkpoint is owed */
	bool		ac_mounted;		/* (m) */

	/*
	 * What writing the next checkpoint needs: where the adopted superblock
	 * came from, the xid it said follows, and the two rings' free slots.
	 * Ephemeral objects are re-emitted into the data ring each time.
	 */
	uint64_t	ac_sb_bno;		/* (c) block the sb came from  */
	uint64_t	ac_next_xid;		/* (c) what it said comes next */
	uint64_t	ac_next_oid;		/* (c) fresh virtual object ids */
	uint32_t	ac_xp_desc_next;	/* (c) first free desc slot    */
	uint64_t	ac_xp_data_base;	/* (m) */
	uint32_t	ac_xp_data_blocks;	/* (m) */
	uint32_t	ac_xp_data_index;	/* (c) this checkpoint's first */
	uint32_t	ac_xp_data_len;		/* (c) ...and how many         */
	uint32_t	ac_xp_data_next;	/* (c) first free data slot    */

	/*
	 * The checkpoint's ephemeral objects, resolved.  A fixed table: the
	 * count follows the container's shape, not its size -- four here
	 * (reaper, space manager, two free-queue trees).  ac_eph_over counts
	 * what did not fit, and fs_apfs_checkpoint refuses while it is nonzero.
	 */
	struct {
		uint64_t	e_oid;
		uint64_t	e_paddr;	/* (c) moves every checkpoint */
		uint64_t	e_fs_oid;
		uint32_t	e_type;
		uint32_t	e_subtype;
		uint32_t	e_size;		/* bytes, as the map states  */
	}		ac_eph[APFS_EPH_MAX];		/* (m) */
	uint32_t	ac_eph_count;			/* (m) */
	uint32_t	ac_eph_over;			/* (m) dropped for space */

	/* What the space manager says, once it has been found and read. */
	bool		ac_sm_valid;		/* (m) */
	uint64_t	ac_sm_paddr;		/* (c) moves every checkpoint */
	uint64_t	ac_sm_free;		/* (c) blocks free on device 0 */
	uint64_t	ac_sm_chunks;		/* (m) */
	uint32_t	ac_sm_blocks_per_chunk;	/* (m) */
	uint32_t	ac_sm_cib_count;	/* (m) */
	uint32_t	ac_sm_cab_count;	/* (m) */
	uint32_t	ac_sm_addr_offset;	/* (m) into the spaceman block */
	uint64_t	ac_sm_ip_base;		/* (m) internal pool           */
	uint64_t	ac_sm_ip_blocks;	/* (m) */
	uint64_t	ac_sm_fq_count[APFS_SFQ_COUNT];	  /* (m) */
	uint64_t	ac_sm_fq_oldest[APFS_SFQ_COUNT];  /* (m) */

	/* What the chunk walk found, and whether it agreed with the above. */
	bool		ac_bm_valid;		/* (m) */
	uint64_t	ac_bm_chunks;		/* (m) chunks described     */
	uint64_t	ac_bm_blocks;		/* (m) blocks they cover    */
	uint64_t	ac_bm_free_said;	/* (m) sum of ci_free_count */
	uint64_t	ac_bm_free_counted;	/* (m) clear bits counted   */
	uint64_t	ac_bm_scanned;		/* (m) chunks bit-counted   */
	uint64_t	ac_bm_wholly_free;	/* (m) chunks with no bitmap */
	uint64_t	ac_bm_disagreed;	/* (m) chunks that did not  */
	/*
	 * Each resident chunk's share of those two totals is kept in its
	 * alloc_chunk, so the share can be swapped for the live figure and the
	 * three numbers compared at one instant.
	 */

	/*
	 * The internal pool: the blocks that describe allocation, which
	 * cannot live in the space they account for.  ac_ipbm_slot is which
	 * ring slot currently holds the pool's own bitmap.
	 */
	bool		ac_ip_valid;		/* (m) */
	uint64_t	ac_ip_base;		/* (m) first pool block  */
	uint64_t	ac_ip_blocks;		/* (m) how many          */
	uint64_t	ac_ipbm_base;		/* (m) ring of bitmaps   */
	uint32_t	ac_ipbm_slots;		/* (m) how long the ring */
	uint32_t	ac_ipbm_slot;		/* (c) the live one      */

	/*
	 * The chunk metadata is allocated from, chosen during the walk: one
	 * with a real bitmap (not the wholly-free shortcut) and room to spare.
	 * These are what the walk found; where its bitmap lives now is the
	 * resident chunk's (g_home) business.
	 */
	bool		ac_alloc_have;		/* (m) */
	uint64_t	ac_alloc_cib;		/* (c) its chunk-info block  */
	uint32_t	ac_alloc_slot;		/* (m) which chunk within it */
	uint64_t	ac_alloc_bitmap;	/* (m) where it was at mount */
	uint64_t	ac_alloc_base;		/* (m) first block of chunk  */
	uint32_t	ac_alloc_blocks;	/* (m) */
};

extern struct apfs_mount	g_apfs;

/*
 * What reading the tree costs.  apfs.c raises them and fs_apfs_stats prints
 * them; the seek test reads them to report its own cost apart.
 */
extern uint64_t	g_n_walks;	/* reads that visited every record */
extern uint64_t	g_n_seeks;	/* reads that descended on a key   */
extern uint64_t	g_n_nodes;	/* B-tree nodes read during them   */
extern uint64_t	g_n_recs;	/* records handed to a callback    */
extern uint64_t	g_n_cmps;	/* keys compared while descending  */

/* The free-queue B-trees, resident for the life of the mount. */
extern uint8_t	*g_fq[APFS_SFQ_COUNT];

/*
 * What has happened to the tree's shape, which the tests that arrange a
 * shape check against.  gone_n counts nodes that left a tree; records a
 * truncate removed are drop_n, private to apfs.c.
 */
extern uint64_t	split_n;	/* nodes split in two                 */
extern uint64_t	deep_n;		/* levels the tree has gained         */
extern uint64_t	reidx_n;	/* index keys corrected after an edit */
extern uint64_t	gone_n;		/* emptied nodes taken out of a tree  */

/*
 * How many checkpoints a released block stays unavailable for.  Policy, not
 * format: how long an older checkpoint stays readable.
 *
 * The price is free-queue room: one node holds every unreleased entry of
 * the last KEEP+1 transactions, and releases lag insertion by KEEP
 * checkpoints.  With transactions of several edits, 4 let the node fill
 * faster than the belt in fs_apfs_ckpt_due could age slices out ("free
 * queue 1 is full (144 keys)" once per busy boot); at 2 the belt always
 * wins.  A crashed mount falls back only to the newest intact checkpoint
 * (tools/hosttorn.c), which needs just the open transaction's frees held;
 * the one more checkpoint kept behind it is a courtesy.
 */
#define	APFS_FQ_KEEP		2

/*
 * One chunk of the allocation bitmap, in memory.  Resident chunks are the
 * only place a bit is ever changed; apfs.c says why there is more than one.
 */
struct alloc_chunk {
	uint8_t		*ch_bm;		/* its allocation bitmap           */
	uint64_t	 ch_base;	/* (m) first block it covers       */
	uint64_t	 ch_bitmap;	/* (c) where that bitmap lives now */
	uint32_t	 ch_blocks;	/* (m) how many blocks it covers   */
	uint32_t	 ch_slot;	/* (m) its index in the chunk-info */
	bool		 ch_dirty;	/* (c) */
	/*
	 * What the chunk said when admitted, so the running totals can swap
	 * this share for the live figure.  Taken at admission, when it is
	 * certainly untouched: only a resident chunk's bits ever change.
	 */
	uint32_t	 ch_free_admit;
	uint32_t	 ch_bits_admit;
};

/* The chunk this volume's metadata is allocated from. */
extern struct alloc_chunk	*g_home;

/*
 * Where a B-tree node keeps its three regions.  The value base is the easy
 * one to get wrong: value offsets run backwards from the end of the node,
 * and a root node reserves its last 40 bytes for the btree_info.
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


/*
 * How deep a descent follows child pointers before calling the tree corrupt.
 * Bounds every downward walk and the path a split records.
 */
#define	APFS_TREE_MAX_DEPTH	8

/*
 * Callback fired for every leaf record, in tree order.  Returning false
 * stops the walk.
 *
 * `bno` is the leaf block holding the record, for the writer.  A block
 * number, not a pointer: the walk frees its node buffer on the way out, so
 * a writer re-reads the block and patches its own copy.
 */
typedef bool (*apfs_rec_fn)(uint64_t oid, uint32_t type, const uint8_t *key,
    uint32_t klen, const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg);


/* Where a key belongs: which leaf, rather than which record. */
struct leaf_find {
	const uint8_t	*lf_key;
	uint32_t	 lf_klen;
	uint64_t	 lf_bno;
	uint64_t	 lf_first;
	bool		 lf_any;
};

/*
 * A node's ancestry, block and oid at every level: a copy changes where a
 * node is without changing the name everything above it uses.
 */
struct tree_path {
	uint64_t	tp_bno[APFS_TREE_MAX_DEPTH];
	uint64_t	tp_oid[APFS_TREE_MAX_DEPTH];
	uint32_t	tp_n;			/* the root is [0] */
};

/* ---- reading the container ------------------------------------------------ */

/*
 * The three things a reader descends from -- transaction, volume object map,
 * root -- taken from here rather than g_apfs because a view (apfs.h) points
 * all three at an older checkpoint.  view_floor is the oldest checkpoint a
 * view may still be opened on.
 */
uint64_t	view_xid(void);
uint64_t	view_omap(void);
uint64_t	view_root(void);
uint64_t	view_floor(void);
bool		block_is_nxsb(const void *buf);
int		read_block_raw(uint64_t bno, void *buf);
uint32_t	crc32c(uint32_t crc, const uint8_t *p, uint32_t n);
size_t		str_len(const char *s);

/* ---- the file-system tree -------------------------------------------------- */

void		btree_layout(const void *node, struct btree_layout *out);
void		btree_entry_loc(const struct btree_layout *bl, uint32_t i,
		    uint32_t *koff, uint32_t *klen, uint32_t *voff,
		    uint32_t *vlen);
int		jkey_cmp(const uint8_t *a, uint32_t alen, const uint8_t *b,
		    uint32_t blen);
bool		btree_scan(uint64_t bno, const uint8_t *key, uint32_t klen,
		    apfs_rec_fn fn, void *arg, int depth, bool *stopped);
bool		btree_walk(uint64_t bno, apfs_rec_fn fn, void *arg, int depth,
		    bool *stopped);
bool		leaf_find(uint64_t oid, uint32_t type, const uint8_t *key,
		    uint32_t klen, const uint8_t *val, uint32_t vlen,
		    uint64_t bno, void *arg);
int		leaf_home(const uint8_t *key, uint32_t klen, uint64_t *bno_out);
int		inode_where(uint64_t oid, uint64_t *bno_out);
int		extent_at(uint64_t id, uint64_t off, uint64_t *phys_out);
bool		path_to(uint64_t want, struct tree_path *tp);
bool		tree_nodes_of(const uint8_t *root, uint64_t *out);

/* ---- changing its shape, which only a test asks for directly --------------- */

int		node_split_at(uint64_t bno, uint32_t at, uint64_t xid,
		    uint8_t *scratch);
int		tree_grow(uint64_t xid, uint8_t *scratch);

/* ---- space ----------------------------------------------------------------- */

int		alloc_blocks(uint32_t count, uint64_t near, uint64_t *first_out);
int		free_blocks(uint64_t first, uint32_t count);
uint32_t	bitmap_free_count(const uint8_t *bm, uint32_t blocks);
struct alloc_chunk	*chunk_for(uint64_t bno);
struct alloc_chunk	*chunk_resident(uint64_t bno);

#endif	/* _FS_APFS_PRIV_H_ */
