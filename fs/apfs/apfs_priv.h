/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef	_FS_APFS_PRIV_H_
#define	_FS_APFS_PRIV_H_

/*
 * APFS INTERNALS, AND WHO IS ALLOWED TO SEE THEM
 *
 * apfs.h is what the rest of the kernel may call.  This is what apfs.c and
 * apfs_test.c share and nothing else may touch: the mounted container's own
 * state, the shapes the tree is read through, and the handful of functions a
 * test needs to arrange a case the ordinary interface cannot ask for -- split
 * this node, grow the tree a level, tell me which leaf that key lives in.
 *
 * It exists because the self-tests moved out of apfs.c, and it is the honest
 * price of that move: a test that proves a node splits correctly has to be
 * able to split one.  What it deliberately does NOT export is the writing
 * machinery -- the leaf edit, the object-map edit, the checkpoint builder --
 * because a test that assembled its own transaction would be checking its own
 * arithmetic rather than the writer's.
 *
 * Everything here was static until the split.  Nothing outside the two files
 * that include this header may use any of it.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apfs.h"

/*
 * How many ephemeral objects one checkpoint may name before this reader stops
 * recording them.  A container holds the reaper, the space manager and one
 * B-tree per free queue, plus a handful per mounted volume; 32 is far above
 * anything a single-volume container produces and is bounded storage in a
 * struct that lives for the life of the mount.
 */
#define	APFS_EPH_MAX		32

/*
 * Mounted container state.  (m) = written once by fs_apfs_init before any
 * reader exists, read-only afterwards.  (c) = the same, except that
 * fs_apfs_checkpoint moves it on to the checkpoint it just wrote; it does so
 * under the volume lock in fs.c, which every path that reads these holds.
 */
struct apfs_mount {
	uint64_t	ac_block_count;		/* (m) */
	uint64_t	ac_xid;			/* (c) newest checkpoint    */
	uint64_t	ac_omap_oid;		/* (m) container object map */
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
	 * The VOLUME's own next object id, which numbers inodes -- a different
	 * namespace from the container's nx_next_oid, which numbers B-tree
	 * nodes and is already a thousand ahead of it.  Advancing this is not
	 * bookkeeping: the checker treats it as an assertion that everything at
	 * or above it is unused, and a created inode whose number the volume
	 * still calls free is the FIRST thing it complains about, ahead of the
	 * file itself.
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
	 * What writing the NEXT checkpoint needs, and nothing reading one
	 * ever asked for: where the superblock we adopted came from, which
	 * xid it said would follow it, and the two rings' free slots.  The
	 * data ring is where ephemeral objects are re-emitted each time; the
	 * reader never had to know it existed, because the checkpoint map
	 * gave it their addresses directly.
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
	 * The checkpoint's ephemeral objects, resolved.  A fixed table because
	 * the count is a property of the container's shape rather than of its
	 * size: four here (reaper, space manager, two free-queue trees), and it
	 * grows only with the number of mounted volumes.  ac_eph_over records
	 * what did not fit, so a container that outgrows this says so instead
	 * of quietly answering half the questions.
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
	uint64_t	ac_sm_free;		/* (m) blocks free on device 0 */
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
	 * What each chunk this kernel holds contributed to those two totals is
	 * recorded per chunk, in alloc_chunk: subtract it, add what the bitmap
	 * in memory says now, and the comparison is between three numbers from
	 * the same instant again.
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
	 * The chunk metadata is taken from, chosen during the walk: one that
	 * has a real bitmap (so the edit exercises the bitmap path rather than
	 * the wholly-free shortcut) and room to spare.  Where its bitmap lives
	 * NOW is the resident chunk's business, below; these are what the walk
	 * found, which is what the pool probe checks and what the admission
	 * starts from.
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
 * What reading the tree costs, counted rather than argued about.  apfs.c
 * raises them and fs_apfs_stats prints them; the seek test reads them to say
 * what checking itself cost, so that the totals a boot reports stay about the
 * filesystem rather than about its proof.
 */
extern uint64_t	g_n_walks;	/* reads that visited every record */
extern uint64_t	g_n_seeks;	/* reads that descended on a key   */
extern uint64_t	g_n_nodes;	/* B-tree nodes read during them   */
extern uint64_t	g_n_recs;	/* records handed to a callback    */
extern uint64_t	g_n_cmps;	/* keys compared while descending  */

/* The free-queue B-trees, resident for the life of the mount. */
extern uint8_t	*g_fq[APFS_SFQ_COUNT];

/*
 * What has happened to the SHAPE of the tree, which is what the three tests
 * that arrange a shape check themselves against.  gone_n is the one to be
 * careful with: it counts NODES that left the tree, and there is a separate
 * drop_n counting RECORDS a truncate removed -- they were the same variable
 * for one rung, because the second was added without noticing the first, and
 * the tree-shape line reported their sum as nodes.
 */
extern uint64_t	split_n;	/* nodes split in two                 */
extern uint64_t	deep_n;		/* levels the tree has gained         */
extern uint64_t	reidx_n;	/* index keys corrected after an edit */
extern uint64_t	gone_n;		/* emptied nodes taken out of a tree  */

/*
 * How many checkpoints a released block stays unavailable for.  Policy, not
 * format: it is how long this kernel promises an older checkpoint remains
 * readable, and the free queue holds blocks for exactly that long.
 */
#define	APFS_FQ_KEEP		4

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
	 * What the chunk said the moment it was brought in, so the running
	 * totals can have this chunk's share subtracted and the live figure put
	 * back.  Taken at admission rather than at mount because that is when
	 * it is certainly still untouched: nothing can change a chunk that is
	 * not resident, every bit going through the copy above.
	 */
	uint32_t	 ch_free_admit;
	uint32_t	 ch_bits_admit;
};

/* The chunk this volume's metadata is allocated from. */
extern struct alloc_chunk	*g_home;

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


/*
 * How deep a descent will follow child pointers before it decides the tree is
 * lying to it.  Every loop here that walks down is bounded by it, and so is
 * the path a split records on its way back up.
 */
#define	APFS_TREE_MAX_DEPTH	8

/*
 * Callback fired for every leaf record, in tree order.  Returning false
 * stops the walk -- a lookup that has found its answer should not keep
 * reading blocks.
 *
 * `bno` is the leaf block the record was found in.  Readers ignore it; it is
 * there for the writer, because a walker that can say what a record contains
 * but never where it lives cannot support changing one.  Note that it is the
 * block NUMBER and not a pointer into the node: the walk frees its buffer on
 * the way out, so a mutation re-reads the block and patches its own copy
 * rather than scribbling on one that is about to be dropped.
 */
typedef bool (*apfs_rec_fn)(uint64_t oid, uint32_t type, const uint8_t *key,
    uint32_t klen, const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg);


/* Where a key belongs, when the question is which leaf rather than which record. */
struct leaf_find {
	const uint8_t	*lf_key;
	uint32_t	 lf_klen;
	uint64_t	 lf_bno;
	uint64_t	 lf_first;
	bool		 lf_any;
};

/*
 * A node's ancestry, block and oid at every level: in a copy-on-write tree
 * those are different questions, since a copy changes where a node is without
 * changing the name everything above it uses.
 */
struct tree_path {
	uint64_t	tp_bno[APFS_TREE_MAX_DEPTH];
	uint64_t	tp_oid[APFS_TREE_MAX_DEPTH];
	uint32_t	tp_n;			/* the root is [0] */
};

/* ---- reading the container ------------------------------------------------ */

uint64_t	view_xid(void);
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
