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
#include "fs_txn.h"
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
static struct {
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
} g_apfs;

/*
 * What the whole-tree walk costs.  Counted rather than argued about: the
 * reader visits every record for every question, and whether that is worth
 * replacing with a keyed descent is a question about these three numbers,
 * not about how the walk reads on the page.
 */
static uint64_t	g_n_walks;	/* whole-tree walks started      */
static uint64_t	g_n_nodes;	/* B-tree nodes read during them */
static uint64_t	g_n_recs;	/* leaf records handed to a callback */

/*
 * The ephemeral layer, in memory.  These two are the objects whose home is
 * RAM and whose disk copies are per-checkpoint: the space manager, and the
 * bitmap of the internal pool that holds the allocation metadata.  Both are
 * read at mount, changed here, and written by fs_apfs_checkpoint.
 */
static uint8_t	*g_sm;		/* the space manager        */
static uint8_t	*g_fq[APFS_SFQ_COUNT];	/* its free-queue B-trees   */
static uint8_t	*g_ipbm;	/* the internal pool bitmap */


/*
 * And the device's own allocation metadata, on the same terms: the chunk
 * bitmaps this kernel is holding, their chunk-info block, and the blocks
 * released by the checkpoint being built.  Written by alloc_flush when a
 * checkpoint is closed, and only then.
 *
 * THE BITMAPS ARE PLURAL, and that is this rung.  A chunk bitmap is one block
 * covering thirty-two thousand blocks of container, and until now the kernel
 * held exactly one -- the chunk the volume's metadata lives in -- which was
 * enough while everything it allocated and everything it released was
 * metadata.  A file's bytes are not.  In this container the only real file has
 * its content at block 5970, in chunk 0, while the metadata being copied
 * around it is at 98304, in chunk 3; moving those bytes therefore frees in one
 * chunk and allocates in another within a single transaction, and a kernel
 * holding one bitmap cannot do it.  free_blocks said exactly that, by name.
 *
 * So bitmaps are admitted on demand and held until the checkpoint writes them.
 * The set is small and fixed: an admission that does not fit is refused out
 * loud, because the alternative -- evicting a dirty bitmap mid-transaction --
 * means copying it into the pool and copying it again on the next bit set,
 * which is the cost the once-per-checkpoint flush exists to avoid.
 */
#define	APFS_CHUNKS_RESIDENT	4

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

static struct alloc_chunk  g_chunk[APFS_CHUNKS_RESIDENT];
static uint32_t		   g_chunk_n;	/* how many are resident     */
static struct alloc_chunk *g_home;	/* where metadata comes from */
static uint8_t	*g_cib;		/* the chunk-info block      */
static uint64_t	 alloc_n_taken;	/* device blocks allocated */
static uint64_t	 alloc_n_given;	/* ...and released         */
static uint64_t	 chunk_n_admit;	/* bitmaps brought into memory */

/*
 * The writers reach for these before the file gets to them.  Space management
 * is one subject and stays in one place, below, rather than being hoisted up
 * here a function at a time to satisfy the order a reader happens to want.
 *
 * alloc_blocks takes a hint: the block the caller would like to be near.  A
 * copy that lands in the chunk it came from keeps the release reachable, and
 * for file data it is also the difference between a file's bytes staying
 * together and being scattered across the container a write at a time.  Zero
 * means "anywhere", which for metadata means the chunk it already lives in.
 */
static int	alloc_blocks(uint32_t count, uint64_t near, uint64_t *first_out);
static int	free_blocks(uint64_t first, uint32_t count);
static int	alloc_flush(uint64_t xid);
static struct alloc_chunk *chunk_for(uint64_t bno);

/*
 * EVERYTHING AN OBJECT MAP CAN BE TOLD, IN ONE COPY OF ITS NODE
 *
 * Three kinds of change, and they travel together because they are one event.
 * The map node is copied once per transaction -- that is the rule the whole
 * spine is built on -- so a caller that made three calls would copy it three
 * times, and the second copy would be replacing an entry in a node the first
 * had already released.  Correct, and six spine objects more expensive each
 * time.
 *
 * Which of the three a caller fills in says what it did:
 *
 *	oe_oids/oe_paddrs	an object MOVED.  Every writer here does this,
 *				because every write is a copy.
 *	oe_new/oe_new_paddrs	an object was MADE, and writing its block did
 *				not make it reachable -- this entry does.  A
 *				split makes one; a root that splits makes two,
 *				since it must keep its own oid.
 *	oe_gone			an object is GONE: a node that lost its last
 *				record and left the tree.  The map is the only
 *				place that still names it, and a map that goes
 *				on naming it is answered with "Omap record:
 *				oid-xid combination is never used".
 */
struct omap_edit {
	const uint64_t	*oe_oids;
	const uint64_t	*oe_paddrs;
	uint32_t	 oe_n;
	const uint64_t	*oe_new;
	const uint64_t	*oe_new_paddrs;
	uint32_t	 oe_nnew;
	const uint64_t	*oe_gone;
	uint32_t	 oe_ngone;
};

static int	spine_update(uint64_t oid, uint64_t paddr, uint64_t xid,
		    void *buf);
static int	spine_update_n(const struct omap_edit *oe, uint64_t xid,
		    void *buf);
static int	cow_physical(uint64_t old_bno, uint64_t xid, void *buf,
		    uint64_t *new_bno);
static int	node_cow(uint8_t *node, uint64_t old_bno, uint64_t xid,
		    uint64_t *new_bno);
static uint32_t	node_place(const uint8_t *node, const uint8_t *key,
		    uint32_t klen);
static int	extref_move(uint64_t old_start, uint64_t new_start,
		    uint64_t blocks, uint64_t xid, void *buf);
static int	fq_insert(uint32_t q, uint64_t xid, uint64_t paddr,
		    uint64_t count);
static void	fq_release(uint32_t q, uint64_t upto_xid);

/*
 * The transaction id a READ should ask about.
 *
 * An object map holds an entry per version, keyed by the transaction that
 * made it, and a lookup takes the newest one no later than the xid it is
 * given.  So once this kernel has copied something, the answer it wants is
 * the one it has just written -- keyed by a transaction that has not
 * committed yet -- and asking with the committed xid finds the version
 * before the copy, or nothing at all.
 *
 * That is not a hypothetical: the first boot with a copied inode read the
 * file back as an I/O error, because the leaf it had just moved was invisible
 * to a lookup that still believed in the committed checkpoint.
 */
static uint64_t
view_xid(void)
{

	return (g_apfs.ac_xid + (g_apfs.ac_dirty ? 1 : 0));
}
static uint64_t	 ip_n_alloc;	/* pool blocks taken    */
static uint64_t	 ip_n_free;	/* pool blocks returned */
static uint64_t	 cow_n_meta;	/* allocation metadata blocks moved */
static uint64_t	 cow_n_spine;	/* spine objects copied            */
static uint64_t	 cow_n_data;	/* file blocks moved by a write    */
static uint64_t	 split_n;	/* nodes split in two              */
static uint64_t	 merge_n;	/* appends that lengthened a run   */
static uint64_t	 short_n;	/* records shortened by a truncate */
static uint64_t	 drop_n;	/* ...and records taken out of it  */
static uint64_t	 make_n;	/* files created                   */
static uint64_t	 kill_n;	/* ...and names taken back out     */
static uint64_t	 hole_n;	/* record ends reusing a deletion  */
static uint64_t	 deep_n;	/* levels the tree has gained      */
static uint64_t	 reidx_n;	/* index keys corrected after an edit */
static uint64_t	 drop_n;	/* emptied nodes taken out of a tree */

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

/* Nothing to say to the object map yet; the caller fills in what it did. */
static void
omap_edit_init(struct omap_edit *oe)
{

	mem_zero((uint8_t *)oe, sizeof(*oe));
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

/*
 * Write one APFS block back with no checksum work.
 *
 * This is for FILE DATA, and the absence of a checksum is the format's doing,
 * not a shortcut: only metadata blocks carry an obj_phys header, and a data
 * block is 4096 bytes of file with nowhere to record a sum of them.  It is
 * also why overwriting file bytes is the cheapest thing this writer does --
 * there is nothing to reseal and nothing else that has to agree.
 */
static int
write_block_raw(uint64_t bno, const void *buf)
{

	if (bio_write(0, bno * APFS_SECTORS_PER_BLOCK, APFS_SECTORS_PER_BLOCK,
	    buf) != 0)
		return (FS_APFS_E_IO);
	return (FS_APFS_E_OK);
}

/*
 * Write one METADATA block back, sealing it first.
 *
 * The Fletcher-64 runs forward here for the first time in this filesystem --
 * every other caller compares it.  It covers the block from offset 8 to the
 * end, so the result must be stored after it is computed and cannot be part
 * of its own input; getting that backwards produces a block that fails its
 * own checksum on the very next read, which is at least a loud failure.
 */
int
fs_apfs_write_block(uint64_t bno, void *buf)
{
	struct apfs_obj_phys	*o;

	o = (struct apfs_obj_phys *)buf;
	o->o_cksum = fs_apfs_fletcher64((const uint8_t *)buf + 8,
	    APFS_BLOCK_SIZE - 8);
	return (write_block_raw(bno, buf));
}

int
fs_apfs_read_block_raw(uint64_t bno, void *buf)
{

	return (read_block_raw(bno, buf));
}

int
fs_apfs_write_block_raw(uint64_t bno, const void *buf)
{

	return (write_block_raw(bno, buf));
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
		/*
		 * Where this checkpoint's own descriptor blocks are, and the
		 * one ephemeral oid worth chasing.  Taken from the superblock
		 * that WON, not from the anchor at block 0: the anchor is a
		 * copy of some earlier checkpoint and its indices point at that
		 * one's blocks.
		 */
		g_apfs.ac_xp_desc_index = nx->nx_xp_desc_index;
		g_apfs.ac_xp_desc_len   = nx->nx_xp_desc_len;
		g_apfs.ac_spaceman_oid  = nx->nx_spaceman_oid;

		/*
		 * And what only a writer needs.  ac_sb_bno matters most: a
		 * checkpoint is built by copying the superblock that closed
		 * the last one, and copying it from the anchor instead would
		 * carry some earlier checkpoint's ring indices forward.
		 */
		g_apfs.ac_sb_bno         = base + i;
		g_apfs.ac_next_xid       = nx->nx_next_xid;
		g_apfs.ac_next_oid       = nx->nx_next_oid;
		g_apfs.ac_xp_desc_next   = nx->nx_xp_desc_next;
		g_apfs.ac_xp_data_base   = nx->nx_xp_data_base;
		g_apfs.ac_xp_data_blocks = nx->nx_xp_data_blocks;
		g_apfs.ac_xp_data_index  = nx->nx_xp_data_index;
		g_apfs.ac_xp_data_len    = nx->nx_xp_data_len;
		g_apfs.ac_xp_data_next   = nx->nx_xp_data_next;
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
 * Read the adopted checkpoint's own descriptor blocks and record every
 * ephemeral object they place.
 *
 * The ring holds one checkpoint after another; nx_xp_desc_index and
 * nx_xp_desc_len name the run belonging to THIS one, and the run wraps.  Its
 * last block is the superblock that closes the checkpoint -- which is how a
 * reader knows the checkpoint was completed -- and the blocks before it are
 * checkpoint maps.  So a slot that is not a map is not an error; it is either
 * that superblock or a slot from a neighbouring checkpoint, and both are
 * simply skipped.
 *
 * Nothing is resolved here beyond recording oid -> block.  Reading the objects
 * themselves is a separate question, and every one of them is optional.
 */
static int
read_checkpoint_maps(void *scratch)
{
	const struct apfs_checkpoint_map_phys	*cpm;
	const struct apfs_checkpoint_mapping	*m;
	uint32_t				 slot;
	uint32_t				 count;
	uint32_t				 maps;
	uint32_t				 i;
	uint32_t				 k;
	uint32_t				 n;

	g_apfs.ac_eph_count = 0;
	g_apfs.ac_eph_over  = 0;
	maps = 0;

	if (g_apfs.ac_xp_desc_len == 0 ||
	    g_apfs.ac_xp_desc_len > g_apfs.ac_xp_desc_blocks)
		return (FS_APFS_E_INVAL);

	for (k = 0; k < g_apfs.ac_xp_desc_len; k++) {
		slot = (g_apfs.ac_xp_desc_index + k) % g_apfs.ac_xp_desc_blocks;
		/*
		 * Checksum-checked: a torn checkpoint map would otherwise send
		 * every later question to a block number out of nowhere.
		 */
		if (fs_apfs_read_block(g_apfs.ac_xp_desc_base + slot,
		    scratch) != FS_APFS_E_OK)
			continue;
		cpm = (const struct apfs_checkpoint_map_phys *)scratch;
		if ((cpm->cpm_o.o_type & APFS_OBJ_TYPE_MASK) !=
		    APFS_OBJ_CHECKPOINT_MAP)
			continue;
		count = cpm->cpm_count;
		if (count > APFS_CPM_MAX_PER_BLOCK)
			count = APFS_CPM_MAX_PER_BLOCK;
		maps++;

		for (i = 0; i < count; i++) {
			m = &cpm->cpm_map[i];
			if (g_apfs.ac_eph_count >= APFS_EPH_MAX) {
				g_apfs.ac_eph_over++;
				continue;
			}
			n = g_apfs.ac_eph_count++;
			g_apfs.ac_eph[n].e_oid     = m->cpm_oid;
			g_apfs.ac_eph[n].e_paddr   = m->cpm_paddr;
			g_apfs.ac_eph[n].e_fs_oid  = m->cpm_fs_oid;
			g_apfs.ac_eph[n].e_type    = m->cpm_type;
			g_apfs.ac_eph[n].e_subtype = m->cpm_subtype;
			g_apfs.ac_eph[n].e_size    = m->cpm_size;
		}
	}

	kprintf("apfs: checkpoint xid %llu -- %u map block(s), %u ephemeral "
	    "object(s)%s\n", (unsigned long long)g_apfs.ac_xid,
	    (unsigned)maps, (unsigned)g_apfs.ac_eph_count,
	    (g_apfs.ac_eph_over != 0) ? " (TABLE FULL, some dropped)" : "");
	return (maps != 0 ? FS_APFS_E_OK : FS_APFS_E_INVAL);
}

/* Where a checkpoint put an ephemeral object, or 0 if it named none. */
static uint64_t
resolve_ephemeral(uint64_t oid)
{
	uint32_t	i;

	for (i = 0; i < g_apfs.ac_eph_count; i++) {
		if (g_apfs.ac_eph[i].e_oid == oid)
			return (g_apfs.ac_eph[i].e_paddr);
	}
	return (0);
}

/*
 * Did the checkpoint map call `oid` a free-queue B-tree?  Used to check the
 * space manager against a list built from an entirely different block.
 */
static bool
ephemeral_is_free_queue(uint64_t oid)
{
	uint32_t	i;

	for (i = 0; i < g_apfs.ac_eph_count; i++) {
		if (g_apfs.ac_eph[i].e_oid != oid)
			continue;
		return ((g_apfs.ac_eph[i].e_type & APFS_OBJ_TYPE_MASK) ==
		    APFS_OBJ_BTREE_ROOT &&
		    g_apfs.ac_eph[i].e_subtype == APFS_OBJ_SPACEMAN_FREE_QUEUE);
	}
	return (false);
}

/*
 * Find and read the space manager.  Read-only, and deliberately so: what this
 * establishes is that the kernel can SEE the container's accounting, which is
 * the thing every later question about allocation has to start from.
 *
 * The free-queue lines are the ones worth reading.  Blocks a transaction gives
 * up do not return to the bitmap when it commits -- they go into one of these
 * trees keyed by that transaction's xid, and come back only once no reader can
 * still be looking at the old state.  A count above zero here means the
 * container is holding space that is neither in use nor available, which is
 * exactly the bookkeeping an allocator would have to join.
 */
static int
read_spaceman(void *scratch)
{
	const struct apfs_spaceman	*sm;
	uint64_t			 paddr;
	uint32_t			 i;

	g_apfs.ac_sm_valid = false;
	if (g_apfs.ac_spaceman_oid == 0)
		return (FS_APFS_E_INVAL);

	paddr = resolve_ephemeral(g_apfs.ac_spaceman_oid);
	if (paddr == 0) {
		kprintf("apfs: spaceman oid %llu is in no checkpoint map\n",
		    (unsigned long long)g_apfs.ac_spaceman_oid);
		return (FS_APFS_E_INVAL);
	}
	if (fs_apfs_read_block(paddr, scratch) != FS_APFS_E_OK)
		return (FS_APFS_E_IO);

	sm = (const struct apfs_spaceman *)scratch;
	if ((sm->sm_o.o_type & APFS_OBJ_TYPE_MASK) != APFS_OBJ_SPACEMAN) {
		kprintf("apfs: block %llu is not a spaceman (type 0x%x)\n",
		    (unsigned long long)paddr, (unsigned)sm->sm_o.o_type);
		return (FS_APFS_E_INVAL);
	}
	/*
	 * The container agreed with itself about its block size once already,
	 * at block 0.  If the space manager disagrees, one of the two is being
	 * read at the wrong offset and nothing below can be trusted.
	 */
	if (sm->sm_block_size != APFS_BLOCK_SIZE) {
		kprintf("apfs: spaceman block size %u != %u -- refusing\n",
		    (unsigned)sm->sm_block_size, APFS_BLOCK_SIZE);
		return (FS_APFS_E_INVAL);
	}

	g_apfs.ac_sm_paddr           = paddr;
	g_apfs.ac_sm_free            = sm->sm_dev[APFS_SD_MAIN].sm_free_count;
	g_apfs.ac_sm_chunks          = sm->sm_dev[APFS_SD_MAIN].sm_chunk_count;
	g_apfs.ac_sm_cib_count       = sm->sm_dev[APFS_SD_MAIN].sm_cib_count;
	g_apfs.ac_sm_cab_count       = sm->sm_dev[APFS_SD_MAIN].sm_cab_count;
	g_apfs.ac_sm_addr_offset     = sm->sm_dev[APFS_SD_MAIN].sm_addr_offset;
	g_apfs.ac_sm_blocks_per_chunk = sm->sm_blocks_per_chunk;
	g_apfs.ac_sm_ip_base         = sm->sm_ip_base;
	g_apfs.ac_sm_ip_blocks       = sm->sm_ip_block_count;
	for (i = 0; i < APFS_SFQ_COUNT; i++) {
		g_apfs.ac_sm_fq_count[i]  = sm->sm_fq[i].sfq_count;
		g_apfs.ac_sm_fq_oldest[i] = sm->sm_fq[i].sfq_oldest_xid;
	}
	g_apfs.ac_sm_valid = true;

	/*
	 * THREE CROSS-CHECKS, AND WHY THEY ARE NOT DECORATION
	 *
	 * Every field above was read at an offset taken from the published
	 * layout.  A layout that is remembered slightly wrong produces numbers
	 * that look entirely reasonable -- a plausible free count, a plausible
	 * chunk size -- and nothing about the block itself says otherwise.  So
	 * the block is made to agree with things that were read from OTHER
	 * blocks, by other code, at offsets already known to be right.
	 *
	 * The strongest of the three is the last.  The checkpoint map named
	 * some B-trees as free queues, and the space manager, sixty bytes into
	 * a different block, names the same oids.  Nothing but a correct
	 * sm_fq offset makes those two lists match.
	 */
	if (sm->sm_dev[APFS_SD_MAIN].sm_block_count != g_apfs.ac_block_count)
		kprintf("apfs: WARNING spaceman says %llu blocks, superblock "
		    "says %llu\n",
		    (unsigned long long)sm->sm_dev[APFS_SD_MAIN].sm_block_count,
		    (unsigned long long)g_apfs.ac_block_count);

	if (g_apfs.ac_sm_blocks_per_chunk == 0 ||
	    g_apfs.ac_sm_chunks * (uint64_t)g_apfs.ac_sm_blocks_per_chunk <
	    g_apfs.ac_block_count)
		kprintf("apfs: WARNING %llu chunks of %u do not cover %llu "
		    "blocks\n", (unsigned long long)g_apfs.ac_sm_chunks,
		    (unsigned)g_apfs.ac_sm_blocks_per_chunk,
		    (unsigned long long)g_apfs.ac_block_count);

	for (i = 0; i < APFS_SFQ_COUNT; i++) {
		if (sm->sm_fq[i].sfq_tree_oid == 0)
			continue;
		if (!ephemeral_is_free_queue(sm->sm_fq[i].sfq_tree_oid))
			kprintf("apfs: WARNING free queue %u names tree oid "
			    "%llu, which the checkpoint map does not\n",
			    (unsigned)i,
			    (unsigned long long)sm->sm_fq[i].sfq_tree_oid);
	}

	/*
	 * And keep it.  An ephemeral object is one whose home is memory: the
	 * disk holds a copy per checkpoint, and the checkpoint writer's job
	 * is to put the current one down.  Reading it back off the platter to
	 * change it -- which is what this file did until now -- works only
	 * while the change is also written back into the same block, and that
	 * is the in-place write the checkpoint exists to stop.
	 */
	g_sm = kmalloc(APFS_BLOCK_SIZE);
	if (g_sm == NULL)
		return (FS_APFS_E_NOMEM);
	for (i = 0; i < APFS_BLOCK_SIZE; i++)
		g_sm[i] = ((const uint8_t *)scratch)[i];
	return (FS_APFS_E_OK);
}

/*
 * THE INTERNAL POOL
 *
 * Allocation is described by the chunk bitmaps and the chunk-info blocks, and
 * those cannot be stored in the space they describe -- moving a bitmap would
 * change the answer to the question "which blocks are free" while that answer
 * was being written.  So they live in a small reserved pool, and which pool
 * blocks are in use is itself a bitmap, kept in a ring so that the version
 * belonging to a checkpoint that has not been superseded is never overwritten.
 *
 * The shape below was measured on a real container rather than remembered,
 * and its own history confirms every part of the model: over four
 * checkpoints, the chunk-info block ping-pongs between two pool blocks
 * (21017, 21019, 21017, 21019), the pool bitmap advances one ring slot each
 * time (0, 1, 2, 3), and the free-list head and tail advance with it.
 */
static uint16_t
ip_tbl_u16(uint32_t off, uint32_t i)
{

	return (*(const uint16_t *)(g_sm + off + i * sizeof(uint16_t)));
}

static void
ip_tbl_set_u16(uint32_t off, uint32_t i, uint16_t v)
{

	*(uint16_t *)(g_sm + off + i * sizeof(uint16_t)) = v;
}

static struct apfs_spaceman *
sm_mem(void)
{

	return ((struct apfs_spaceman *)g_sm);
}

/*
 * Read the pool's geometry and its live bitmap.  Everything here is checked
 * against something read elsewhere: the offsets must land inside the block,
 * the ring slot must be one of the ring's, and -- the strongest of the three
 * -- the two blocks the chunk walk already found (the chunk's bitmap and its
 * chunk-info block) must be inside the pool AND marked taken in it.  Nothing
 * but a correct reading of all these fields makes that last one true.
 */
static int
ip_load(void)
{
	const struct apfs_spaceman	*sm;
	uint64_t			 probe[2];
	uint32_t			 i;

	g_apfs.ac_ip_valid = false;
	if (g_sm == NULL || !g_apfs.ac_sm_valid)
		return (FS_APFS_E_INVAL);
	sm = sm_mem();

	if (sm->sm_ip_bm_size_in_blocks != 1) {
		kprintf("apfs: pool bitmap is %u blocks -- only one is "
		    "handled\n", (unsigned)sm->sm_ip_bm_size_in_blocks);
		return (FS_APFS_E_INVAL);
	}
	if (sm->sm_ip_bm_block_count == 0 ||
	    sm->sm_ip_block_count == 0 ||
	    sm->sm_ip_block_count > APFS_BLOCK_SIZE * 8) {
		kprintf("apfs: pool of %llu blocks with a %u-slot ring makes "
		    "no sense\n", (unsigned long long)sm->sm_ip_block_count,
		    (unsigned)sm->sm_ip_bm_block_count);
		return (FS_APFS_E_INVAL);
	}
	/*
	 * The three tables are byte offsets into this same block, so a wrong
	 * reading of any of them is a read of the block's own bytes as a
	 * table -- which produces numbers, not a fault.  Bound them.
	 */
	if (sm->sm_ip_bm_xid_offset + sizeof(uint64_t) > APFS_BLOCK_SIZE ||
	    sm->sm_ip_bitmap_offset + sizeof(uint16_t) > APFS_BLOCK_SIZE ||
	    sm->sm_ip_bm_free_next_offset +
	    sm->sm_ip_bm_block_count * sizeof(uint16_t) > APFS_BLOCK_SIZE) {
		kprintf("apfs: pool tables at +%u/+%u/+%u do not fit in a "
		    "block\n", (unsigned)sm->sm_ip_bm_xid_offset,
		    (unsigned)sm->sm_ip_bitmap_offset,
		    (unsigned)sm->sm_ip_bm_free_next_offset);
		return (FS_APFS_E_INVAL);
	}

	g_apfs.ac_ip_base    = sm->sm_ip_base;
	g_apfs.ac_ip_blocks  = sm->sm_ip_block_count;
	g_apfs.ac_ipbm_base  = sm->sm_ip_bm_base;
	g_apfs.ac_ipbm_slots = sm->sm_ip_bm_block_count;
	g_apfs.ac_ipbm_slot  = ip_tbl_u16(sm->sm_ip_bitmap_offset, 0);
	if (g_apfs.ac_ipbm_slot >= g_apfs.ac_ipbm_slots) {
		kprintf("apfs: pool bitmap claims ring slot %u of %u\n",
		    (unsigned)g_apfs.ac_ipbm_slot,
		    (unsigned)g_apfs.ac_ipbm_slots);
		return (FS_APFS_E_INVAL);
	}

	/*
	 * Only one chunk-info block is handled, because only then is the
	 * space manager's cib_addr[] a single number to move.  Every
	 * container this has been run against has one; a bigger one needs
	 * the walk, not a bigger constant.
	 */
	if (g_apfs.ac_sm_cib_count != 1 || g_apfs.ac_sm_cab_count != 0) {
		kprintf("apfs: %u chunk-info blocks and %u address blocks -- "
		    "allocation metadata will not be moved\n",
		    (unsigned)g_apfs.ac_sm_cib_count,
		    (unsigned)g_apfs.ac_sm_cab_count);
		return (FS_APFS_E_INVAL);
	}
	if (g_apfs.ac_sm_addr_offset + sizeof(uint64_t) > APFS_BLOCK_SIZE)
		return (FS_APFS_E_INVAL);

	g_ipbm = kmalloc(APFS_BLOCK_SIZE);
	if (g_ipbm == NULL)
		return (FS_APFS_E_NOMEM);
	/* Raw: a bitmap has no object header, so it has no checksum. */
	if (read_block_raw(g_apfs.ac_ipbm_base + g_apfs.ac_ipbm_slot,
	    g_ipbm) != FS_APFS_E_OK) {
		kprintf("apfs: pool bitmap at %llu unreadable\n",
		    (unsigned long long)(g_apfs.ac_ipbm_base +
		    g_apfs.ac_ipbm_slot));
		return (FS_APFS_E_IO);
	}

	probe[0] = g_apfs.ac_alloc_bitmap;
	probe[1] = g_apfs.ac_alloc_cib;
	for (i = 0; g_apfs.ac_alloc_have && i < 2; i++) {
		uint64_t	off;

		if (probe[i] < g_apfs.ac_ip_base ||
		    probe[i] >= g_apfs.ac_ip_base + g_apfs.ac_ip_blocks) {
			kprintf("apfs: block %llu describes allocation but is "
			    "outside the pool %llu+%llu\n",
			    (unsigned long long)probe[i],
			    (unsigned long long)g_apfs.ac_ip_base,
			    (unsigned long long)g_apfs.ac_ip_blocks);
			return (FS_APFS_E_INVAL);
		}
		off = probe[i] - g_apfs.ac_ip_base;
		if ((g_ipbm[off >> 3] & (uint8_t)(1u << (off & 7u))) == 0) {
			kprintf("apfs: pool block %llu is in use but its "
			    "bitmap calls it free\n",
			    (unsigned long long)probe[i]);
			return (FS_APFS_E_INVAL);
		}
	}

	/*
	 * And the chunk-info block, for the same reason: it changes many times
	 * per checkpoint and is written once.  The bitmaps it names are brought
	 * in one at a time, as blocks in their chunks are wanted -- starting
	 * with the one metadata comes from, which is admitted here so that a
	 * container whose allocator cannot start says so at mount rather than
	 * at the first write.
	 */
	g_cib = kmalloc(APFS_BLOCK_SIZE);
	if (g_cib == NULL)
		return (FS_APFS_E_NOMEM);
	if (fs_apfs_read_block(g_apfs.ac_alloc_cib, g_cib) != FS_APFS_E_OK) {
		kprintf("apfs: chunk-info block %llu would not read\n",
		    (unsigned long long)g_apfs.ac_alloc_cib);
		return (FS_APFS_E_IO);
	}
	if (g_apfs.ac_alloc_have) {
		g_home = chunk_for(g_apfs.ac_alloc_base);
		if (g_home == NULL) {
			kprintf("apfs: the chunk metadata was to come from "
			    "cannot be held -- nothing will be written\n");
			g_apfs.ac_alloc_have = false;
		}
	}

	/*
	 * The free-queue trees.  Ephemeral like the space manager that names
	 * them, so they are read once and written by the checkpoint writer.
	 */
	for (i = 0; i < APFS_SFQ_COUNT; i++) {
		uint64_t	tree_oid;
		uint64_t	tree_bno;

		tree_oid = sm->sm_fq[i].sfq_tree_oid;
		if (tree_oid == 0)
			continue;
		tree_bno = resolve_ephemeral(tree_oid);
		if (tree_bno == 0) {
			kprintf("apfs: free queue %u names tree oid %llu, "
			    "which no checkpoint map places\n", (unsigned)i,
			    (unsigned long long)tree_oid);
			return (FS_APFS_E_INVAL);
		}
		g_fq[i] = kmalloc(APFS_BLOCK_SIZE);
		if (g_fq[i] == NULL)
			return (FS_APFS_E_NOMEM);
		if (fs_apfs_read_block(tree_bno, g_fq[i]) != FS_APFS_E_OK) {
			kprintf("apfs: free-queue tree at %llu unreadable\n",
			    (unsigned long long)tree_bno);
			return (FS_APFS_E_IO);
		}
	}

	g_apfs.ac_ip_valid = true;
	kprintf("apfs: internal pool %llu+%llu, bitmap ring @%llu (%u slots), "
	    "slot %u live for xid %llu\n",
	    (unsigned long long)g_apfs.ac_ip_base,
	    (unsigned long long)g_apfs.ac_ip_blocks,
	    (unsigned long long)g_apfs.ac_ipbm_base,
	    (unsigned)g_apfs.ac_ipbm_slots, (unsigned)g_apfs.ac_ipbm_slot,
	    (unsigned long long)*(const uint64_t *)(g_sm +
	    sm->sm_ip_bm_xid_offset));
	return (FS_APFS_E_OK);
}

/*
 * Take a pool block, or 0 if the pool is full.  A block that has been
 * released but not yet let go by the free queue is still marked in use here,
 * which is exactly how the queue keeps it out of reach.
 */
static uint64_t
ip_alloc(void)
{
	uint64_t	i;

	if (!g_apfs.ac_ip_valid)
		return (0);
	for (i = 0; i < g_apfs.ac_ip_blocks; i++) {
		if ((g_ipbm[i >> 3] & (uint8_t)(1u << (i & 7u))) != 0)
			continue;
		g_ipbm[i >> 3] |= (uint8_t)(1u << (i & 7u));
		ip_n_alloc++;
		return (g_apfs.ac_ip_base + i);
	}
	kprintf("apfs: the internal pool is full (%llu blocks, %llu waiting in "
	    "its free queue)\n", (unsigned long long)g_apfs.ac_ip_blocks,
	    (unsigned long long)(g_sm != NULL ?
	    sm_mem()->sm_fq[APFS_SFQ_IP].sfq_count : 0));
	return (0);
}

/*
 * Give a pool block back -- which means putting it in the pool's free queue,
 * not clearing its bit.  It stays marked in use, so nothing hands it out,
 * until the transaction that released it is far enough behind.
 */
static void
ip_free(uint64_t bno)
{
	uint64_t	i;

	if (!g_apfs.ac_ip_valid || bno < g_apfs.ac_ip_base ||
	    bno >= g_apfs.ac_ip_base + g_apfs.ac_ip_blocks) {
		kprintf("apfs: ip_free(%llu) -- not a pool block\n",
		    (unsigned long long)bno);
		return;
	}
	i = bno - g_apfs.ac_ip_base;
	if ((g_ipbm[i >> 3] & (uint8_t)(1u << (i & 7u))) == 0) {
		kprintf("apfs: pool block %llu freed twice\n",
		    (unsigned long long)bno);
		return;
	}
	if (fq_insert(APFS_SFQ_IP, g_apfs.ac_xid + 1, bno, 1) != FS_APFS_E_OK)
		kprintf("apfs: pool block %llu could not be queued -- it is "
		    "leaked until the next mount\n", (unsigned long long)bno);
	ip_n_free++;
}

/*
 * Blocks have become free again: move the two counters that say so.  The
 * bits are the caller's business; this is the half that keeps the chunk-info
 * and the space manager agreeing with them.
 */
static void
alloc_count_free(const struct alloc_chunk *ch, uint64_t count)
{
	struct apfs_chunk_info_block	*cib;
	struct apfs_spaceman		*sm;

	if (g_cib == NULL || g_sm == NULL || ch == NULL)
		return;
	cib = (struct apfs_chunk_info_block *)g_cib;
	cib->cib_chunk_info[ch->ch_slot].ci_free_count += (uint32_t)count;
	sm = sm_mem();
	sm->sm_dev[APFS_SD_MAIN].sm_free_count += count;
	g_apfs.ac_sm_free = sm->sm_dev[APFS_SD_MAIN].sm_free_count;
}

/*
 * THE FREE QUEUES
 *
 * A block released by a copy is not free.  The checkpoint that is still live
 * points at it, and so do the checkpoints behind that one -- the descriptor
 * ring keeps them on purpose, and the whole claim of a ring is that an older
 * checkpoint is still a filesystem.  Hand the block back at once and that
 * claim quietly stops being true: the superblock is still there, still
 * checksums, and leads to blocks something else has since written.
 *
 * That is not hypothetical.  Before this, a container this kernel had been
 * writing for a while held twelve superblocks of which four were corpses:
 *
 *	xid 2   block 98304 is no longer an omap (type 0x03)
 *	xid 5   the omap tree at 98323 no longer maps the volume
 *
 * So a release goes into a queue instead, keyed by the transaction that made
 * it, and the block stays marked in use until that transaction is far enough
 * behind for nobody to want it.  The format has a place for exactly this --
 * two B-trees, one for the device and one for the internal pool, both named
 * by the space manager and both ephemeral -- and the container arrives with
 * entries already in them.
 *
 * Their shape was measured rather than assumed: fixed-size keys and values,
 * key (xid, paddr) of 16 bytes, value a block count of 8 -- and an offset of
 * 0xFFFF where a value would be, which means the count is one and no value is
 * stored.  Both forms appear in the container as mkapfs leaves it.
 */
#define	APFS_FQ_GHOST		0xFFFFU		/* "no value; the count is 1" */
#define	APFS_FQ_KEEP		4		/* checkpoints kept readable  */

static uint64_t	 fq_n_queued;			/* blocks put in         */
static uint64_t	 fq_n_released;			/* ...and let go again   */

struct fq_node {
	uint8_t		*fn_toc;
	uint8_t		*fn_keys;
	uint8_t		*fn_vals;	/* one past the last value byte */
	uint32_t	 fn_nkeys;
	uint32_t	 fn_toc_len;
};

static void
fq_layout(uint8_t *node, struct fq_node *out)
{
	const struct apfs_btree_node_phys	*n;

	n = (const struct apfs_btree_node_phys *)node;
	out->fn_nkeys   = n->btn_nkeys;
	out->fn_toc_len = n->btn_table_space.nl_len;
	out->fn_toc     = node + APFS_BTNODE_HDR_SIZE +
	    n->btn_table_space.nl_off;
	out->fn_keys    = out->fn_toc + out->fn_toc_len;
	out->fn_vals    = node + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE;
}

static void
fq_entry(const struct fq_node *fn, uint32_t i, uint16_t *koff, uint16_t *voff)
{
	const uint16_t	*toc;

	toc = (const uint16_t *)fn->fn_toc;
	*koff = toc[i * 2];
	*voff = toc[i * 2 + 1];
}

static void
fq_key(const struct fq_node *fn, uint32_t i, uint64_t *xid, uint64_t *paddr)
{
	const struct apfs_spaceman_free_queue_key	*k;
	uint16_t					 koff;
	uint16_t					 voff;

	fq_entry(fn, i, &koff, &voff);
	k = (const struct apfs_spaceman_free_queue_key *)(fn->fn_keys + koff);
	*xid   = k->sfqk_xid;
	*paddr = k->sfqk_paddr;
}

static uint64_t
fq_count_at(const struct fq_node *fn, uint32_t i)
{
	uint16_t	koff;
	uint16_t	voff;

	fq_entry(fn, i, &koff, &voff);
	if (voff == APFS_FQ_GHOST)
		return (1);
	return (*(const uint64_t *)(fn->fn_vals - voff));
}

/*
 * Give a removed key or value back to the node's free list.
 *
 * NOT OPTIONAL, though it looked it.  A node's key area is divided between
 * the keys the table of contents points at and a chain of holes -- each hole
 * holding, in its first four bytes, the offset of the next and its own length
 * -- and the header carries the total.  A checker rebuilds both sides and
 * compares: "B-tree: wrong free space total for key area" is what it says
 * about space that is neither used nor listed, which is what dropping an
 * entry without this leaves behind.
 *
 * A freed key is sixteen bytes and a freed value eight, so each is large
 * enough to hold the four-byte header its own hole needs.  Value offsets are
 * measured back from the end of the value area, which is the convention the
 * table of contents already uses.
 */
static void
fq_free_key(struct apfs_btree_node_phys *n, const struct fq_node *fn,
    uint16_t koff)
{
	struct apfs_nloc	*hole;

	hole = (struct apfs_nloc *)(fn->fn_keys + koff);
	hole->nl_off = n->btn_key_free_list.nl_off;
	hole->nl_len = (uint16_t)sizeof(struct apfs_spaceman_free_queue_key);
	n->btn_key_free_list.nl_off = koff;
	n->btn_key_free_list.nl_len = (uint16_t)(n->btn_key_free_list.nl_len +
	    sizeof(struct apfs_spaceman_free_queue_key));
}

static void
fq_free_val(struct apfs_btree_node_phys *n, const struct fq_node *fn,
    uint16_t voff)
{
	struct apfs_nloc	*hole;

	hole = (struct apfs_nloc *)(fn->fn_vals - voff);
	hole->nl_off = n->btn_val_free_list.nl_off;
	hole->nl_len = 8;
	n->btn_val_free_list.nl_off = voff;
	n->btn_val_free_list.nl_len =
	    (uint16_t)(n->btn_val_free_list.nl_len + 8);
}

/*
 * Put (xid, paddr, count) into queue `q`, keeping the table of contents in
 * key order.
 *
 * Space comes from a HOLE a release left behind if there is one, and only
 * otherwise from the node's free span.  Every hole in this node is exactly the
 * right size -- one queue key is as long as any other, and so is one count --
 * so taking one is popping the head of a list rather than searching it, which
 * is why the fs tree's inserts can go on ignoring their own free lists and
 * this one cannot.
 *
 * WITHOUT THAT THE NODE BLEEDS, and it took a busier boot to see it.  The span
 * only ever shrinks and the chain only ever grows, so a container that queues
 * and releases for long enough runs out of span while the queue is nearly
 * empty and starts losing blocks it has no room to record.  Measured the first
 * time truncation gave a boot enough work to reach it: "free queue 1 is still
 * full" printed with EIGHT keys in a node that had just been holding ninety.
 */
static int
fq_insert(uint32_t q, uint64_t xid, uint64_t paddr, uint64_t count)
{
	struct apfs_btree_node_phys		*n;
	struct apfs_spaceman_free_queue_key	*k;
	struct apfs_spaceman			*sm;
	struct apfs_nloc			*hole;
	struct fq_node				 fn;
	uint16_t				*toc;
	uint64_t				 exid;
	uint64_t				 epaddr;
	uint32_t				 need;
	uint32_t				 pos;
	uint32_t				 i;
	uint16_t				 koff;
	uint16_t				 voff;
	bool					 keyhole;
	bool					 valhole;

	if (q >= APFS_SFQ_COUNT || g_fq[q] == NULL || g_sm == NULL)
		return (FS_APFS_E_INVAL);
	n = (struct apfs_btree_node_phys *)g_fq[q];
	fq_layout(g_fq[q], &fn);

	/*
	 * The list's total is what says whether there is a hole: a chain with
	 * bytes in it has a head, and asking the total rather than the head
	 * keeps this from depending on which value means "none".
	 */
	keyhole = n->btn_key_free_list.nl_len >= sizeof(*k);
	valhole = n->btn_val_free_list.nl_len >= 8u;
	need    = (keyhole ? 0u : (uint32_t)sizeof(*k)) +
	    ((count > 1 && !valhole) ? 8u : 0u);
	if ((uint32_t)(fn.fn_nkeys + 1) * 4u > fn.fn_toc_len ||
	    n->btn_free_space.nl_len < need) {
		/*
		 * The queue is one node, so the history it can hold is
		 * bounded, and this is where the bound bites.  Rather than
		 * lose the block, give up the history: everything but the
		 * newest transaction is let go early and the insert is tried
		 * once more.  Loudly, because a container doing this
		 * regularly has stopped keeping the promise the ring makes.
		 */
		kprintf("apfs: free queue %u is full (%u keys) -- releasing "
		    "everything before xid %llu early\n", (unsigned)q,
		    (unsigned)fn.fn_nkeys, (unsigned long long)xid);
		fq_release(q, xid - 1);
		fq_layout(g_fq[q], &fn);
		keyhole = n->btn_key_free_list.nl_len >= sizeof(*k);
		valhole = n->btn_val_free_list.nl_len >= 8u;
		need    = (keyhole ? 0u : (uint32_t)sizeof(*k)) +
		    ((count > 1 && !valhole) ? 8u : 0u);
		if ((uint32_t)(fn.fn_nkeys + 1) * 4u > fn.fn_toc_len ||
		    n->btn_free_space.nl_len < need) {
			kprintf("apfs: free queue %u is still full -- block "
			    "%llu is leaked until the next mount\n",
			    (unsigned)q, (unsigned long long)paddr);
			return (FS_APFS_E_NOALLOC);
		}
	}

	/* Where the key belongs, in (xid, paddr) order. */
	for (pos = 0; pos < fn.fn_nkeys; pos++) {
		fq_key(&fn, pos, &exid, &epaddr);
		if (exid > xid || (exid == xid && epaddr > paddr))
			break;
	}

	if (keyhole) {
		koff = n->btn_key_free_list.nl_off;
		hole = (struct apfs_nloc *)(fn.fn_keys + koff);
		n->btn_key_free_list.nl_off = hole->nl_off;
		n->btn_key_free_list.nl_len =
		    (uint16_t)(n->btn_key_free_list.nl_len - sizeof(*k));
	} else {
		koff = n->btn_free_space.nl_off;
		n->btn_free_space.nl_off = (uint16_t)(koff + sizeof(*k));
		n->btn_free_space.nl_len =
		    (uint16_t)(n->btn_free_space.nl_len - sizeof(*k));
	}
	k = (struct apfs_spaceman_free_queue_key *)(fn.fn_keys + koff);
	k->sfqk_xid   = xid;
	k->sfqk_paddr = paddr;

	if (count > 1) {
		if (valhole) {
			voff = n->btn_val_free_list.nl_off;
			hole = (struct apfs_nloc *)(fn.fn_vals - voff);
			n->btn_val_free_list.nl_off = hole->nl_off;
			n->btn_val_free_list.nl_len =
			    (uint16_t)(n->btn_val_free_list.nl_len - 8u);
		} else {
			/*
			 * Values grow down from the end of the node, so the
			 * next one sits at the top of what is left of the free
			 * span -- which is where it is whether or not the key
			 * above came out of the span too.
			 */
			n->btn_free_space.nl_len =
			    (uint16_t)(n->btn_free_space.nl_len - 8u);
			voff = (uint16_t)(APFS_BLOCK_SIZE -
			    APFS_BTREE_INFO_SIZE -
			    (APFS_BTNODE_HDR_SIZE + n->btn_table_space.nl_off +
			    fn.fn_toc_len + n->btn_free_space.nl_off +
			    n->btn_free_space.nl_len));
		}
		*(uint64_t *)(fn.fn_vals - voff) = count;
	} else
		voff = APFS_FQ_GHOST;

	toc = (uint16_t *)fn.fn_toc;
	for (i = fn.fn_nkeys; i > pos; i--) {
		toc[i * 2]     = toc[(i - 1) * 2];
		toc[i * 2 + 1] = toc[(i - 1) * 2 + 1];
	}
	toc[pos * 2]     = koff;
	toc[pos * 2 + 1] = voff;
	n->btn_nkeys++;
	*(uint64_t *)(g_fq[q] + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE +
	    APFS_BTREE_INFO_KEYCOUNT) += 1;

	sm = sm_mem();
	sm->sm_fq[q].sfq_count += count;
	if (sm->sm_fq[q].sfq_oldest_xid == 0 ||
	    xid < sm->sm_fq[q].sfq_oldest_xid)
		sm->sm_fq[q].sfq_oldest_xid = xid;
	fq_n_queued += count;
	return (FS_APFS_E_OK);
}

/*
 * Let go of everything queued at `upto_xid` or earlier: clear the bits, move
 * the counters, and drop the entries.
 *
 * This is the only place a block becomes free again, and the xid is what
 * makes it safe -- by the time a transaction is APFS_FQ_KEEP checkpoints
 * behind, no superblock still worth mounting refers to what it released.
 */
static void
fq_release(uint32_t q, uint64_t upto_xid)
{
	struct apfs_btree_node_phys	*n;
	struct apfs_spaceman		*sm;
	struct fq_node			 fn;
	uint16_t			*toc;
	uint64_t			 oldest;
	uint64_t			 xid;
	uint64_t			 paddr;
	uint64_t			 count;
	uint64_t			 bit;
	uint32_t			 i;
	uint32_t			 j;
	uint32_t			 kept;

	if (q >= APFS_SFQ_COUNT || g_fq[q] == NULL || g_sm == NULL)
		return;
	n   = (struct apfs_btree_node_phys *)g_fq[q];
	sm  = sm_mem();
	fq_layout(g_fq[q], &fn);
	toc = (uint16_t *)fn.fn_toc;

	kept   = 0;
	oldest = 0;
	for (i = 0; i < fn.fn_nkeys; i++) {
		struct alloc_chunk	*ch;
		uint16_t		 koff;
		uint16_t		 voff;
		bool			 hold;

		fq_entry(&fn, i, &koff, &voff);
		fq_key(&fn, i, &xid, &paddr);
		count = fq_count_at(&fn, i);

		/*
		 * Two reasons to keep an entry: it is too young to let go, or
		 * its chunk is not in reach.  The second used to drop the entry
		 * anyway -- the block stayed marked in use with nothing left to
		 * say who owed it, which is a leak that survives the mount.
		 * Holding it costs a queue slot and gets another go next
		 * checkpoint, by which time the resident set has usually moved.
		 */
		hold = (xid > upto_xid);
		ch   = NULL;
		if (!hold && q != APFS_SFQ_IP) {
			ch = chunk_for(paddr);
			if (ch == NULL || paddr < ch->ch_base || paddr + count >
			    ch->ch_base + ch->ch_blocks) {
				kprintf("apfs: queued block %llu is in a chunk "
				    "this checkpoint cannot reach -- held\n",
				    (unsigned long long)paddr);
				hold = true;
			}
		}
		if (hold) {
			if (kept != i) {
				toc[kept * 2]     = toc[i * 2];
				toc[kept * 2 + 1] = toc[i * 2 + 1];
			}
			if (oldest == 0 || xid < oldest)
				oldest = xid;
			kept++;
			continue;
		}

		if (q == APFS_SFQ_IP) {
			for (j = 0; j < count; j++) {
				if (paddr + j < g_apfs.ac_ip_base)
					continue;
				bit = paddr + j - g_apfs.ac_ip_base;
				if (bit >= g_apfs.ac_ip_blocks)
					continue;
				g_ipbm[bit >> 3] &=
				    (uint8_t)~(1u << (bit & 7u));
			}
		} else {
			bit = paddr - ch->ch_base;
			for (j = 0; j < count; j++)
				ch->ch_bm[(bit + j) >> 3] &=
				    (uint8_t)~(1u << ((bit + j) & 7u));
			alloc_count_free(ch, count);
			ch->ch_dirty = true;
		}
		sm->sm_fq[q].sfq_count -= count;
		fq_n_released += count;
		fq_free_key(n, &fn, koff);
		if (voff != APFS_FQ_GHOST)
			fq_free_val(n, &fn, voff);
	}

	if (kept == fn.fn_nkeys)
		return;
	n->btn_nkeys = kept;
	*(uint64_t *)(g_fq[q] + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE +
	    APFS_BTREE_INFO_KEYCOUNT) = kept;
	sm->sm_fq[q].sfq_oldest_xid = oldest;

	/*
	 * An empty queue gets its node back.  Key and value space is not
	 * returned entry by entry, so without this the node would slowly fill
	 * with holes; emptying is common enough that this is all the
	 * housekeeping it needs.
	 *
	 * THE FREE LISTS HAVE TO GO WITH IT.  A node records its holes in two
	 * chains threaded through the key and value areas -- a length in the
	 * header and, at each hole, an offset to the next -- and a checker
	 * walks them.  Widening the free span without emptying those chains
	 * leaves them pointing into space that is now unallocated, so the walk
	 * reads whatever is there as a hole header: "B-tree node: free key is
	 * too small", which is what apfsck said the first time this ran.
	 */
	if (kept == 0) {
		n->btn_free_space.nl_off = 0;
		n->btn_free_space.nl_len = (uint16_t)(APFS_BLOCK_SIZE -
		    APFS_BTREE_INFO_SIZE - APFS_BTNODE_HDR_SIZE -
		    n->btn_table_space.nl_off - fn.fn_toc_len);
		n->btn_key_free_list.nl_off = APFS_FQ_GHOST;
		n->btn_key_free_list.nl_len = 0;
		n->btn_val_free_list.nl_off = APFS_FQ_GHOST;
		n->btn_val_free_list.nl_len = 0;
	}
}

/* The in-memory copy of an ephemeral free-queue tree, or NULL. */
static const uint8_t *
fq_mem(uint64_t oid)
{
	const struct apfs_spaceman	*sm;
	uint32_t			 i;

	if (g_sm == NULL)
		return (NULL);
	sm = sm_mem();
	for (i = 0; i < APFS_SFQ_COUNT; i++) {
		if (g_fq[i] != NULL && sm->sm_fq[i].sfq_tree_oid == oid)
			return (g_fq[i]);
	}
	return (NULL);
}

/*
 * Put the pool's bitmap down in a fresh ring slot and tell the space manager
 * where it went.  The slot it replaces goes to the TAIL of the free list, not
 * the head, which is what makes the ring a queue and gives the checkpoints
 * behind this one their bitmaps for as long as the ring is deep.
 *
 * Written before the space manager that names it, and both before the
 * superblock: nothing here is reachable until that last write lands.
 */
static int
ip_rotate(uint64_t xid, uint32_t *slot_out)
{
	struct apfs_spaceman	*sm;
	uint32_t		 next;
	uint32_t		 old;
	uint32_t		 s;

	sm = sm_mem();
	s = sm->sm_ip_bm_free_head;
	if (s >= g_apfs.ac_ipbm_slots) {
		kprintf("apfs: the pool bitmap ring has no free slot "
		    "(head %u of %u)\n", (unsigned)s,
		    (unsigned)g_apfs.ac_ipbm_slots);
		return (FS_APFS_E_NOALLOC);
	}
	old  = g_apfs.ac_ipbm_slot;
	next = ip_tbl_u16(sm->sm_ip_bm_free_next_offset, s);

	if (write_block_raw(g_apfs.ac_ipbm_base + s, g_ipbm) != FS_APFS_E_OK) {
		kprintf("apfs: pool bitmap would not write to slot %u\n",
		    (unsigned)s);
		return (FS_APFS_E_IO);
	}

	sm->sm_ip_bm_free_head = (uint16_t)next;
	/*
	 * The slot just taken leaves the list, and a slot outside the list is
	 * spelled 0xFFFF -- not merely unreferenced.  A checker walks the
	 * chain from head to tail, counts it, and requires exactly
	 * sm_ip_bm_size_in_blocks slots to be missing from it; leaving this
	 * entry pointing at its old successor makes the live bitmap look
	 * free, and the count comes out one short.
	 */
	ip_tbl_set_u16(sm->sm_ip_bm_free_next_offset, s, 0xFFFFU);
	if (sm->sm_ip_bm_free_tail < g_apfs.ac_ipbm_slots)
		ip_tbl_set_u16(sm->sm_ip_bm_free_next_offset,
		    sm->sm_ip_bm_free_tail, (uint16_t)old);
	ip_tbl_set_u16(sm->sm_ip_bm_free_next_offset, old, 0xFFFFU);
	sm->sm_ip_bm_free_tail = (uint16_t)old;
	if (sm->sm_ip_bm_free_head >= g_apfs.ac_ipbm_slots)
		sm->sm_ip_bm_free_head = (uint16_t)old;

	*(uint64_t *)(g_sm + sm->sm_ip_bm_xid_offset) = xid;
	ip_tbl_set_u16(sm->sm_ip_bitmap_offset, 0, (uint16_t)s);
	*slot_out = s;
	return (FS_APFS_E_OK);
}

/* Blocks reported free by one chunk's bitmap: the CLEAR bits in it. */
static uint32_t
bitmap_free_count(const uint8_t *bm, uint32_t blocks)
{
	uint32_t	free;
	uint32_t	i;

	free = 0;
	for (i = 0; i < blocks; i++) {
		if ((bm[i >> 3] & (uint8_t)(1u << (i & 7u))) == 0)
			free++;
	}
	return (free);
}

/*
 * Walk the chunk-info blocks and, for as many chunks as the budget allows,
 * count the bits.
 *
 * There are two costs here and they scale differently, which is why they are
 * separated.  Reading the chunk-info blocks is cheap and bounded by the
 * container's size divided by four million blocks -- a terabyte is sixty-five
 * of them -- so the totals they record are always checked.  Counting bits
 * means one block read per chunk, which for that same terabyte is eight
 * thousand reads at mount time, so it stops after APFS_BM_SCAN_MAX chunks and
 * SAYS how many it did not look at.  A verification that quietly examined a
 * fraction and reported success would be worse than none.
 *
 * The check itself is three numbers that come from three places: the space
 * manager's free count, the sum of the per-chunk counts recorded in the
 * chunk-info blocks, and the number of clear bits actually in the bitmaps.
 * Any pair agreeing while the third differs says exactly where the reader is
 * wrong.
 */
#define	APFS_BM_SCAN_MAX	64

static int
verify_chunk_bitmaps(void *sm_buf, void *cib_buf, void *bm_buf)
{
	const struct apfs_chunk_info_block	*cib;
	const struct apfs_chunk_info		*ci;
	const uint8_t				*p;
	uint64_t				 cib_addr;
	uint32_t				 count;
	uint32_t				 counted;
	uint32_t				 c;
	uint32_t				 i;

	g_apfs.ac_bm_valid = false;
	if (!g_apfs.ac_sm_valid)
		return (FS_APFS_E_INVAL);
	/*
	 * A container large enough to need chunk-info ADDRESS blocks has one
	 * more level between the space manager and the chunks.  Nothing here
	 * produces one, and guessing at a level this code has never seen read
	 * would be worse than declining.
	 */
	if (g_apfs.ac_sm_cab_count != 0) {
		kprintf("apfs: %u chunk-info address block(s) -- bitmap check "
		    "not implemented for that layout\n",
		    (unsigned)g_apfs.ac_sm_cab_count);
		return (FS_APFS_E_INVAL);
	}
	if (g_apfs.ac_sm_addr_offset + g_apfs.ac_sm_cib_count * 8u >
	    APFS_BLOCK_SIZE) {
		kprintf("apfs: %u cib addresses at +%u run past the space "
		    "manager's block\n", (unsigned)g_apfs.ac_sm_cib_count,
		    (unsigned)g_apfs.ac_sm_addr_offset);
		return (FS_APFS_E_INVAL);
	}

	/* The cib addresses live inside the space manager's own block. */
	if (fs_apfs_read_block(g_apfs.ac_sm_paddr, sm_buf) != FS_APFS_E_OK) {
		kprintf("apfs: space manager block %llu unreadable on the "
		    "second pass\n", (unsigned long long)g_apfs.ac_sm_paddr);
		return (FS_APFS_E_IO);
	}
	p = (const uint8_t *)sm_buf + g_apfs.ac_sm_addr_offset;

	for (c = 0; c < g_apfs.ac_sm_cib_count; c++) {
		mem_copy((uint8_t *)&cib_addr, p + c * 8, 8);
		if (fs_apfs_read_block(cib_addr, cib_buf) != FS_APFS_E_OK) {
			kprintf("apfs: chunk-info block %llu unreadable or "
			    "fails its checksum\n",
			    (unsigned long long)cib_addr);
			return (FS_APFS_E_IO);
		}
		cib = (const struct apfs_chunk_info_block *)cib_buf;
		if ((cib->cib_o.o_type & APFS_OBJ_TYPE_MASK) !=
		    APFS_OBJ_SPACEMAN_CIB) {
			kprintf("apfs: block %llu is not a chunk-info block "
			    "(type 0x%x)\n", (unsigned long long)cib_addr,
			    (unsigned)cib->cib_o.o_type);
			return (FS_APFS_E_INVAL);
		}
		count = cib->cib_chunk_info_count;
		if (count > APFS_CI_MAX_PER_CIB)
			count = APFS_CI_MAX_PER_CIB;

		for (i = 0; i < count; i++) {
			ci = &cib->cib_chunk_info[i];
			g_apfs.ac_bm_chunks++;
			g_apfs.ac_bm_blocks    += ci->ci_block_count;
			g_apfs.ac_bm_free_said += ci->ci_free_count;

			if (ci->ci_bitmap_addr == 0) {
				/*
				 * No bitmap at all.  The chunk is wholly free,
				 * and a chunk claiming otherwise without one
				 * is a reader that has the convention
				 * backwards.
				 */
				g_apfs.ac_bm_wholly_free++;
				g_apfs.ac_bm_free_counted += ci->ci_block_count;
				if (ci->ci_free_count != ci->ci_block_count)
					g_apfs.ac_bm_disagreed++;
				continue;
			}
			if (g_apfs.ac_bm_scanned >= APFS_BM_SCAN_MAX) {
				/* Budget spent; its free count is taken on
				 * trust, and the summary says so. */
				g_apfs.ac_bm_free_counted += ci->ci_free_count;
				continue;
			}
			if (ci->ci_block_count > APFS_BLOCK_SIZE * 8u) {
				kprintf("apfs: chunk @%llu claims %u blocks, "
				    "more than a bitmap block holds\n",
				    (unsigned long long)ci->ci_addr,
				    (unsigned)ci->ci_block_count);
				return (FS_APFS_E_INVAL);
			}
			/*
			 * Read RAW.  A bitmap block is bits and nothing else:
			 * no obj_phys, so no checksum, so its first eight
			 * bytes are the allocation state of the chunk's first
			 * sixty-four blocks and not a Fletcher-64.  Reading it
			 * through the checked reader rejects every bitmap in
			 * the container -- which is exactly what it did, and
			 * silently, until this walk started saying why it
			 * stopped.
			 */
			if (read_block_raw(ci->ci_bitmap_addr, bm_buf) !=
			    FS_APFS_E_OK) {
				kprintf("apfs: bitmap block %llu unreadable\n",
				    (unsigned long long)ci->ci_bitmap_addr);
				return (FS_APFS_E_IO);
			}
			counted = bitmap_free_count((const uint8_t *)bm_buf,
			    ci->ci_block_count);
			g_apfs.ac_bm_scanned++;
			if (!g_apfs.ac_alloc_have && counted > 64) {
				g_apfs.ac_alloc_have   = true;
				g_apfs.ac_alloc_cib    = cib_addr;
				g_apfs.ac_alloc_slot   = i;
				g_apfs.ac_alloc_bitmap = ci->ci_bitmap_addr;
				g_apfs.ac_alloc_base   = ci->ci_addr;
				g_apfs.ac_alloc_blocks = ci->ci_block_count;
			}
			g_apfs.ac_bm_free_counted += counted;
			if (counted != ci->ci_free_count) {
				g_apfs.ac_bm_disagreed++;
				kprintf("apfs: WARNING chunk @%llu says %u "
				    "free, its bitmap has %u clear bits\n",
				    (unsigned long long)ci->ci_addr,
				    (unsigned)ci->ci_free_count,
				    (unsigned)counted);
			}
		}
	}

	g_apfs.ac_bm_valid = true;
	if (g_apfs.ac_bm_free_said != g_apfs.ac_sm_free)
		kprintf("apfs: WARNING chunks total %llu free, space manager "
		    "says %llu\n", (unsigned long long)g_apfs.ac_bm_free_said,
		    (unsigned long long)g_apfs.ac_sm_free);
	if (g_apfs.ac_bm_blocks != g_apfs.ac_block_count)
		kprintf("apfs: WARNING chunks cover %llu blocks, container has "
		    "%llu\n", (unsigned long long)g_apfs.ac_bm_blocks,
		    (unsigned long long)g_apfs.ac_block_count);
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

	rv = fs_apfs_omap_lookup(ctr_omap_tree, g_apfs.ac_fs_oid, view_xid(),
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
	g_apfs.ac_next_ino  = sb->apfs_next_obj_id;
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
	    view_xid(), &g_apfs.ac_root_tree_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	g_apfs.ac_vol_omap_tree = vol_omap_tree;

	/*
	 * The rest of the spine, which only a writer needs.  Reading a file
	 * needs the fs tree and the omap that finds its nodes; changing one
	 * needs every object BETWEEN that tree and the container superblock,
	 * because moving a block means telling whatever points at it, all the
	 * way up.
	 */
	g_apfs.ac_ctr_omap_tree  = ctr_omap_tree;
	g_apfs.ac_vol_sb_bno     = apsb_bno;
	g_apfs.ac_vol_omap_bno   = sb->apfs_omap_oid;
	g_apfs.ac_root_tree_oid  = sb->apfs_root_tree_oid;
	/*
	 * And the other tree that names a file's blocks.  PHYSICAL, so its oid
	 * is already the block it lives in and no map is asked.
	 */
	g_apfs.ac_extref_bno     = sb->apfs_extentref_tree_oid;
	/*
	 * And what the volume claims to own.  A container-wide free count is
	 * not the same statement: this one says how many blocks belong to THIS
	 * volume, and apfsck checks it against the extents it can reach --
	 * "Volume superblock: bad block count", which is what the first file
	 * this kernel lengthened produced.
	 */
	g_apfs.ac_fs_alloc_count = sb->apfs_fs_alloc_count;
	return (FS_APFS_E_OK);
}

/* ---- file-system tree ----------------------------------------------------- */

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
	if (depth == 0)
		g_n_walks++;
	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (false);
	if (fs_apfs_read_block(bno, node) != FS_APFS_E_OK) {
		kfree(node);
		return (false);
	}
	g_n_nodes++;
	btree_layout(node, &bl);

	ok = true;
	for (i = 0; i < bl.bl_nkeys && !*stopped; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		if ((bl.bl_flags & APFS_BTNODE_LEAF) != 0) {
			const uint8_t	*k;
			uint64_t	 raw;

			g_n_recs++;
			k = bl.bl_keys + koff;
			raw = *(const uint64_t *)k;
			if (!fn(raw & APFS_J_OBJ_ID_MASK,
			    (uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT),
			    k, klen, bl.bl_vals - voff, vlen, bno, arg))
				*stopped = true;
			continue;
		}
		child_oid = *(const uint64_t *)(bl.bl_vals - voff);
		if (fs_apfs_omap_lookup(g_apfs.ac_vol_omap_tree, child_oid,
		    view_xid(), &child_bno) != FS_APFS_E_OK) {
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
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	const struct apfs_drec_val	*dv;
	struct dirent_search		*ds;
	const char			*name;
	uint32_t			 nlen;
	size_t				 i;

	(void)bno;
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
	uint64_t	ii_alloced;
	uint64_t	ii_mtime;	/* ns since the Unix epoch */
	uint64_t	ii_atime;
	uint64_t	ii_ctime;
	uint64_t	ii_btime;
	uint32_t	ii_nlink;
	uint32_t	ii_uid;
	uint32_t	ii_gid;
	uint16_t	ii_mode;
	bool		ii_found;
};

static bool
inode_pick(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
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
	(void)bno;
	ii = arg;
	if (type != APFS_TYPE_INODE || oid != ii->ii_oid)
		return (true);
	if (vlen < sizeof(*iv))
		return (true);

	iv = (const struct apfs_inode_val *)val;
	ii->ii_private_id = iv->ai_private_id;
	ii->ii_mode       = iv->ai_mode;
	ii->ii_uid        = iv->ai_owner;
	ii->ii_gid        = iv->ai_group;
	ii->ii_mtime      = iv->ai_mod_time;
	ii->ii_atime      = iv->ai_access_time;
	ii->ii_ctime      = iv->ai_change_time;
	ii->ii_btime      = iv->ai_create_time;
	/*
	 * One field, two meanings, told apart by the mode: for a directory
	 * APFS counts CHILDREN here, for anything else it counts links.  A
	 * directory's link count in POSIX terms is 2 plus its subdirectories
	 * -- itself, its "." and each child's ".." -- but APFS has no such
	 * entries to count, so reporting the honest 1 beats inventing a
	 * number no reader of this volume can verify.
	 */
	ii->ii_nlink = (iv->ai_mode & APFS_S_IFMT) == APFS_S_IFDIR ? 1u :
	    (iv->ai_nchildren_or_nlink > 0 ?
	    (uint32_t)iv->ai_nchildren_or_nlink : 1u);
	ii->ii_found = true;

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
			ii->ii_size    = ds->ds_size;
			ii->ii_alloced = ds->ds_alloced_size;
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
	ii->ii_alloced    = 0;
	ii->ii_mtime      = 0;
	ii->ii_atime      = 0;
	ii->ii_ctime      = 0;
	ii->ii_btime      = 0;
	ii->ii_nlink      = 1;
	ii->ii_uid        = 0;
	ii->ii_gid        = 0;
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

	out->afs_size    = is_dir ? 0 : ii.ii_size;
	out->afs_ino     = oid;
	out->afs_alloced = is_dir ? 0 : ii.ii_alloced;
	out->afs_mtime_ns = ii.ii_mtime;
	out->afs_atime_ns = ii.ii_atime;
	out->afs_ctime_ns = ii.ii_ctime;
	out->afs_btime_ns = ii.ii_btime;
	out->afs_nlink   = ii.ii_nlink;
	out->afs_uid     = ii.ii_uid;
	out->afs_gid     = ii.ii_gid;
	out->afs_mode    = ii.ii_mode;
	out->afs_is_dir  = is_dir ? 1 : 0;
	return (FS_APFS_E_OK);
}

/*
 * Copying part of a file's extents into a buffer.  The wanted byte window is
 * [er_lo, er_hi) of the file and er_buf holds er_lo; a whole-file read is just
 * the window [0, size).  er_bounce holds the one partial block a window whose
 * edges do not land on block boundaries needs -- allocated once by the caller
 * rather than per extent.
 */
struct extent_read {
	uint64_t	 er_id;		/* the dstream this belongs to */
	uint8_t		*er_buf;	/* holds file byte er_lo       */
	uint8_t		*er_bounce;
	uint64_t	 er_size;	/* end of the file's content   */
	uint64_t	 er_lo;		/* window start, in file bytes */
	uint64_t	 er_hi;		/* window end,   in file bytes */
	uint64_t	 er_got;
	int		 er_rv;
};

static bool
extent_copy(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	const struct apfs_file_extent_val	*fe;
	struct extent_read			*er;
	uint64_t				 logical;
	uint64_t				 len;
	uint64_t				 phys;
	uint64_t				 off;
	uint64_t				 dst;
	uint64_t				 lo;
	uint64_t				 hi;
	uint64_t				 n;

	(void)bno;
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
	/*
	 * Past the window.  Records sort by (oid, logical), so once one of
	 * this file's extents starts beyond what was asked for, no later
	 * record can be wanted either -- stop the walk rather than read the
	 * rest of the tree for nothing.  For a whole-file read the window is
	 * the whole file and this never fires.
	 */
	if (logical >= er->er_hi)
		return (false);
	if (phys == 0)
		return (true);		/* a hole: the buffer is already zero */

	for (off = 0; off < len; off += APFS_BLOCK_SIZE) {
		dst = logical + off;		/* file offset of this block */
		/*
		 * An extent is an ALLOCATED run and can reach past the end of
		 * the file; the tail of its last block is not ours to copy.
		 */
		if (dst >= er->er_size || dst >= er->er_hi)
			break;
		if (dst + APFS_BLOCK_SIZE <= er->er_lo)
			continue;		/* entirely before the window */

		/* The slice of this block that is both real content and wanted. */
		lo = (dst < er->er_lo) ? er->er_lo : dst;
		hi = dst + APFS_BLOCK_SIZE;
		if (hi > er->er_size)
			hi = er->er_size;
		if (hi > er->er_hi)
			hi = er->er_hi;
		if (lo >= hi)
			continue;
		n = hi - lo;

		if (n == APFS_BLOCK_SIZE) {
			if (read_block_raw(phys + off / APFS_BLOCK_SIZE,
			    er->er_buf + (lo - er->er_lo)) != FS_APFS_E_OK) {
				er->er_rv = FS_APFS_E_IO;
				return (false);
			}
		} else {
			if (read_block_raw(phys + off / APFS_BLOCK_SIZE,
			    er->er_bounce) != FS_APFS_E_OK) {
				er->er_rv = FS_APFS_E_IO;
				return (false);
			}
			mem_copy(er->er_buf + (lo - er->er_lo),
			    er->er_bounce + (lo - dst), (size_t)n);
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
	er.er_lo     = 0;
	er.er_hi     = st.afs_size;
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

/*
 * Resolve a path to the two things a ranged read actually needs: the dstream
 * id its extents are keyed on, and how many of its bytes are real.
 *
 * This is the expensive half of reading, and splitting it out is the point:
 * one walk per path component plus one for the inode, paid once, instead of
 * once per 4 KiB the pager asks for.
 */
int
fs_apfs_open(const char *path, uint64_t *id_out, uint64_t *size_out,
    uint64_t *ino_out)
{
	struct inode_info	ii;
	uint64_t		oid;
	int			is_dir;
	int			rv;

	if (path == NULL || id_out == NULL || size_out == NULL ||
	    ino_out == NULL)
		return (FS_APFS_E_IO);
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

	*id_out   = ii.ii_private_id;
	*size_out = ii.ii_size;
	*ino_out  = oid;
	return (FS_APFS_E_OK);
}

/*
 * The ranged read behind fs_pread.  Same extent walk as the slurp above,
 * pointed at a window instead of at the whole file: the caller's buffer holds
 * file byte `off`, and only the blocks overlapping [off, off+len) are read.
 * One tree walk, no path resolution -- that was done once, by fs_apfs_open.
 *
 * The buffer is zeroed first for the same reason the slurp zeroes its own --
 * a hole has no block to read, so its bytes have to be put there by hand --
 * and that also covers a window that reaches past end-of-file.
 */
int
fs_apfs_pread(uint64_t id, uint64_t size, uint64_t off, uint8_t *buf,
    uint32_t len, uint32_t *out_got)
{
	struct extent_read	 er;
	uint8_t			*bounce;
	uint64_t		 hi;
	bool			 stopped;

	if (buf == NULL || out_got == NULL)
		return (FS_APFS_E_IO);
	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);

	*out_got = 0;
	if (len == 0)
		return (FS_APFS_E_OK);

	if (off >= size)		/* at or past EOF: zero bytes, no error */
		return (FS_APFS_E_OK);
	hi = off + (uint64_t)len;
	if (hi > size)
		hi = size;

	mem_zero(buf, (size_t)(hi - off));

	bounce = kmalloc(APFS_BLOCK_SIZE);
	if (bounce == NULL)
		return (FS_APFS_E_NOMEM);

	er.er_id     = id;
	er.er_buf    = buf;
	er.er_bounce = bounce;
	er.er_size   = size;
	er.er_lo     = off;
	er.er_hi     = hi;
	er.er_got    = 0;
	er.er_rv     = FS_APFS_E_OK;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, extent_copy, &er, 0, &stopped))
		er.er_rv = FS_APFS_E_IO;
	kfree(bounce);

	if (er.er_rv != FS_APFS_E_OK)
		return (er.er_rv);

	/*
	 * The window's whole length is delivered, not er_got: the bytes a hole
	 * contributes are real zeroes that no block was read for.
	 */
	*out_got = (uint32_t)(hi - off);
	return (FS_APFS_E_OK);
}

/* ---- writing -------------------------------------------------------------- */

/*
 * WRITING A FILE'S BYTES, WHICH MEANS MOVING THEM
 *
 * Until this rung a write went straight onto the block the extent record
 * named, and the checkpoint machinery around it was half a promise.  The ring
 * of old superblocks was intact -- copy-on-write, the spine and the free
 * queues saw to that -- and every one of them pointed at a file whose contents
 * had since been overwritten underneath it.  Measured on the container rather
 * than argued about; the leaf describing /var/db/big.txt moves from checkpoint
 * to checkpoint and the block holding its bytes does not:
 *
 *	xid 3   leaf 98320   extent (logical 0, 155648 bytes) at block 5970
 *	xid 5   leaf 98310   extent (logical 0, 155648 bytes) at block 5970
 *	...
 *	xid 17  leaf 98337   extent (logical 0, 155648 bytes) at block 5970
 *
 *	distinct leaf blocks across the ring: 5
 *	distinct data blocks across the ring: 1
 *
 * So a write now takes fresh blocks, copies into them, and moves the record --
 * and the run it moves is THE WHOLE EXTENT, not the part written.  That is the
 * expensive choice and it is deliberate.  Writing twelve bytes into a
 * thirty-eight block extent ought to relocate one block and leave the extent
 * split in three, which is two more records than there were; a record has to
 * be inserted, a node with no room has to split, and that is the rung after
 * this one.  Moving the run whole keeps every count identical -- one file
 * extent in and one out, one physical extent in and one out -- so nothing
 * inserts, nothing splits, and no tree changes shape.  The cost is honest and
 * it is the file's size: this container's write self-test moves 152 KiB to
 * change twelve bytes.
 *
 * Two trees name those blocks and both have to be told.  The file-system tree
 * says where the file's bytes are; the extent reference tree says who owns the
 * run -- see extref_move, which is where the interesting half of that lives.
 */

/* How many extents one write may move before it is refused as unbounded. */
#define	APFS_WRITE_EXTENTS_MAX	8

/*
 * The extent record covering one file offset, and the leaf holding it.  A
 * locate pass, exactly like inode_locate below and for the same reason:
 * btree_walk frees its node buffer on the way out, so the patch has to re-read
 * the block it was told about.
 */
struct extent_locate {
	uint64_t	el_id;		/* the dstream being written   */
	uint64_t	el_want;	/* the file offset to cover    */
	uint64_t	el_logical;	/* what was found: its start   */
	uint64_t	el_len;		/* ...its length, in BYTES     */
	uint64_t	el_phys;	/* ...and its first block      */
	uint64_t	el_bno;		/* the leaf the record is in   */
	bool		el_found;
};

static bool
extent_locate(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	const struct apfs_file_extent_val	*fe;
	struct extent_locate			*el;
	uint64_t				 logical;
	uint64_t				 len;

	el = arg;
	if (type != APFS_TYPE_FILE_EXTENT || oid != el->el_id)
		return (true);
	if (klen < 16 || vlen < sizeof(*fe))
		return (true);

	logical = *(const uint64_t *)(key + 8);
	fe      = (const struct apfs_file_extent_val *)val;
	len     = fe->fe_len_and_flags & APFS_FILE_EXTENT_LEN_MASK;
	if (el->el_want < logical || el->el_want >= logical + len)
		return (true);

	el->el_logical = logical;
	el->el_len     = len;
	el->el_phys    = fe->fe_phys_block_num;
	el->el_bno     = bno;
	el->el_found   = true;
	return (false);
}

/* Where the run covering file offset `off` starts, or 0 if it is a hole. */
static int
extent_at(uint64_t id, uint64_t off, uint64_t *phys_out)
{
	struct extent_locate	el;
	bool			stopped;

	el.el_id    = id;
	el.el_want  = off;
	el.el_found = false;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, extent_locate, &el, 0,
	    &stopped))
		return (FS_APFS_E_IO);
	if (!el.el_found)
		return (FS_APFS_E_NOTFOUND);
	*phys_out = el.el_phys;
	return (FS_APFS_E_OK);
}

/*
 * Move the run this extent describes, putting the caller's bytes in as the
 * copy goes past them, and leave *pos at the first file byte beyond it.
 *
 * The copy is what makes the partial blocks at either end correct for free.
 * Every block of the old run is read whole and written whole; the window only
 * decides which of its bytes are replaced on the way.  There is no
 * read-modify-write special case because there is nothing special about it --
 * the bytes of a block that the caller did not ask about are the file's, and
 * the copy carries them across because it carries everything across.
 */
static int
extent_relocate(const struct extent_locate *el, const uint8_t *buf,
    uint64_t wlo, uint64_t whi, uint8_t *bounce, uint8_t *node, uint64_t *pos)
{
	struct apfs_file_extent_val	*fe;
	struct apfs_obj_phys		*o;
	struct btree_layout		 bl;
	const uint8_t			*k;
	uint64_t			 blocks;
	uint64_t			 first;
	uint64_t			 leaf_oid;
	uint64_t			 new_leaf;
	uint64_t			 raw;
	uint64_t			 xid;
	uint64_t			 b;
	uint64_t			 dst;
	uint64_t			 lo;
	uint64_t			 hi;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;

	blocks = (el->el_len + APFS_BLOCK_SIZE - 1) / APFS_BLOCK_SIZE;
	if (blocks == 0 || blocks > 0xFFFFFFFFULL)
		return (FS_APFS_E_INVAL);
	xid = g_apfs.ac_xid + 1;

	/*
	 * Near the run it replaces.  Not a preference: the release below has
	 * to clear bits in the old run's chunk, so a copy that lands in that
	 * same chunk is a copy this transaction can finish.
	 */
	rv = alloc_blocks((uint32_t)blocks, el->el_phys, &first);
	if (rv != FS_APFS_E_OK)
		return (rv);

	for (b = 0; b < blocks; b++) {
		rv = read_block_raw(el->el_phys + b, bounce);
		if (rv != FS_APFS_E_OK)
			goto give_back;
		dst = el->el_logical + b * APFS_BLOCK_SIZE;
		lo  = (dst > wlo) ? dst : wlo;
		hi  = dst + APFS_BLOCK_SIZE;
		if (hi > whi)
			hi = whi;
		if (lo < hi)
			mem_copy(bounce + (lo - dst), buf + (lo - wlo),
			    (size_t)(hi - lo));
		rv = write_block_raw(first + b, bounce);
		if (rv != FS_APFS_E_OK)
			goto give_back;
	}

	/*
	 * The record, found again inside our own copy of the leaf with the
	 * same layout code the reader uses.  Keyed by (object, offset in the
	 * file), neither of which this changes -- only the block number in the
	 * value does, so the record stays exactly where it is in the node.
	 */
	rv = fs_apfs_read_block(el->el_bno, node);
	if (rv != FS_APFS_E_OK)
		goto give_back;
	btree_layout(node, &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		if (klen < 16 || vlen < sizeof(*fe))
			continue;
		k   = bl.bl_keys + koff;
		raw = *(const uint64_t *)k;
		if ((raw & APFS_J_OBJ_ID_MASK) != el->el_id)
			continue;
		if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) !=
		    APFS_TYPE_FILE_EXTENT)
			continue;
		if (*(const uint64_t *)(k + 8) != el->el_logical)
			continue;
		fe = (struct apfs_file_extent_val *)(bl.bl_vals - voff);
		fe->fe_phys_block_num = first;
		rv = FS_APFS_E_OK;
		break;
	}
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: the extent at file offset %llu vanished from "
		    "leaf %llu between finding it and patching it\n",
		    (unsigned long long)el->el_logical,
		    (unsigned long long)el->el_bno);
		goto give_back;
	}

	/* The leaf is virtual: it keeps its oid and only its address moves. */
	o        = (struct apfs_obj_phys *)node;
	leaf_oid = o->o_oid;
	rv = alloc_blocks(1, el->el_bno, &new_leaf);
	if (rv != FS_APFS_E_OK)
		goto give_back;
	o->o_xid = xid;
	rv = fs_apfs_write_block(new_leaf, node);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(new_leaf, 1);
		goto give_back;
	}
	rv = free_blocks(el->el_bno, 1);
	if (rv != FS_APFS_E_OK)
		goto give_back;
	cow_n_spine++;

	/*
	 * Past this point nothing can be given back quietly: the leaf has
	 * moved, and a failure leaves a transaction that must not be written.
	 * Both remaining steps say so and refuse the checkpoint rather than
	 * publish half of one.
	 */
	rv = extref_move(el->el_phys, first, blocks, xid, node);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: the file's bytes moved to %llu but the extent "
		    "reference tree did not follow (%d) -- this checkpoint "
		    "must not be written\n", (unsigned long long)first, rv);
		return (rv);
	}
	rv = spine_update(leaf_oid, new_leaf, xid, node);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: the extent leaf moved to %llu but the spine did "
		    "not follow (%d) -- this checkpoint must not be written\n",
		    (unsigned long long)new_leaf, rv);
		return (rv);
	}
	if (leaf_oid == g_apfs.ac_root_tree_oid)
		g_apfs.ac_root_tree_bno = new_leaf;

	rv = free_blocks(el->el_phys, (uint32_t)blocks);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: the old run at %llu (%llu blocks) could not be "
		    "released (%d)\n", (unsigned long long)el->el_phys,
		    (unsigned long long)blocks, rv);
		return (rv);
	}
	cow_n_data += blocks;
	*pos = el->el_logical + el->el_len;
	return (FS_APFS_E_OK);

give_back:
	(void)free_blocks(first, (uint32_t)blocks);
	return (rv);
}

int
fs_apfs_pwrite(uint64_t id, uint64_t size, uint64_t off, const uint8_t *buf,
    uint32_t len, uint32_t *out_put)
{
	struct extent_locate	 el;
	uint8_t			*bounce;
	uint8_t			*node;
	uint64_t		 pos;
	uint64_t		 end;
	uint32_t		 moved;
	int			 rv;
	bool			 stopped;

	if (buf == NULL || out_put == NULL)
		return (FS_APFS_E_IO);
	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	*out_put = 0;
	if (len == 0)
		return (FS_APFS_E_OK);

	/*
	 * Growth needs a record inserted, not just moved; refuse the whole
	 * write rather than do the prefix that happens to fit.  A short write
	 * that reports success is how a file ends up half-updated with nobody
	 * told.
	 */
	if (off >= size || off + (uint64_t)len > size)
		return (FS_APFS_E_NOALLOC);

	bounce = kmalloc(APFS_BLOCK_SIZE);
	node   = kmalloc(APFS_BLOCK_SIZE);
	if (bounce == NULL || node == NULL) {
		kfree(bounce);
		kfree(node);
		return (FS_APFS_E_NOMEM);
	}

	pos = off;
	end = off + (uint64_t)len;
	rv  = FS_APFS_E_OK;
	for (moved = 0; pos < end; moved++) {
		if (moved >= APFS_WRITE_EXTENTS_MAX) {
			kprintf("apfs: a write of %u bytes at %llu spans more "
			    "than %u extents -- refused\n", (unsigned)len,
			    (unsigned long long)off,
			    (unsigned)APFS_WRITE_EXTENTS_MAX);
			rv = FS_APFS_E_NOALLOC;
			goto out;
		}
		el.el_id    = id;
		el.el_want  = pos;
		el.el_found = false;
		stopped = false;
		if (!btree_walk(g_apfs.ac_root_tree_bno, extent_locate, &el, 0,
		    &stopped)) {
			rv = FS_APFS_E_IO;
			goto out;
		}
		/*
		 * Coverage, checked rather than assumed.  A range no extent
		 * record describes is not an error the walk reports -- it
		 * simply never mentions those bytes -- so an unbacked file
		 * would otherwise come back as a flawless write of nothing.
		 * Silence is not success.
		 */
		if (!el.el_found) {
			rv = FS_APFS_E_NOALLOC;
			goto out;
		}
		/*
		 * A hole overlapping the write.  Reading one costs nothing
		 * because its bytes are defined to be zero; writing one means
		 * finding it a run and giving the file a record it does not
		 * have, which is the insert this rung does not do.
		 */
		if (el.el_phys == 0) {
			rv = FS_APFS_E_NOALLOC;
			goto out;
		}
		rv = extent_relocate(&el, buf, off, end, bounce, node, &pos);
		if (rv != FS_APFS_E_OK)
			goto out;
	}

	*out_put = len;
out:
	kfree(bounce);
	kfree(node);
	return (rv);
}

/*
 * Where an inode record lives.  The locate pass records the block and stops;
 * the patch re-reads it.  Splitting it that way keeps btree_walk read-only --
 * it frees its node buffer on the way out, so anything written into that
 * buffer would be discarded, and a walker that both reads and writes is a
 * walker whose callbacks have to know which they are.
 */
struct inode_locate {
	uint64_t	il_oid;
	uint64_t	il_bno;
	bool		il_found;
};

static bool
inode_locate(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct inode_locate	*il;

	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	il = arg;
	if (type != APFS_TYPE_INODE || oid != il->il_oid)
		return (true);
	il->il_bno   = bno;
	il->il_found = true;
	return (false);
}

int
fs_apfs_touch(uint64_t oid, uint64_t mtime_ns)
{
	struct btree_layout	 bl;
	struct inode_locate	 il;
	struct apfs_inode_val	*iv;
	struct apfs_obj_phys	*o;
	uint8_t			*node;
	const uint8_t		*k;
	uint64_t		 leaf_oid;
	uint64_t		 new_bno;
	uint64_t		 raw;
	uint64_t		 xid;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 i;
	int			 rv;
	bool			 stopped;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	il.il_oid   = oid;
	il.il_bno   = 0;
	il.il_found = false;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!il.il_found)
		return (FS_APFS_E_NOTFOUND);

	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (FS_APFS_E_NOMEM);
	rv = fs_apfs_read_block(il.il_bno, node);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * Find the record again inside our own copy, using the same layout
	 * code the reader uses.  Re-deriving the offset rather than carrying
	 * one out of the walk is what keeps writer and reader from ever
	 * disagreeing about where a value begins -- and the root-node case,
	 * where 40 bytes of btree_info shift the value base, is exactly the
	 * kind of detail two copies of that arithmetic would drift on.
	 */
	btree_layout(node, &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		k = bl.bl_keys + koff;
		raw = *(const uint64_t *)k;
		if ((raw & APFS_J_OBJ_ID_MASK) != oid)
			continue;
		if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_INODE)
			continue;
		if (vlen < sizeof(*iv)) {
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		iv = (struct apfs_inode_val *)(bl.bl_vals - voff);
		iv->ai_mod_time    = mtime_ns;
		iv->ai_change_time = mtime_ns;
		rv = FS_APFS_E_OK;
		break;
	}
	if (rv != FS_APFS_E_OK)
		goto out;			/* nothing has been written */

	/*
	 * COPY-ON-WRITE, and the node is VIRTUAL: it keeps the oid it had,
	 * because that oid is the name the object map answers.  Only its
	 * address changes, and making that address the answer is what
	 * spine_update does.
	 */
	o        = (struct apfs_obj_phys *)node;
	leaf_oid = o->o_oid;
	xid      = g_apfs.ac_xid + 1;

	rv = alloc_blocks(1, 0, &new_bno);
	if (rv != FS_APFS_E_OK)
		goto out;
	o->o_xid = xid;
	rv = fs_apfs_write_block(new_bno, node);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(new_bno, 1);
		goto out;
	}
	rv = free_blocks(il.il_bno, 1);
	if (rv != FS_APFS_E_OK)
		goto out;
	cow_n_spine++;

	rv = spine_update(leaf_oid, new_bno, xid, node);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: the inode moved to %llu but the spine did not "
		    "follow (%d) -- this checkpoint must not be written\n",
		    (unsigned long long)new_bno, rv);
		goto out;
	}
	/*
	 * If what moved WAS the tree root, the reader's shortcut to it is now
	 * a stale address.  It is not, in this container -- the root is an
	 * index node and inodes live in leaves -- but a container with one
	 * node of file-system tree would take this branch on its first write.
	 */
	if (leaf_oid == g_apfs.ac_root_tree_oid)
		g_apfs.ac_root_tree_bno = new_bno;
	rv = FS_APFS_E_OK;
out:
	kfree(node);
	return (rv);
}

/* ---- growing -------------------------------------------------------------- */

/*
 * A FILE GETS LONGER
 *
 * Every write until now replaced a record; this one ADDS one, which is the
 * first thing in this file that changes a tree's shape.  Three records have to
 * agree afterwards and they live in two different trees:
 *
 *	the file extent    (object, offset in the file) -> run
 *	the physical extent          (first block)      -> length, owner, count
 *	the inode's dstream                             -> length, allocated
 *
 * Room was measured before any of it was designed.  The leaf holding this
 * container's file extents has 193 bytes of free span and six spare entries in
 * its table of contents, against 48 bytes and one entry for a file extent
 * record -- so four inserts fit and the fifth does not.  That bound is real and
 * it is reported rather than worked around: a node with no room has to SPLIT,
 * a split changes the index above it, and that is the next rung.  Refusing out
 * loud leaves a filesystem that still says the truth about itself.
 *
 * The table of contents is the reason a node has any bound at all.  It is a
 * fixed reservation at the front, and the key area begins where it ends, so
 * growing it would move every key under every offset already recorded.
 */

/*
 * Order two file-system tree keys: negative, zero, positive.
 *
 * Records sort by object id FIRST and type second, which is the opposite of
 * what comparing the raw first word would do -- the type is in the top bits.
 * Beyond that the order is per type; the only one this needs is the file
 * extent, whose remaining key is its offset within the file.  Two keys with
 * the same object and type and no rule to separate them compare equal, which
 * is honest: this returns an order, not a total order over records it has
 * never been asked about.
 */
static int
jkey_cmp(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
	uint64_t	ra, rb;
	uint64_t	ida, idb;
	uint64_t	la, lb;
	uint32_t	ta, tb;

	if (alen < 8 || blen < 8)
		return (0);
	ra  = *(const uint64_t *)a;
	rb  = *(const uint64_t *)b;
	ida = ra & APFS_J_OBJ_ID_MASK;
	idb = rb & APFS_J_OBJ_ID_MASK;
	if (ida != idb)
		return (ida < idb ? -1 : 1);
	ta = (uint32_t)(ra >> APFS_J_OBJ_TYPE_SHIFT);
	tb = (uint32_t)(rb >> APFS_J_OBJ_TYPE_SHIFT);
	if (ta != tb)
		return (ta < tb ? -1 : 1);
	if (ta == APFS_TYPE_FILE_EXTENT && alen >= 16 && blen >= 16) {
		la = *(const uint64_t *)(a + 8);
		lb = *(const uint64_t *)(b + 8);
		if (la != lb)
			return (la < lb ? -1 : 1);
		return (0);
	}
	/*
	 * Directory entries, because a node's separator can be one and getting
	 * two of them the wrong way round would put a record in the wrong half
	 * of a split.  Hashed volumes sort by the word holding the name's
	 * length and hash and then by the name; plain ones by the name alone.
	 * Which of the two this volume is was settled at mount.
	 */
	if (ta == APFS_TYPE_DIR_REC) {
		uint32_t	ha, hb;
		uint32_t	off;
		uint32_t	n;
		uint32_t	i;

		off = g_apfs.ac_drec_hashed ? 12u : 10u;
		if (alen < off || blen < off)
			return (0);
		if (g_apfs.ac_drec_hashed) {
			ha = *(const uint32_t *)(a + 8);
			hb = *(const uint32_t *)(b + 8);
			if (ha != hb)
				return (ha < hb ? -1 : 1);
		}
		n = (alen - off < blen - off) ? alen - off : blen - off;
		for (i = 0; i < n; i++) {
			if (a[off + i] != b[off + i])
				return (a[off + i] < b[off + i] ? -1 : 1);
		}
		if (alen != blen)
			return (alen < blen ? -1 : 1);
	}
	return (0);
}

/*
 * A HOLE IS ROOM, AND UNTIL NOW IT WAS NOT
 *
 * Deleting a record threads its bytes onto one of the node's free-list chains;
 * inserting one took only from the free span.  The span is a one-way ratchet,
 * so a node that has records come and go loses a record's worth of room per
 * cycle and eventually refuses an insert while claiming plenty of free bytes.
 *
 * That is exactly the bug the free queue had, and the free queue could be fixed
 * the easy way: every hole in it is the same size, so taking one is popping the
 * head of a list.  Here they are not.  A directory entry, an inode record and
 * an extent record are three different lengths, and the chain has to be
 * searched rather than popped.
 *
 * `step` is +1 for the key area, whose offsets grow with the address, and -1
 * for the value area, whose offsets are measured backwards from the end of the
 * node.  That sign is the whole of the difference between the two.
 *
 * A hole is usable only if it fits exactly or leaves at least a link behind:
 * a remainder too small to hold its own nloc could not stay on the chain, and
 * quietly dropping it would leave bytes belonging to nothing -- which is
 * precisely the accounting apfsck recomputes.  Every byte of this node is
 * either inside a record or on a chain, before and after.
 */
static uint32_t
hole_find(const struct apfs_nloc *head, const uint8_t *base, int step,
    uint32_t need)
{
	const struct apfs_nloc	*hole;
	uint32_t		 off;

	off = head->nl_off;
	while (off != APFS_BTOFF_INVALID) {
		hole = (const struct apfs_nloc *)(base + step * (int)off);
		if (hole->nl_len == need ||
		    hole->nl_len >= need + (uint32_t)sizeof(*hole))
			return (off);
		off = hole->nl_off;
	}
	return (APFS_BTOFF_INVALID);
}

/*
 * Take `need` bytes out of the hole at `at`, which hole_find has already said
 * will do, and report where they are.
 *
 * The bytes come off the FAR end.  A hole's own nloc sits at its first byte and
 * whoever points at the hole names that byte, so shrinking one from the far end
 * is an edit inside it with nothing above to tell; only a hole consumed exactly
 * has to be unlinked, and the link that names it is either the previous hole's
 * or the head in the node's header, which are the same four bytes in two
 * places.
 */
static uint32_t
hole_take(struct apfs_nloc *head, uint8_t *base, int step, uint32_t need,
    uint32_t at)
{
	struct apfs_nloc	*prev;
	struct apfs_nloc	*hole;
	uint32_t		 off;
	uint32_t		 own;

	prev = head;
	off  = head->nl_off;
	while (off != at) {
		prev = (struct apfs_nloc *)(base + step * (int)off);
		off  = prev->nl_off;
	}
	hole = (struct apfs_nloc *)(base + step * (int)at);
	own  = hole->nl_len;
	if (own == need)
		prev->nl_off = hole->nl_off;
	else
		hole->nl_len = (uint16_t)(own - need);
	head->nl_len = (uint16_t)(head->nl_len - need);
	return ((uint32_t)((int)at + step * (int)(own - need)));
}

/*
 * Put a record into a variable-KV node, at the position the caller worked out,
 * and refuse if there is no room.
 *
 * Keys grow up from the start of the key area and values grow down from the
 * end of the node; the free span in the middle is what both eat into, and the
 * node header carries where it starts and how much is left.  Either end may
 * instead come out of a hole a delete left behind, and the two are independent:
 * a key can be threaded into a hole while its value takes from the span.
 */
static int
leaf_insert(uint8_t *node, uint32_t pos, const void *key, uint32_t klen,
    const void *val, uint32_t vlen)
{
	struct apfs_btree_node_phys	*n;
	struct btree_layout		 bl;
	struct apfs_kvloc		*kv;
	uint32_t			 khole;
	uint32_t			 vhole;
	uint32_t			 koff;
	uint32_t			 voff;
	uint32_t			 vbase;
	uint32_t			 need;
	uint32_t			 i;

	n = (struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	if (bl.bl_fixed || pos > bl.bl_nkeys)
		return (FS_APFS_E_INVAL);
	if ((uint32_t)(bl.bl_nkeys + 1) * (uint32_t)sizeof(struct apfs_kvloc) >
	    n->btn_table_space.nl_len) {
		kprintf("apfs: the node at level %u has %u entries and no room "
		    "in its table of contents -- a split is a different rung\n",
		    (unsigned)bl.bl_level, (unsigned)bl.bl_nkeys);
		return (FS_APFS_E_NOALLOC);
	}

	/*
	 * Both ends are placed before either is written, because a record that
	 * fitted its key and then found nowhere for its value would have to put
	 * the key back -- and the room question has to be answerable without
	 * changing anything, which is what the split path asks it for.
	 */
	khole = hole_find(&n->btn_key_free_list, bl.bl_keys, 1, klen);
	vhole = hole_find(&n->btn_val_free_list, bl.bl_vals, -1, vlen);
	need  = (khole == APFS_BTOFF_INVALID ? klen : 0) +
	    (vhole == APFS_BTOFF_INVALID ? vlen : 0);
	if (need > n->btn_free_space.nl_len) {
		kprintf("apfs: the node at level %u has %u bytes free and the "
		    "record needs %u -- a split is a different rung\n",
		    (unsigned)bl.bl_level, (unsigned)n->btn_free_space.nl_len,
		    (unsigned)need);
		return (FS_APFS_E_NOALLOC);
	}

	if (khole != APFS_BTOFF_INVALID) {
		koff = hole_take(&n->btn_key_free_list, (uint8_t *)bl.bl_keys,
		    1, klen, khole);
		hole_n++;
	} else {
		koff = n->btn_free_space.nl_off;
		n->btn_free_space.nl_off = (uint16_t)(koff + klen);
		n->btn_free_space.nl_len =
		    (uint16_t)(n->btn_free_space.nl_len - klen);
	}
	mem_copy((uint8_t *)bl.bl_keys + koff, key, klen);

	if (vhole != APFS_BTOFF_INVALID) {
		voff = hole_take(&n->btn_val_free_list, (uint8_t *)bl.bl_vals,
		    -1, vlen, vhole);
		hole_n++;
	} else {
		/*
		 * The value's offset is measured BACKWARDS from the end of the
		 * node, so it is whatever is left of the free span after this
		 * value is taken off the bottom of it.
		 */
		n->btn_free_space.nl_len =
		    (uint16_t)(n->btn_free_space.nl_len - vlen);
		vbase = APFS_BLOCK_SIZE -
		    (((n->btn_flags & APFS_BTNODE_ROOT) != 0) ?
		    APFS_BTREE_INFO_SIZE : 0);
		voff = vbase - (APFS_BTNODE_HDR_SIZE +
		    n->btn_table_space.nl_off + n->btn_table_space.nl_len +
		    n->btn_free_space.nl_off + n->btn_free_space.nl_len);
	}
	mem_copy((uint8_t *)bl.bl_vals - voff, val, vlen);

	kv = (struct apfs_kvloc *)(node + APFS_BTNODE_HDR_SIZE +
	    n->btn_table_space.nl_off);
	for (i = bl.bl_nkeys; i > pos; i--)
		kv[i] = kv[i - 1];
	kv[pos].k.nl_off = (uint16_t)koff;
	kv[pos].k.nl_len = (uint16_t)klen;
	kv[pos].v.nl_off = (uint16_t)voff;
	kv[pos].v.nl_len = (uint16_t)vlen;
	n->btn_nkeys++;
	return (FS_APFS_E_OK);
}

/*
 * Take a record OUT of a variable-KV node.
 *
 * Its bytes do not go back to the free span.  That span is one stretch in the
 * middle of the node, with the keys growing up into it and the values growing
 * down; a record removed from anywhere but the very edge leaves a hole that is
 * not next to it.  What the format keeps instead is a CHAIN -- the hole holds
 * an nloc naming the next hole and its own length, and the node header holds
 * the head of the chain and the total it comes to.
 *
 * That total is checked, which is how this was settled before it was written:
 * deleting a record on a copy of the image without threading its bytes onto
 * the chain makes apfsck answer "B-tree: wrong free space total for key area",
 * and threading them on makes it silent.  The value side is the same chain
 * measured backwards from the end of the node, exactly as the values are.
 *
 * The room this returns IS room a later insert can use -- see hole_find above.
 * It was not, once, and that was survivable only while the one thing that could
 * delete was a truncate: a file that is made shorter and never longer again
 * gives its bytes back once.  A name that comes and goes is a cycle, and a node
 * that loses a record's worth of room per cycle stops working after fifteen of
 * them.
 */
static int
leaf_delete(uint8_t *node, uint32_t pos)
{
	struct apfs_btree_node_phys	*n;
	struct btree_layout		 bl;
	struct apfs_kvloc		*kv;
	struct apfs_nloc		*hole;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;

	n = (struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	if (bl.bl_fixed || pos >= bl.bl_nkeys)
		return (FS_APFS_E_INVAL);
	btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);

	/*
	 * A hole has to be big enough to say where the next one is.  Nothing
	 * this kernel deletes is that small -- its shortest key is eight bytes
	 * and its shortest value four -- but a hole that cannot hold its own
	 * link would corrupt the chain instead of extending it.
	 */
	if (klen < sizeof(*hole) || vlen < sizeof(*hole)) {
		kprintf("apfs: a %u-byte key and %u-byte value cannot be put "
		    "on a free list whose links are %u bytes\n",
		    (unsigned)klen, (unsigned)vlen, (unsigned)sizeof(*hole));
		return (FS_APFS_E_INVAL);
	}

	hole = (struct apfs_nloc *)((uint8_t *)bl.bl_keys + koff);
	hole->nl_off = n->btn_key_free_list.nl_off;
	hole->nl_len = (uint16_t)klen;
	n->btn_key_free_list.nl_off = (uint16_t)koff;
	n->btn_key_free_list.nl_len =
	    (uint16_t)(n->btn_key_free_list.nl_len + klen);

	hole = (struct apfs_nloc *)((uint8_t *)bl.bl_vals - voff);
	hole->nl_off = n->btn_val_free_list.nl_off;
	hole->nl_len = (uint16_t)vlen;
	n->btn_val_free_list.nl_off = (uint16_t)voff;
	n->btn_val_free_list.nl_len =
	    (uint16_t)(n->btn_val_free_list.nl_len + vlen);

	kv = (struct apfs_kvloc *)(node + APFS_BTNODE_HDR_SIZE +
	    n->btn_table_space.nl_off);
	for (i = pos; i + 1 < bl.bl_nkeys; i++)
		kv[i] = kv[i + 1];
	n->btn_nkeys--;
	return (FS_APFS_E_OK);
}

/*
 * How many records a tree holds is written ONCE, in the btree_info at the end
 * of its root node -- not in the leaf the record went into.  So an insert into
 * a leaf moves two nodes, and the root is the second.
 */
static void
tree_count_add(uint8_t *root, int64_t delta)
{
	uint64_t	*count;

	count = (uint64_t *)(root + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE +
	    APFS_BTREE_INFO_KEYCOUNT);
	*count = (uint64_t)((int64_t)*count + delta);
}

/*
 * A newly allocated run belongs to somebody: say so in the extent reference
 * tree.  The tree is a single node that is its own root, so its record count
 * is in the node being edited and there is nothing above it to tell.
 */
static int
extref_insert(uint64_t start, uint64_t blocks, uint64_t owner, uint64_t xid,
    void *buf)
{
	struct apfs_phys_ext_val	 pv;
	struct btree_layout		 bl;
	uint8_t				*node;
	uint64_t			 key;
	uint64_t			 raw;
	uint64_t			 new_bno;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 pos;
	int				 rv;

	rv = fs_apfs_read_block(g_apfs.ac_extref_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	node = buf;
	btree_layout(node, &bl);
	if (bl.bl_level != 0 || bl.bl_fixed)
		return (FS_APFS_E_INVAL);

	key = start | ((uint64_t)APFS_TYPE_EXTENT << APFS_J_OBJ_TYPE_SHIFT);
	for (pos = 0; pos < bl.bl_nkeys; pos++) {
		btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw & APFS_J_OBJ_ID_MASK) > start)
			break;
	}

	pv.pe_len_and_kind = blocks |
	    ((uint64_t)APFS_PEXT_KIND_NEW << APFS_PEXT_KIND_SHIFT);
	pv.pe_owning_obj_id = owner;
	pv.pe_refcnt        = 1;
	rv = leaf_insert(node, pos, &key, (uint32_t)sizeof(key), &pv,
	    (uint32_t)sizeof(pv));
	if (rv != FS_APFS_E_OK)
		return (rv);
	tree_count_add(node, 1);

	rv = cow_physical(g_apfs.ac_extref_bno, xid, buf, &new_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	g_apfs.ac_extref_bno = new_bno;
	return (FS_APFS_E_OK);
}

/*
 * A run has grown at its end: say so in the extent reference tree, instead of
 * giving it a second record for blocks that touch the first.
 *
 * This is what keeps appending from being quadratic in records.  Its node is
 * its own root and holds sixteen entries, so four appends filled it -- the
 * fifth said "no room in its table of contents", which is true and is not the
 * problem.  Two runs that touch, with one owner between them, ARE one run; the
 * format says so by giving the record a length, and writing two records for
 * them is the thing that should never have been asked of the tree.
 */
static int
extref_extend(uint64_t start, uint64_t extra, uint64_t xid, void *buf)
{
	struct apfs_phys_ext_val	*pv;
	struct btree_layout		 bl;
	uint8_t				*node;
	uint64_t			 raw;
	uint64_t			 new_bno;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;

	rv = fs_apfs_read_block(g_apfs.ac_extref_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	node = buf;
	btree_layout(node, &bl);
	if (bl.bl_level != 0 || bl.bl_fixed)
		return (FS_APFS_E_INVAL);

	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_EXTENT)
			continue;
		if ((raw & APFS_J_OBJ_ID_MASK) != start)
			continue;
		if (vlen < sizeof(*pv))
			return (FS_APFS_E_INVAL);
		pv = (struct apfs_phys_ext_val *)(bl.bl_vals - voff);
		pv->pe_len_and_kind += extra;
		rv = cow_physical(g_apfs.ac_extref_bno, xid, buf, &new_bno);
		if (rv != FS_APFS_E_OK)
			return (rv);
		g_apfs.ac_extref_bno = new_bno;
		return (FS_APFS_E_OK);
	}
	return (FS_APFS_E_NOTFOUND);
}

/*
 * A run has lost blocks off its END, or lost all of them: say so in the extent
 * reference tree.
 *
 * The key is where the run STARTS, and a truncation only ever moves its end,
 * so a shortened record does not re-sort and the edit is a length in place --
 * the same shape, and the same reason, as extref_extend.
 *
 * Leaving it out is not a leak nobody notices.  apfsck recomputes what the
 * volume owns from exactly these records, and a copy of the image whose file
 * extent had been shortened but whose reference had not drew "Physical extent
 * record: bad reference count" -- measured, before this existed.
 *
 * A run somebody else also names is refused rather than quietly de-referenced.
 * Nothing in this kernel makes one yet; a clone would, and the difference
 * between shortening a run and dropping one reference to it is the whole of
 * what a clone is.
 */
static int
extref_shrink(uint64_t start, uint64_t keep, uint64_t xid, void *buf,
    bool *dropped)
{
	struct apfs_phys_ext_val	*pv;
	struct btree_layout		 bl;
	uint8_t				*node;
	uint64_t			 raw;
	uint64_t			 new_bno;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;

	*dropped = false;
	rv = fs_apfs_read_block(g_apfs.ac_extref_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	node = buf;
	btree_layout(node, &bl);
	if (bl.bl_level != 0 || bl.bl_fixed)
		return (FS_APFS_E_INVAL);

	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_EXTENT)
			continue;
		if ((raw & APFS_J_OBJ_ID_MASK) != start)
			continue;
		if (vlen < sizeof(*pv))
			return (FS_APFS_E_INVAL);
		pv = (struct apfs_phys_ext_val *)(bl.bl_vals - voff);
		if (pv->pe_refcnt != 1) {
			kprintf("apfs: the run at %llu is named %d times -- "
			    "shortening a shared run is a different rung\n",
			    (unsigned long long)start, (int)pv->pe_refcnt);
			return (FS_APFS_E_NOALLOC);
		}
		if (keep == 0) {
			rv = leaf_delete(node, i);
			if (rv != FS_APFS_E_OK)
				return (rv);
			tree_count_add(node, -1);
			*dropped = true;
		} else
			pv->pe_len_and_kind =
			    (pv->pe_len_and_kind & ~APFS_PEXT_LEN_MASK) | keep;

		rv = cow_physical(g_apfs.ac_extref_bno, xid, buf, &new_bno);
		if (rv != FS_APFS_E_OK)
			return (rv);
		g_apfs.ac_extref_bno = new_bno;
		return (FS_APFS_E_OK);
	}
	return (FS_APFS_E_NOTFOUND);
}

/*
 * Which leaf a key belongs in: the last one holding a key no greater than it.
 *
 * A walk in tree order is enough to answer that, and it is the same walk every
 * reader here already uses.  Doing it by descending the index would be faster
 * and would need a second copy of the key ordering; this one has exactly one.
 */
struct leaf_find {
	const uint8_t	*lf_key;
	uint32_t	 lf_klen;
	uint64_t	 lf_bno;
	uint64_t	 lf_first;
	bool		 lf_any;
};

static bool
leaf_find(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct leaf_find	*lf;

	(void)oid;
	(void)type;
	(void)val;
	(void)vlen;
	lf = arg;
	if (!lf->lf_any) {
		lf->lf_first = bno;
		lf->lf_any   = true;
	}
	if (jkey_cmp(key, klen, lf->lf_key, lf->lf_klen) <= 0)
		lf->lf_bno = bno;
	return (true);
}

/* ---- splitting ------------------------------------------------------------ */

/*
 * A NODE RUNS OUT OF ROOM
 *
 * Every insert so far has been able to refuse, and the refusal was honest
 * while nothing depended on it.  It cannot stay: four appends fill this
 * container's extent leaf, and a filesystem that stops working after four
 * appends is not one.
 *
 * A split is the first operation here that makes a NEW object rather than
 * moving one, and that is the part measurement had to settle.  Three counters
 * had to be found before any of it could be written, and two of them are
 * traps:
 *
 *	nx_next_oid      1126  the CONTAINER's, and the source of a fresh
 *	                       virtual object id
 *	apfs_next_obj_id   40  the VOLUME's, which numbers inodes -- a
 *	                       different namespace, and already far behind
 *	                       the tree's own nodes at 1125
 *	bt_node_count       3  in the root's footer, and one more after this;
 *	                       bt_key_count does NOT move, because the same
 *	                       records are still there
 *
 * The new half is a virtual object, so writing it does not make it reachable:
 * the volume's object map has to gain an ENTRY, which is an insert into a
 * fixed-size-KV tree, and the parent has to gain a separator, which is an
 * insert into a variable one.  Both had room, measured before the code -- the
 * object map has 109 spare entries in its table of contents and the root six.
 * A parent with none would have to split in turn and the tree would gain a
 * level; that is refused out loud, and it is the honest edge of this rung.
 */

/* Would a record of this size go into this node as it stands? */
static bool
leaf_has_room(const uint8_t *node, uint32_t klen, uint32_t vlen)
{
	const struct apfs_btree_node_phys	*n;
	struct btree_layout			 bl;
	uint32_t				 entry;
	uint32_t				 need;

	n = (const struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	entry = bl.bl_fixed ? (uint32_t)sizeof(struct apfs_kvoff) :
	    (uint32_t)sizeof(struct apfs_kvloc);
	if ((uint32_t)(bl.bl_nkeys + 1) * entry > n->btn_table_space.nl_len)
		return (false);
	need = klen + vlen;
	/*
	 * Asked exactly the way leaf_insert answers it, holes and all.  Not of
	 * a FIXED-KV node: leaf_insert_fixed takes from the span and only from
	 * there, and the two fixed trees here -- the object map and the free
	 * queues -- never reach this, so the difference is a rule stated rather
	 * than a case ever taken.
	 */
	if (!bl.bl_fixed) {
		if (hole_find(&n->btn_key_free_list, bl.bl_keys, 1, klen) !=
		    APFS_BTOFF_INVALID)
			need -= klen;
		if (hole_find(&n->btn_val_free_list, bl.bl_vals, -1, vlen) !=
		    APFS_BTOFF_INVALID)
			need -= vlen;
	}
	return (need <= n->btn_free_space.nl_len);
}

/*
 * The same insert, for a node whose keys and values are all one size.  The
 * object map is such a tree, and a split needs an entry in it -- so this and
 * leaf_insert are the same arithmetic told apart by four bytes of table entry
 * against eight, which is the whole of the difference between the two node
 * layouts this format has.
 */
static int
leaf_insert_fixed(uint8_t *node, uint32_t pos, const void *key, uint32_t klen,
    const void *val, uint32_t vlen)
{
	struct apfs_btree_node_phys	*n;
	struct btree_layout		 bl;
	struct apfs_kvoff		*kv;
	uint8_t				*keys;
	uint32_t			 koff;
	uint32_t			 voff;
	uint32_t			 vbase;
	uint32_t			 i;

	n = (struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	if (!bl.bl_fixed || pos > bl.bl_nkeys)
		return (FS_APFS_E_INVAL);
	if (!leaf_has_room(node, klen, vlen)) {
		kprintf("apfs: the object map node holds %u entries and has no "
		    "room for another -- splitting one is a different rung\n",
		    (unsigned)bl.bl_nkeys);
		return (FS_APFS_E_NOALLOC);
	}

	keys = node + APFS_BTNODE_HDR_SIZE + n->btn_table_space.nl_off +
	    n->btn_table_space.nl_len;
	koff = n->btn_free_space.nl_off;
	mem_copy(keys + koff, key, klen);
	n->btn_free_space.nl_off = (uint16_t)(koff + klen);
	n->btn_free_space.nl_len = (uint16_t)(n->btn_free_space.nl_len - klen);

	n->btn_free_space.nl_len = (uint16_t)(n->btn_free_space.nl_len - vlen);
	vbase = APFS_BLOCK_SIZE -
	    (((n->btn_flags & APFS_BTNODE_ROOT) != 0) ?
	    APFS_BTREE_INFO_SIZE : 0);
	voff = vbase - (APFS_BTNODE_HDR_SIZE + n->btn_table_space.nl_off +
	    n->btn_table_space.nl_len + n->btn_free_space.nl_off +
	    n->btn_free_space.nl_len);
	mem_copy((uint8_t *)bl.bl_vals - voff, val, vlen);

	kv = (struct apfs_kvoff *)(node + APFS_BTNODE_HDR_SIZE +
	    n->btn_table_space.nl_off);
	for (i = bl.bl_nkeys; i > pos; i--)
		kv[i] = kv[i - 1];
	kv[pos].k = (uint16_t)koff;
	kv[pos].v = (uint16_t)voff;
	n->btn_nkeys++;
	return (FS_APFS_E_OK);
}

/*
 * Build a node holding records [from, to) of another, laid out afresh, and
 * SAYING WHAT KIND OF NODE IT IS.
 *
 * Rebuilding rather than moving bytes about is the point.  A node's key area
 * accumulates holes as records come and go, and half of one copied verbatim
 * would carry a free-list chain describing space that is no longer in it.
 * Re-inserting each record produces a layout correct by construction, through
 * the same insert every other writer here goes through.
 *
 * The kind is an argument because the tree can now gain a level, and the two
 * halves a root splits into are not roots.  Three things say so and the
 * checker reads all three: the ROOT flag, which also decides whether the last
 * forty bytes are a btree_info or free space; the object type, which is
 * BTREE_ROOT for one and BTREE_NODE for the others; and the level.  Leaving
 * the flag on a half was answered with "B-tree node: wrong object type for
 * root" and leaving the type behind with "wrong object type for nonroot" --
 * the same obligation seen from either side.
 */
static int
node_rebuild_as(const uint8_t *src, uint32_t from, uint32_t to, uint8_t *dst,
    uint16_t flags, uint32_t type)
{
	const struct apfs_btree_node_phys	*s;
	struct apfs_btree_node_phys		*d;
	struct btree_layout			 bl;
	uint32_t				 koff, klen, voff, vlen;
	uint32_t				 i;
	int					 rv;

	s = (const struct apfs_btree_node_phys *)src;
	btree_layout(src, &bl);
	if (bl.bl_fixed || to > bl.bl_nkeys || from > to)
		return (FS_APFS_E_INVAL);

	mem_zero(dst, APFS_BLOCK_SIZE);
	d  = (struct apfs_btree_node_phys *)dst;
	*d = *s;
	d->btn_o.o_type = (s->btn_o.o_type & ~APFS_OBJ_TYPE_MASK) | type;
	d->btn_flags              = flags;
	d->btn_nkeys              = 0;
	d->btn_table_space.nl_off = 0;
	d->btn_table_space.nl_len = s->btn_table_space.nl_len;
	d->btn_free_space.nl_off  = 0;
	d->btn_free_space.nl_len  = (uint16_t)(APFS_BLOCK_SIZE -
	    APFS_BTNODE_HDR_SIZE - d->btn_table_space.nl_len -
	    (((d->btn_flags & APFS_BTNODE_ROOT) != 0) ?
	    APFS_BTREE_INFO_SIZE : 0));
	/*
	 * No holes, and the chains have to SAY so.  0xFFFF is the format's
	 * "no such offset"; a zero would name the first byte of the key area
	 * as a hole, and a checker walking the chain would read a live record
	 * as a hole header -- which is the mistake the free queues made once
	 * already ("B-tree node: free key is too small").
	 */
	d->btn_key_free_list.nl_off = APFS_BTOFF_INVALID;
	d->btn_key_free_list.nl_len = 0;
	d->btn_val_free_list.nl_off = APFS_BTOFF_INVALID;
	d->btn_val_free_list.nl_len = 0;
	if ((d->btn_flags & APFS_BTNODE_ROOT) != 0)
		mem_copy(dst + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE,
		    src + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE,
		    APFS_BTREE_INFO_SIZE);

	for (i = from; i < to; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		rv = leaf_insert(dst, i - from, bl.bl_keys + koff, klen,
		    bl.bl_vals - voff, vlen);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}
	return (FS_APFS_E_OK);
}

/* The same node again, which is what a split of anything but a root wants. */
static int
node_rebuild(const uint8_t *src, uint32_t from, uint32_t to, uint8_t *dst)
{
	const struct apfs_btree_node_phys	*s;

	s = (const struct apfs_btree_node_phys *)src;
	return (node_rebuild_as(src, from, to, dst, s->btn_flags,
	    s->btn_o.o_type & APFS_OBJ_TYPE_MASK));
}

/*
 * How many NODES a tree has, which lives beside the record count in the
 * btree_info at the end of the root -- so every split moves the root as well,
 * whether or not the root is the node that split.
 */
static void
tree_nodes_add(uint8_t *root, int64_t delta)
{
	uint64_t	*count;

	count = (uint64_t *)(root + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE +
	    APFS_BTREE_INFO_NODECOUNT);
	*count = (uint64_t)((int64_t)*count + delta);
}

/*
 * And the same number read back, from a buffer the caller says is a root.
 * Says so rather than trusting: the forty bytes are a btree_info only in a
 * node carrying the ROOT flag, and in any other node they are records.
 */
static bool
tree_nodes_of(const uint8_t *root, uint64_t *out)
{
	struct btree_layout	bl;

	btree_layout(root, &bl);
	if ((bl.bl_flags & APFS_BTNODE_ROOT) == 0)
		return (false);
	*out = *(const uint64_t *)(root + APFS_BLOCK_SIZE -
	    APFS_BTREE_INFO_SIZE + APFS_BTREE_INFO_NODECOUNT);
	return (true);
}

/*
 * WHO IS ABOVE A NODE
 *
 * A split hands its parent a separator, and until now "the parent" was a word
 * for the root: the tree was two levels deep and there was nothing else it
 * could be.  A tree that can gain a level has to be asked.
 *
 * By SEARCH and not by key.  Descending on the key is faster and is what a
 * lookup does, but it answers a different question -- which node a key BELONGS
 * in -- and the two agree only while the tree is in order, which is exactly
 * what the caller is in the middle of maintaining.  Following the child
 * pointers until the block turns up cannot be fooled by a separator that is
 * wrong, and the cost is a handful of node reads once per split.
 *
 * The path is the block AND oid of every node from the root down, because the
 * two are different questions in a copy-on-write tree: the block is where a
 * node is now, the oid is the name everything above it uses, and a copy
 * changes the first without touching the second.
 */
#define	APFS_TREE_MAX_DEPTH	8

struct tree_path {
	uint64_t	tp_bno[APFS_TREE_MAX_DEPTH];
	uint64_t	tp_oid[APFS_TREE_MAX_DEPTH];
	uint32_t	tp_n;			/* the root is [0] */
};

static bool
path_walk(uint64_t bno, uint64_t want, struct tree_path *tp, uint32_t depth)
{
	struct btree_layout	 bl;
	uint8_t			*node;
	uint64_t		 oid;
	uint64_t		 child;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 i;
	bool			 found;

	if (depth >= APFS_TREE_MAX_DEPTH)
		return (false);
	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (false);
	if (fs_apfs_read_block(bno, node) != FS_APFS_E_OK) {
		kfree(node);
		return (false);
	}
	tp->tp_bno[depth] = bno;
	tp->tp_oid[depth] = ((const struct apfs_obj_phys *)node)->o_oid;
	if (bno == want) {
		tp->tp_n = depth + 1;
		kfree(node);
		return (true);
	}
	btree_layout(node, &bl);
	found = false;
	if ((bl.bl_flags & APFS_BTNODE_LEAF) == 0) {
		for (i = 0; !found && i < bl.bl_nkeys; i++) {
			btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
			oid = *(const uint64_t *)(bl.bl_vals - voff);
			if (fs_apfs_omap_lookup(g_apfs.ac_vol_omap_tree, oid,
			    view_xid(), &child) != FS_APFS_E_OK)
				break;
			found = path_walk(child, want, tp, depth + 1);
		}
	}
	kfree(node);
	return (found);
}

static bool
path_to(uint64_t want, struct tree_path *tp)
{

	tp->tp_n = 0;
	return (path_walk(g_apfs.ac_root_tree_bno, want, tp, 0));
}

/*
 * MORE THAN ONE NODE AT A TIME
 *
 * The records one operation touches are not always neighbours, and a tree that
 * splits keeps making that truer.  A name's records never were: an entry sorts
 * under the PARENT's object id while the inode and its data-stream reference
 * sort under the child's.  A file's records LOOK like neighbours -- the inode,
 * its dstream id and its extents share an object id and differ only in the
 * record type -- but neighbours in key order are in the same node only until a
 * split falls between them, and then a truncate that could edit one node could
 * not do its job at all.
 *
 * So this is the shape every writer here uses to change several nodes as one
 * event: name the leaves, read them all, edit them in memory, and commit.  A
 * refusal therefore costs nothing, which matters more than it sounds -- the
 * alternative is a writer that has already copied one node when it discovers
 * the second has no room.
 *
 * ONE COPY PER NODE PER TRANSACTION is the rule underneath it.  Copying a node
 * twice would leave the second copy replacing a node the first had already
 * released, so the leaves are de-duplicated as they are named and every edit
 * lands in the one buffer that stands for that node.
 */

/*
 * The widest edit here is a truncate: APFS_TRUNC_MAX runs, each of which may
 * have ended up in a leaf of its own, plus the inode's.  The static assert
 * beside that constant keeps the two from drifting apart.
 */
#define	APFS_EDIT_LEAVES	9

struct leaf_edit {
	uint64_t	 le_bno[APFS_EDIT_LEAVES];	/* distinct leaves */
	uint64_t	 le_oid[APFS_EDIT_LEAVES];
	uint64_t	 le_new[APFS_EDIT_LEAVES];
	uint8_t		*le_node[APFS_EDIT_LEAVES];
	bool		 le_gone[APFS_EDIT_LEAVES];	/* left the tree   */
	uint32_t	 le_n;
	uint32_t	 le_dropped;	/* how many of them did            */
	uint32_t	 le_root;	/* which of them is the tree root  */
};

/*
 * Remember a leaf, once, and say which slot it took -- or APFS_EDIT_LEAVES if
 * there is no room for another, which the caller must check.  Answering with
 * the count itself rather than writing past the end is the difference between
 * a refusal and a corrupted stack: this used to trust its callers to know how
 * many leaves they could touch, which was true of the two that existed.
 */
static uint32_t
edit_leaf(struct leaf_edit *ne, uint64_t bno)
{
	uint32_t	i;

	for (i = 0; i < ne->le_n; i++)
		if (ne->le_bno[i] == bno)
			return (i);
	if (ne->le_n >= APFS_EDIT_LEAVES)
		return (APFS_EDIT_LEAVES);
	ne->le_bno[ne->le_n] = bno;
	return (ne->le_n++);
}

/*
 * Empty, and SAFE TO FREE.  Called before the first leaf is named, because
 * every one of these operations can refuse before it reads anything and they
 * all release through the same label on the way out.
 */
static void
edit_init(struct leaf_edit *ne)
{
	uint32_t	i;

	ne->le_n       = 0;
	ne->le_dropped = 0;
	ne->le_root    = APFS_EDIT_LEAVES;
	for (i = 0; i < APFS_EDIT_LEAVES; i++) {
		ne->le_node[i] = NULL;
		ne->le_new[i]  = 0;
		ne->le_gone[i] = false;
	}
}

/*
 * Has any of it reached the disk yet?
 *
 * A commit refuses before its first copy -- room is settled in memory, which is
 * the whole point of the shape above -- so a caller whose commit was refused
 * can put back the volume counters it had already adjusted, and MUST: nothing
 * was written, and the next checkpoint would otherwise publish a file count for
 * a file that is still there.  Once a copy has landed the answer is yes and the
 * checkpoint is not to be written at all, so what the counters say stops
 * mattering.
 */
static bool
edit_moved(const struct leaf_edit *ne)
{
	uint32_t	i;

	for (i = 0; i < ne->le_n; i++)
		if (ne->le_new[i] != 0)
			return (true);
	return (false);
}

/* One named leaf, brought into memory. */
static int
edit_load(struct leaf_edit *ne, uint32_t i)
{
	int	rv;

	ne->le_node[i] = kmalloc(APFS_BLOCK_SIZE);
	if (ne->le_node[i] == NULL)
		return (FS_APFS_E_NOMEM);
	rv = fs_apfs_read_block(ne->le_bno[i], ne->le_node[i]);
	if (rv != FS_APFS_E_OK)
		return (rv);
	ne->le_oid[i] = ((struct apfs_obj_phys *)ne->le_node[i])->o_oid;
	if (ne->le_oid[i] == g_apfs.ac_root_tree_oid)
		ne->le_root = i;
	return (FS_APFS_E_OK);
}

/*
 * Read every leaf the edit touches, all at once, so that a refusal costs
 * nothing.  Every insert and delete then happens in memory; only when all of
 * them have succeeded does anything reach the disk.
 */
static int
edit_read(struct leaf_edit *ne)
{
	uint32_t	i;
	int		rv;

	for (i = 0; i < ne->le_n; i++) {
		rv = edit_load(ne, i);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}
	return (FS_APFS_E_OK);
}

static void
edit_free(struct leaf_edit *ne)
{
	uint32_t	i;

	for (i = 0; i < APFS_EDIT_LEAVES; i++)
		if (ne->le_node[i] != NULL)
			kfree(ne->le_node[i]);
}

/*
 * A NODE'S FIRST KEY IS ALSO ITS PARENT'S BUSINESS
 *
 * The key an index node stores for a child is that child's own first key, and
 * the checker does not treat that as a convention:
 *
 *	B-tree: index key absent from child node.
 *
 * Which means a delete has a consequence a delete does not look like it has.
 * Take the first record out of a leaf and the leaf is still sorted, still
 * reachable, still correct to read -- and the key its parent files it under
 * describes a record that is no longer in it.  The same is true of an insert
 * that lands before everything else in a node.
 *
 * So every node this edit touched is held up against what its parent says
 * about it, and the parent is corrected where they disagree.  BY ASKING THE
 * PARENT rather than by remembering the key beforehand: the parent's copy is
 * the thing that has to be true, so comparing against it checks the invariant
 * itself instead of a proxy for it.
 *
 * The correction can cascade.  Fixing a parent's first entry changes the
 * parent's own first key, which makes ITS parent stale -- so a corrected node
 * joins the edit and the loop reaches it, all the way to the root, which has
 * nobody above it to tell.
 *
 * The key is REPLACED and not patched, because keys are not all one size: an
 * extent key is sixteen bytes, an inode's is eight, a name's is as long as the
 * name.  So it is a delete and an insert in the same slot -- the entry cannot
 * move, since the new key is still inside the range that child owns -- and
 * that can fail for want of room, which is why this runs before anything has
 * been copied.
 */
/*
 * A NODE THAT HAS LOST ITS LAST RECORD LEAVES THE TREE
 *
 * It cannot stay.  A node is filed above under a key that is its own first
 * key, and a node with no records has no first key, so the index above it
 * describes a record nobody can find -- which is not a matter of tidiness:
 * an emptied node left in place was answered with
 *
 *	B-tree: keys are out of order.
 *
 * measured on a copy of the image before this was written.  Which is why the
 * writer used to refuse the delete outright rather than leave one behind.
 *
 * Taking it out is five things, and the same measurement says what each one is
 * for -- everything below done except one:
 *
 *	the parent stops naming it	B-tree: keys are out of order
 *	the object map forgets its oid	Omap record: oid-xid combination is
 *					never used
 *	its block goes back		Space manager: bad allocation bitmap
 *	the tree counts one node fewer	Catalog: wrong node count in info footer
 *	the volume owns one block fewer	Volume superblock: bad block count
 *
 * This does the first; the other four happen in edit_commit, which is where
 * blocks move and where the object map is told.  The node is not copied at
 * all -- it is marked gone, and a gone node's block is released instead.
 *
 * WHAT THIS DOES NOT DO is give a LEVEL back.  A root left with a single child
 * could be replaced by that child, and the same measurement says it does not
 * have to be: a root with one child is accepted in silence.  So the tree can
 * end up taller than its contents need, which costs a lookup one hop and is
 * not a correctness question.  Collapsing it is its own rung.
 *
 * The cascade IS here, though, and it falls out of the shape rather than being
 * arranged: the parent joins the edit, and if it was holding nothing but this
 * child it is now empty itself, and the pass over the edit reaches it.
 */
static int
node_drop(struct leaf_edit *ne, uint32_t i, uint8_t *scratch)
{
	struct tree_path	 tp;
	struct btree_layout	 bl;
	uint8_t			*parent;
	uint32_t		 pkoff, pklen, pvoff, pvlen;
	uint32_t		 pslot;
	uint32_t		 pos;
	int			 rv;

	(void)scratch;
	if (!path_to(ne->le_bno[i], &tp) || tp.tp_n < 2) {
		kprintf("apfs: the empty node at %llu is not reachable from "
		    "the root of the tree it is in\n",
		    (unsigned long long)ne->le_bno[i]);
		return (FS_APFS_E_NOTFOUND);
	}
	pslot = edit_leaf(ne, tp.tp_bno[tp.tp_n - 2]);
	if (pslot == APFS_EDIT_LEAVES) {
		kprintf("apfs: no room in this edit for the index above the "
		    "empty node at %llu\n", (unsigned long long)ne->le_bno[i]);
		return (FS_APFS_E_SPREAD);
	}
	if (ne->le_node[pslot] == NULL) {
		rv = edit_load(ne, pslot);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}
	parent = ne->le_node[pslot];

	btree_layout(parent, &bl);
	for (pos = 0; pos < bl.bl_nkeys; pos++) {
		btree_entry_loc(&bl, pos, &pkoff, &pklen, &pvoff, &pvlen);
		if (pvlen != sizeof(uint64_t))
			continue;
		if (*(const uint64_t *)(bl.bl_vals - pvoff) == ne->le_oid[i])
			break;
	}
	if (pos == bl.bl_nkeys) {
		kprintf("apfs: the node above the empty one at %llu does not "
		    "name oid %llu\n", (unsigned long long)ne->le_bno[i],
		    (unsigned long long)ne->le_oid[i]);
		return (FS_APFS_E_NOTFOUND);
	}
	/*
	 * The root is the one node that cannot go, since the volume superblock
	 * names it -- so a tree whose last leaf has emptied stops here rather
	 * than unmaking itself.  Nothing can reach this while the volume holds
	 * a single file, because the root directory's own records are in it.
	 */
	if (bl.bl_nkeys == 1 && ne->le_oid[pslot] == g_apfs.ac_root_tree_oid) {
		kprintf("apfs: the tree's last node has emptied -- a tree with "
		    "nothing in it is not a state this writer makes\n");
		return (FS_APFS_E_NOALLOC);
	}
	rv = leaf_delete(parent, pos);
	if (rv != FS_APFS_E_OK)
		return (rv);

	ne->le_gone[i] = true;
	ne->le_dropped++;
	drop_n++;
	kprintf("apfs: the node at %llu lost its last record and left the tree "
	    "-- oid %llu names nothing now, and the node at %llu holds %u\n",
	    (unsigned long long)ne->le_bno[i],
	    (unsigned long long)ne->le_oid[i],
	    (unsigned long long)ne->le_bno[pslot], (unsigned)(bl.bl_nkeys - 1));
	return (FS_APFS_E_OK);
}

static int
edit_reindex_one(struct leaf_edit *ne, uint32_t i, uint8_t *scratch)
{
	struct tree_path	 tp;
	struct btree_layout	 bl;
	uint8_t			*parent;
	uint64_t		 child;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 pkoff, pklen, pvoff, pvlen;
	uint32_t		 pslot;
	uint32_t		 pos;
	uint32_t		 n;
	int			 rv;

	if (ne->le_gone[i])
		return (FS_APFS_E_OK);		/* not in the tree any more */
	if (ne->le_oid[i] == g_apfs.ac_root_tree_oid)
		return (FS_APFS_E_OK);		/* nothing above it */
	btree_layout(ne->le_node[i], &bl);
	if (bl.bl_nkeys == 0)
		return (node_drop(ne, i, scratch));

	if (!path_to(ne->le_bno[i], &tp) || tp.tp_n < 2) {
		kprintf("apfs: the node at %llu is not reachable from the root "
		    "of the tree it is in\n", (unsigned long long)ne->le_bno[i]);
		return (FS_APFS_E_NOTFOUND);
	}
	pslot = edit_leaf(ne, tp.tp_bno[tp.tp_n - 2]);
	if (pslot == APFS_EDIT_LEAVES) {
		kprintf("apfs: no room in this edit for the index above the "
		    "node at %llu\n", (unsigned long long)ne->le_bno[i]);
		return (FS_APFS_E_SPREAD);
	}
	if (ne->le_node[pslot] == NULL) {
		rv = edit_load(ne, pslot);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}
	parent = ne->le_node[pslot];

	btree_layout(parent, &bl);
	for (pos = 0; pos < bl.bl_nkeys; pos++) {
		btree_entry_loc(&bl, pos, &pkoff, &pklen, &pvoff, &pvlen);
		if (pvlen != sizeof(child))
			continue;
		if (*(const uint64_t *)(bl.bl_vals - pvoff) == ne->le_oid[i])
			break;
	}
	if (pos == bl.bl_nkeys) {
		kprintf("apfs: the node above %llu does not name oid %llu\n",
		    (unsigned long long)ne->le_bno[i],
		    (unsigned long long)ne->le_oid[i]);
		return (FS_APFS_E_NOTFOUND);
	}
	/*
	 * The child's first key, taken out of the child's own buffer and kept
	 * aside: the insert below rearranges the parent, and the layout the
	 * key was read through belongs to a different node anyway.
	 */
	btree_layout(ne->le_node[i], &bl);
	btree_entry_loc(&bl, 0, &koff, &klen, &voff, &vlen);
	if (klen > APFS_BLOCK_SIZE)
		return (FS_APFS_E_INVAL);
	mem_copy(scratch, bl.bl_keys + koff, klen);

	btree_layout(parent, &bl);
	btree_entry_loc(&bl, pos, &pkoff, &pklen, &pvoff, &pvlen);
	if (pklen == klen && jkey_cmp(bl.bl_keys + pkoff, pklen, scratch,
	    klen) == 0)
		return (FS_APFS_E_OK);		/* the index is already right */

	child = ne->le_oid[i];
	n     = bl.bl_nkeys;
	rv = leaf_delete(parent, pos);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = leaf_insert(parent, pos, scratch, klen, &child,
	    (uint32_t)sizeof(child));
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: the index above %llu cannot hold that child's "
		    "new first key\n", (unsigned long long)ne->le_bno[i]);
		return (rv);
	}
	btree_layout(parent, &bl);
	if (bl.bl_nkeys != n)
		return (FS_APFS_E_INVAL);
	reidx_n++;
	return (FS_APFS_E_OK);
}

static int
edit_reindex(struct leaf_edit *ne, uint8_t *scratch)
{
	uint32_t	i;
	uint32_t	settled;
	int		rv;

	/*
	 * The bound is re-read every time round, because correcting a parent
	 * puts that parent INTO this list -- and it may need correcting too.
	 *
	 * And the whole pass repeats until nothing changes, because a node can
	 * be dealt with and then invalidated by a node dealt with after it: a
	 * parent already walked past is emptied by the LAST of its children
	 * going, and an empty node that nobody looks at again is exactly what
	 * this is here to prevent.  Two numbers say whether anything happened
	 * -- how many nodes the edit holds, and how many have left the tree --
	 * and a pass that moves neither is a pass that found nothing to do.
	 */
	do {
		settled = ne->le_n + ne->le_dropped;
		for (i = 0; i < ne->le_n; i++) {
			rv = edit_reindex_one(ne, i, scratch);
			if (rv != FS_APFS_E_OK)
				return (rv);
		}
	} while (settled != ne->le_n + ne->le_dropped);
	return (FS_APFS_E_OK);
}

/*
 * And write them, plus the root whose record count moved.  Nothing above this
 * can fail for want of room -- that was settled in memory -- so a failure here
 * is a disk that stopped answering, and it leaves a half-built checkpoint that
 * must not be committed.
 */
static int
edit_commit(struct leaf_edit *ne, int64_t records, uint64_t xid,
    uint8_t *scratch)
{
	struct omap_edit	oe;
	uint64_t		oids[APFS_EDIT_LEAVES + 1];
	uint64_t		paddrs[APFS_EDIT_LEAVES + 1];
	uint64_t		gone[APFS_EDIT_LEAVES];
	uint64_t		new_root;
	uint32_t		nmoved;
	uint32_t		ngone;
	uint32_t		i;
	int			rv;

	/*
	 * FIRST, and in memory: an index that no longer describes its child is
	 * the one thing here that can still refuse, and it can add nodes to
	 * the list this function is about to copy.  It is also where a node
	 * that has lost its last record leaves the tree, which takes nodes OUT
	 * of that list -- so the two counts below are not both le_n.
	 */
	rv = edit_reindex(ne, scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);

	if (ne->le_root < ne->le_n) {
		tree_count_add(ne->le_node[ne->le_root], records);
		tree_nodes_add(ne->le_node[ne->le_root],
		    -(int64_t)ne->le_dropped);
	}

	nmoved = 0;
	ngone  = 0;
	for (i = 0; i < ne->le_n; i++) {
		/*
		 * A node that has left the tree is not copied: nothing names it
		 * any more, so a copy would be a block written for no reader.
		 * Its block goes back and its oid goes on the list the object
		 * map has to forget.
		 */
		if (ne->le_gone[i]) {
			rv = free_blocks(ne->le_bno[i], 1);
			if (rv != FS_APFS_E_OK)
				return (rv);
			gone[ngone++] = ne->le_oid[i];
			continue;
		}
		rv = node_cow(ne->le_node[i], ne->le_bno[i], xid,
		    &ne->le_new[i]);
		if (rv != FS_APFS_E_OK)
			return (rv);
		oids[nmoved]   = ne->le_oid[i];
		paddrs[nmoved] = ne->le_new[i];
		nmoved++;
	}

	if (ne->le_root < ne->le_n) {
		g_apfs.ac_root_tree_bno = ne->le_new[ne->le_root];
	} else {
		rv = fs_apfs_read_block(g_apfs.ac_root_tree_bno, scratch);
		if (rv != FS_APFS_E_OK)
			return (rv);
		tree_count_add(scratch, records);
		tree_nodes_add(scratch, -(int64_t)ne->le_dropped);
		oids[nmoved] = ((struct apfs_obj_phys *)scratch)->o_oid;
		rv = node_cow(scratch, g_apfs.ac_root_tree_bno, xid, &new_root);
		if (rv != FS_APFS_E_OK)
			return (rv);
		paddrs[nmoved] = new_root;
		g_apfs.ac_root_tree_bno = new_root;
		nmoved++;
	}

	/*
	 * The volume owns one block fewer per node dropped, and it has to say
	 * so BEFORE the spine runs -- the volume superblock carrying that count
	 * is one of the things the spine copies.
	 */
	g_apfs.ac_fs_alloc_count -= ne->le_dropped;

	omap_edit_init(&oe);
	oe.oe_oids   = oids;
	oe.oe_paddrs = paddrs;
	oe.oe_n      = nmoved;
	oe.oe_gone   = gone;
	oe.oe_ngone  = ngone;
	return (spine_update_n(&oe, xid, scratch));
}

/*
 * THE TREE GAINS A LEVEL
 *
 * Every other split has somewhere to put its separator.  The root does not:
 * there is nothing above it, and there cannot be -- the volume superblock
 * names the root's OID, so a root that made way for a new node above it would
 * be a root the volume could no longer find.
 *
 * So the root does not move.  It splits DOWNWARD: its records go into two new
 * nodes at the level it used to occupy, and the root itself is rebuilt in
 * place, one level higher, holding two records that name them.  It keeps its
 * oid, its block moves the way every copy's does, and the tree is a level
 * deeper without anything outside it having to be told.
 *
 * Both halves are therefore NEW objects -- which is why the object map's
 * insertion side had to become a list; a leaf split makes one and this makes
 * two, in a node that has to be copied once either way.
 *
 * What the checker asks for, leaving out one obligation at a time:
 *
 *	leaving out			apfsck answers
 *	  the root's new level		"B-tree: node levels are corrupted"
 *	  the ROOT flag, off the halves	"wrong object type for root"
 *	  the halves' object type	"wrong object type for nonroot"
 *	  the two object-map entries	"Object map: record missing for id"
 *	  the tree's node count		"Catalog: wrong node count in info
 *					 footer"
 *	  the volume's block count	"Volume superblock: bad block count"
 *	  the container's next oid	"Object header: unassigned object id"
 *
 * The last is the container's nx_next_oid, and it is the same trap the create
 * rung found in the volume's apfs_next_obj_id one namespace down: it is not a
 * counter kept for tidiness, it is an assertion that everything at or above it
 * is unused, and an object handed a number the container still calls free is
 * caught by the header check before anything else is looked at.
 */
static int
tree_grow(uint64_t xid, uint8_t *scratch)
{
	struct apfs_btree_node_phys	*n;
	struct btree_layout		 bl;
	struct omap_edit		 oe;
	uint8_t				*lo;
	uint8_t				*hi;
	uint8_t				*nr;
	uint64_t			 ins_oids[2];
	uint64_t			 ins_paddrs[2];
	uint64_t			 child;
	uint64_t			 root_oid;
	uint64_t			 root_bno;
	uint64_t			 new_root;
	uint32_t			 nkeys;
	uint32_t			 half;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;

	lo = kmalloc(APFS_BLOCK_SIZE);
	hi = kmalloc(APFS_BLOCK_SIZE);
	nr = kmalloc(APFS_BLOCK_SIZE);
	if (lo == NULL || hi == NULL || nr == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}

	root_bno = g_apfs.ac_root_tree_bno;
	rv = fs_apfs_read_block(root_bno, scratch);
	if (rv != FS_APFS_E_OK)
		goto out;
	btree_layout(scratch, &bl);
	if ((bl.bl_flags & APFS_BTNODE_ROOT) == 0) {
		kprintf("apfs: the node at %llu is not the tree's root\n",
		    (unsigned long long)root_bno);
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	if ((uint32_t)bl.bl_level + 2u > APFS_TREE_MAX_DEPTH) {
		kprintf("apfs: the tree is already %u levels deep and this "
		    "kernel walks %u\n", (unsigned)(bl.bl_level + 1),
		    (unsigned)APFS_TREE_MAX_DEPTH);
		rv = FS_APFS_E_NOALLOC;
		goto out;
	}
	nkeys = bl.bl_nkeys;
	if (nkeys < 2) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	half     = nkeys / 2;
	root_oid = ((const struct apfs_obj_phys *)scratch)->o_oid;
	if (g_apfs.ac_next_oid == 0) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	ins_oids[0] = g_apfs.ac_next_oid;
	ins_oids[1] = g_apfs.ac_next_oid + 1;

	/*
	 * The halves first, at the level the root is at now, and no longer
	 * roots -- so they lose the flag, the type and the forty bytes of
	 * btree_info, which become free space they can use.
	 */
	rv = node_rebuild_as(scratch, 0, half, lo,
	    (uint16_t)(bl.bl_flags & ~APFS_BTNODE_ROOT), APFS_OBJ_BTREE_NODE);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = node_rebuild_as(scratch, half, nkeys, hi,
	    (uint16_t)(bl.bl_flags & ~APFS_BTNODE_ROOT), APFS_OBJ_BTREE_NODE);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * And the root: the same node with no records in it, one level up.
	 * Rebuilding an EMPTY range is how it keeps its flags, its object type
	 * and its btree_info without any of that being written out again here.
	 */
	rv = node_rebuild_as(scratch, 0, 0, nr, bl.bl_flags,
	    APFS_OBJ_BTREE_ROOT);
	if (rv != FS_APFS_E_OK)
		goto out;
	n = (struct apfs_btree_node_phys *)nr;
	n->btn_level = (uint16_t)(bl.bl_level + 1);
	for (i = 0; i < 2; i++) {
		btree_entry_loc(&bl, i == 0 ? 0 : half, &koff, &klen, &voff,
		    &vlen);
		child = ins_oids[i];
		rv = leaf_insert(nr, i, bl.bl_keys + koff, klen, &child,
		    (uint32_t)sizeof(child));
		if (rv != FS_APFS_E_OK)
			goto out;
	}
	tree_nodes_add(nr, 2);

	rv = alloc_blocks(1, root_bno, &ins_paddrs[0]);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = alloc_blocks(1, root_bno, &ins_paddrs[1]);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(ins_paddrs[0], 1);
		goto out;
	}
	for (i = 0; i < 2; i++) {
		n = (struct apfs_btree_node_phys *)(i == 0 ? lo : hi);
		n->btn_o.o_oid = ins_oids[i];
		n->btn_o.o_xid = xid;
		rv = fs_apfs_write_block(ins_paddrs[i], i == 0 ? lo : hi);
		if (rv != FS_APFS_E_OK) {
			(void)free_blocks(ins_paddrs[0], 1);
			(void)free_blocks(ins_paddrs[1], 1);
			goto out;
		}
	}
	cow_n_spine += 2;

	rv = node_cow(nr, root_bno, xid, &new_root);
	if (rv != FS_APFS_E_OK)
		goto broken;

	/* Two nodes more belong to this volume: three written, one freed. */
	g_apfs.ac_fs_alloc_count += 2;

	omap_edit_init(&oe);
	oe.oe_oids       = &root_oid;
	oe.oe_paddrs     = &new_root;
	oe.oe_n          = 1;
	oe.oe_new        = ins_oids;
	oe.oe_new_paddrs = ins_paddrs;
	oe.oe_nnew       = 2;
	rv = spine_update_n(&oe, xid, scratch);
	if (rv != FS_APFS_E_OK)
		goto broken;

	g_apfs.ac_root_tree_bno = new_root;
	g_apfs.ac_next_oid      = ins_oids[1] + 1;
	deep_n++;
	kprintf("apfs: the tree is %u levels deep -- the root kept oid %llu at "
	    "%llu and its %u children went to new oids %llu and %llu\n",
	    (unsigned)(bl.bl_level + 2), (unsigned long long)root_oid,
	    (unsigned long long)new_root, (unsigned)nkeys,
	    (unsigned long long)ins_oids[0], (unsigned long long)ins_oids[1]);
	rv = FS_APFS_E_OK;
out:
	kfree(lo);
	kfree(hi);
	kfree(nr);
	return (rv);

broken:
	kprintf("apfs: giving the tree another level failed part way (%d) -- "
	    "this checkpoint must not be written\n", rv);
	kfree(lo);
	kfree(hi);
	kfree(nr);
	return (rv);
}

/*
 * Split the node at `bno` in two and tell everything that has to know.
 *
 * Objects move together and none of them is reachable until the checkpoint
 * commits: the lower half (keeping the oid it had), the upper half (a brand
 * new object), the parent that gains a separator, the ROOT whose btree_info
 * counts the tree's nodes, the volume's object map that gains an entry, and
 * the spine above all of it.  The parent and the root are the same node in a
 * two-level tree and different ones in a deeper tree, which is the whole of
 * what changed when the tree learned to grow.
 *
 * NOTHING BETWEEN THEM MOVES, and that is worth saying because it looks like
 * an omission.  An interior node names its children by oid, and a copy keeps
 * its oid -- so a node three levels down can be rewritten without the levels
 * above it knowing, and the object map absorbs the whole difference.  That
 * indirection is what copy-on-write buys with the lookup it costs on every
 * descent.
 *
 * ROOM ABOVE IS ASKED FOR FIRST, before a block is allocated or a byte
 * written.  A split that got half way and found the parent full would have
 * nowhere to put the half it had already made.  A full parent makes room the
 * same way this node is about to -- by splitting, which asks the same question
 * one level up -- and the recursion ends at the root, which cannot split
 * sideways and grows the tree instead.  Either way the question is then asked
 * again of a parent that is half empty by construction.
 *
 * `at` is where to cut, and zero means the middle.  A caller that cares is a
 * TEST: putting a chosen record at the start of the upper half is how the
 * index self-test arranges for that record to be deleted afterwards, which is
 * the one thing that makes a parent's key wrong and which no ordinary writer
 * can be relied on to do on demand.  Zero is not a special value being stolen
 * -- a split there would leave the lower half empty and is refused anyway.
 */
static int
node_split_at(uint64_t bno, uint32_t at, uint64_t xid, uint8_t *scratch)
{
	struct tree_path		 tp;
	struct apfs_obj_phys		*o;
	struct btree_layout		 bl;
	struct omap_edit		 oe;
	uint8_t				*lo;
	uint8_t				*hi;
	uint8_t				*sep;
	uint8_t				*par;
	uint64_t			 oids[3];
	uint64_t			 paddrs[3];
	uint64_t			 lo_bno;
	uint64_t			 hi_bno;
	uint64_t			 lo_oid;
	uint64_t			 hi_oid;
	uint64_t			 parent_bno;
	uint64_t			 new_parent;
	uint64_t			 new_root;
	uint64_t			 top;
	uint32_t			 half;
	uint32_t			 nkeys;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 seplen;
	uint32_t			 nmoved;
	uint32_t			 tries;
	bool				 under_root;
	int				 rv;

	lo  = kmalloc(APFS_BLOCK_SIZE);
	hi  = kmalloc(APFS_BLOCK_SIZE);
	sep = kmalloc(APFS_BLOCK_SIZE);
	par = kmalloc(APFS_BLOCK_SIZE);
	if (lo == NULL || hi == NULL || sep == NULL || par == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}

	nkeys      = 0;
	half       = 0;
	seplen     = 0;
	parent_bno = 0;
	under_root = false;
	for (tries = 0; ; tries++) {
		rv = fs_apfs_read_block(bno, scratch);
		if (rv != FS_APFS_E_OK)
			goto out;
		btree_layout(scratch, &bl);
		if ((bl.bl_flags & APFS_BTNODE_ROOT) != 0) {
			/*
			 * The root has nobody to hand a separator to, so it
			 * does not split sideways -- it grows the tree.
			 */
			rv = tree_grow(xid, scratch);
			goto out;
		}
		nkeys = bl.bl_nkeys;
		if (nkeys < 2) {
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		half = (at != 0 && at < nkeys) ? at : nkeys / 2;
		/*
		 * The separator is the first key of the upper half, which is
		 * this node's key at `half` -- known before the halves exist,
		 * because its LENGTH is what the parent needs room for.
		 */
		btree_entry_loc(&bl, half, &koff, &klen, &voff, &vlen);
		if (klen == 0 || klen > APFS_BLOCK_SIZE) {
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		seplen = klen;

		if (!path_to(bno, &tp) || tp.tp_n < 2) {
			kprintf("apfs: the node at %llu is not reachable from "
			    "the root of the tree it is being split in\n",
			    (unsigned long long)bno);
			rv = FS_APFS_E_NOTFOUND;
			goto out;
		}
		parent_bno = tp.tp_bno[tp.tp_n - 2];
		under_root = tp.tp_n == 2;
		rv = fs_apfs_read_block(parent_bno, par);
		if (rv != FS_APFS_E_OK)
			goto out;
		if (leaf_has_room(par, seplen, (uint32_t)sizeof(hi_oid)))
			break;
		if (tries > 1) {
			kprintf("apfs: making room above the node at %llu did "
			    "not settle\n", (unsigned long long)bno);
			rv = FS_APFS_E_NOALLOC;
			goto out;
		}
		/*
		 * The parent is full, so it has to make room of its own before
		 * this node can hand it anything -- and what it does about that
		 * is the same question one level up.  A full node that is not
		 * the root SPLITS, which hands its own parent a separator; a
		 * full ROOT cannot, and grows the tree instead.  So the
		 * recursion always ends, and it ends at the only node that has
		 * another way out.
		 *
		 * Whatever happens up there, everything worked out below is
		 * stale afterwards -- the parent may be a different node now --
		 * so the loop starts again from the victim rather than trying
		 * to patch up what it had.
		 */
		rv = under_root ? tree_grow(xid, scratch) :
		    node_split_at(parent_bno, 0, xid, scratch);
		if (rv != FS_APFS_E_OK)
			goto out;
	}

	lo_oid = ((const struct apfs_obj_phys *)scratch)->o_oid;
	rv = node_rebuild(scratch, 0, half, lo);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = node_rebuild(scratch, half, nkeys, hi);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The separator kept aside: the root is read into the same scratch
	 * buffer the node came from, so a pointer into that buffer would be
	 * reading the root by the time it is used.
	 */
	btree_layout(hi, &bl);
	btree_entry_loc(&bl, 0, &koff, &klen, &voff, &vlen);
	if (klen != seplen) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	mem_copy(sep, bl.bl_keys + koff, seplen);

	hi_oid = g_apfs.ac_next_oid;
	if (hi_oid == 0) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	rv = alloc_blocks(1, bno, &lo_bno);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = alloc_blocks(1, bno, &hi_bno);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(lo_bno, 1);
		goto out;
	}

	o = (struct apfs_obj_phys *)lo;
	o->o_oid = lo_oid;	/* virtual: the oid is a name, and it keeps it */
	o->o_xid = xid;
	o = (struct apfs_obj_phys *)hi;
	o->o_oid = hi_oid;
	o->o_xid = xid;
	rv = fs_apfs_write_block(lo_bno, lo);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_write_block(hi_bno, hi);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(lo_bno, 1);
		(void)free_blocks(hi_bno, 1);
		goto out;
	}
	rv = free_blocks(bno, 1);
	if (rv != FS_APFS_E_OK)
		goto out;
	cow_n_spine += 2;

	/*
	 * The parent gains the separator.  Its records are (a child's first
	 * key) -> (that child's oid), so the value is eight bytes whatever the
	 * key length happens to be.
	 */
	rv = leaf_insert(par, node_place(par, sep, seplen), sep, seplen,
	    &hi_oid, (uint32_t)sizeof(hi_oid));
	if (rv != FS_APFS_E_OK)
		goto broken;
	/*
	 * One more node in the tree.  The RECORD count does not move: the same
	 * records are still there, in two nodes instead of one.  The count is
	 * in the root, which is the parent only while the tree is two levels
	 * deep -- otherwise it is a second node to copy, above a parent that
	 * has already been dealt with.
	 */
	if (under_root)
		tree_nodes_add(par, 1);
	rv = node_cow(par, parent_bno, xid, &new_parent);
	if (rv != FS_APFS_E_OK)
		goto broken;
	oids[0]   = tp.tp_oid[tp.tp_n - 2];
	paddrs[0] = new_parent;
	oids[1]   = lo_oid;
	paddrs[1] = lo_bno;
	nmoved    = 2;
	top       = new_parent;
	if (!under_root) {
		rv = fs_apfs_read_block(tp.tp_bno[0], scratch);
		if (rv != FS_APFS_E_OK)
			goto broken;
		tree_nodes_add(scratch, 1);
		rv = node_cow(scratch, tp.tp_bno[0], xid, &new_root);
		if (rv != FS_APFS_E_OK)
			goto broken;
		oids[2]   = tp.tp_oid[0];
		paddrs[2] = new_root;
		nmoved    = 3;
		top       = new_root;
	}

	/*
	 * The object map learns where all of them are: the halves and the
	 * nodes above by replacement, the new half by insertion, all inside
	 * one copy of its node.
	 */
	/*
	 * One more block belongs to this volume: two nodes were written where
	 * one was freed.  The count is the volume's own claim about itself and
	 * apfsck recomputes it, so a split that forgets produces the same
	 * "Volume superblock: bad block count" that growing a file did -- what
	 * it counts is blocks, not what is in them, and the fixture's own
	 * arithmetic says so: 91 extent blocks, 3 tree nodes, 2 for the object
	 * map, 1 for the extent reference tree and 1 for the volume superblock
	 * itself add up to the 98 it stores.
	 *
	 * BEFORE the spine, and this is the second time that ordering has bitten
	 * in this file: the superblock carrying the count is copied by
	 * spine_update, so a count raised afterwards belongs to no transaction
	 * at all and is lost at the next mount.
	 */
	g_apfs.ac_fs_alloc_count += 1;

	omap_edit_init(&oe);
	oe.oe_oids       = oids;
	oe.oe_paddrs     = paddrs;
	oe.oe_n          = nmoved;
	oe.oe_new        = &hi_oid;
	oe.oe_new_paddrs = &hi_bno;
	oe.oe_nnew       = 1;
	rv = spine_update_n(&oe, xid, scratch);
	if (rv != FS_APFS_E_OK)
		goto broken;

	g_apfs.ac_root_tree_bno = top;
	g_apfs.ac_next_oid      = hi_oid + 1;
	split_n++;
	kprintf("apfs: node %llu split -- %u records stay as oid %llu at %llu, "
	    "%u move to a new oid %llu at %llu, under the node at %llu\n",
	    (unsigned long long)bno, (unsigned)half, (unsigned long long)lo_oid,
	    (unsigned long long)lo_bno, (unsigned)(nkeys - half),
	    (unsigned long long)hi_oid, (unsigned long long)hi_bno,
	    (unsigned long long)new_parent);
	rv = FS_APFS_E_OK;
out:
	kfree(lo);
	kfree(hi);
	kfree(sep);
	kfree(par);
	return (rv);

broken:
	kprintf("apfs: splitting the node at %llu failed part way (%d) -- this "
	    "checkpoint must not be written\n", (unsigned long long)bno, rv);
	kfree(lo);
	kfree(hi);
	kfree(sep);
	kfree(par);
	return (rv);
}

/*
 * Make a file longer.
 *
 * Two cases, and the cheap one is worth having: a file whose last block is
 * only partly used grows into that block without any record changing except
 * its length.  Only when the allocation really is exhausted does this take a
 * run, hand it to both trees, and pay for a record.
 *
 * The new run is ZEROED.  A block just taken from the allocator holds whatever
 * the file that had it last left there, and handing that to a reader as the
 * tail of their file is a disclosure, not a bug in the arithmetic.
 */
static int
grow_once(uint64_t ino, uint64_t id, uint64_t new_size, uint64_t *full_leaf)
{
	struct apfs_file_extent_val	 fe;
	struct apfs_dstream		*ds;
	struct apfs_inode_val		*iv;
	struct apfs_xf_blob		*blob;
	struct apfs_x_field		*xf;
	struct btree_layout		 bl;
	struct inode_info		 ii;
	struct leaf_edit		 ne;
	struct leaf_find		 lf;
	uint8_t				*node;
	uint8_t				*zero;
	const uint8_t			*k;
	uint64_t			 key[2];
	uint64_t			 first;
	uint64_t			 blocks;
	uint64_t			 alloced;
	uint64_t			 near;
	uint64_t			 ino_leaf;
	uint64_t			 ext_leaf;
	uint64_t			 raw;
	uint64_t			 xid;
	uint64_t			 b;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 ext_slot;
	uint32_t			 ino_slot;
	uint32_t			 ent, data, nexts;
	uint32_t			 pos;
	uint32_t			 i;
	int				 rv;
	bool				 stopped;
	bool				 merge;
	struct extent_locate		 last;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);
	if (inode_info(ino, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	if (new_size <= ii.ii_size)
		return (FS_APFS_E_OK);

	alloced = ii.ii_alloced;
	blocks  = 0;
	first   = 0;
	xid     = g_apfs.ac_xid + 1;
	node    = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (FS_APFS_E_NOMEM);
	edit_init(&ne);		/* before the first thing that can fail out */

	/*
	 * A run, if the file has run out of one.  Taken next to the bytes it
	 * extends, which keeps the release of the whole thing reachable later
	 * and keeps a growing file from scattering across the container.
	 */
	merge          = false;
	last.el_found  = false;
	if (new_size > alloced) {
		blocks = (new_size - alloced + APFS_BLOCK_SIZE - 1) /
		    APFS_BLOCK_SIZE;
		near = 0;
		if (alloced > 0) {
			last.el_id    = id;
			last.el_want  = alloced - 1;
			last.el_found = false;
			stopped = false;
			if (!btree_walk(g_apfs.ac_root_tree_bno, extent_locate,
			    &last, 0, &stopped)) {
				rv = FS_APFS_E_IO;
				goto out;
			}
			/*
			 * The block just PAST the run, not its start: what
			 * this wants is to continue it, and alloc_blocks
			 * takes the hint literally when what is there is
			 * free.
			 */
			if (last.el_found && last.el_phys != 0)
				near = last.el_phys +
				    last.el_len / APFS_BLOCK_SIZE;
		}
		rv = alloc_blocks((uint32_t)blocks, near, &first);
		if (rv != FS_APFS_E_OK)
			goto out;

		/*
		 * TWO RUNS THAT TOUCH ARE ONE RUN.  If the allocator handed
		 * back the blocks immediately after the file's last extent --
		 * which, asked to stay near them, it usually does -- then
		 * lengthening that extent says the same thing as adding a
		 * record and costs nothing.  Appending block by block would
		 * otherwise spend a record per block in each of two trees, and
		 * the extent reference tree is one node: it filled after four.
		 */
		if (last.el_found && last.el_phys != 0 &&
		    last.el_logical + last.el_len == alloced &&
		    last.el_phys + last.el_len / APFS_BLOCK_SIZE == first)
			merge = true;

		zero = kmalloc(APFS_BLOCK_SIZE);
		if (zero == NULL) {
			(void)free_blocks(first, (uint32_t)blocks);
			rv = FS_APFS_E_NOMEM;
			goto out;
		}
		mem_zero(zero, APFS_BLOCK_SIZE);
		for (b = 0; b < blocks; b++) {
			rv = write_block_raw(first + b, zero);
			if (rv != FS_APFS_E_OK) {
				kfree(zero);
				(void)free_blocks(first, (uint32_t)blocks);
				goto out;
			}
		}
		kfree(zero);
	}

	/*
	 * The inode's length lives in an extended field of its record, so the
	 * patch is a fixed-size edit inside a value that is not: find the
	 * dstream by walking the field table, exactly as the reader does.
	 */
	{
		struct inode_locate	il;

		il.il_oid   = ino;
		il.il_bno   = 0;
		il.il_found = false;
		stopped = false;
		if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0,
		    &stopped)) {
			rv = FS_APFS_E_IO;
			goto give_back;
		}
		if (!il.il_found) {
			rv = FS_APFS_E_NOTFOUND;
			goto give_back;
		}
		ino_leaf = il.il_bno;
	}

	/*
	 * WHICH LEAF THE EXTENT BELONGS IN, which need not be the inode's.
	 *
	 * A file's records sort together -- they share an object id -- but
	 * sorting together is not living together: a split falls where the tree
	 * needs it, and after enough of them a file's bytes and its length are
	 * in different nodes.  Both leaves are read and edited as one edit, and
	 * the de-duplication in edit_leaf means the common case (one leaf,
	 * named twice) still copies a single node.
	 */
	key[0] = (id & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_FILE_EXTENT << APFS_J_OBJ_TYPE_SHIFT);
	key[1] = alloced;
	lf.lf_key   = (const uint8_t *)key;
	lf.lf_klen  = (uint32_t)sizeof(key);
	lf.lf_bno   = 0;
	lf.lf_first = 0;
	lf.lf_any   = false;
	stopped = false;
	ext_leaf = ino_leaf;
	if (blocks != 0 && merge) {
		ext_leaf = last.el_bno;		/* the record being lengthened */
	} else if (blocks != 0) {
		if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_find, &lf, 0,
		    &stopped)) {
			rv = FS_APFS_E_IO;
			goto give_back;
		}
		ext_leaf = lf.lf_bno != 0 ? lf.lf_bno : lf.lf_first;
	}

	ext_slot = edit_leaf(&ne, ext_leaf);
	ino_slot = edit_leaf(&ne, ino_leaf);
	if (ext_slot == APFS_EDIT_LEAVES || ino_slot == APFS_EDIT_LEAVES) {
		rv = FS_APFS_E_SPREAD;
		goto give_back;
	}
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto give_back;

	/*
	 * Room, asked before anything is changed.  Saying which leaf was full
	 * rather than merely that one was is what lets the caller split it and
	 * come back: after a split the record may belong in either half, so
	 * everything above has to be worked out again from scratch.
	 */
	if (blocks != 0 && !merge && !leaf_has_room(ne.le_node[ext_slot],
	    (uint32_t)sizeof(key), (uint32_t)sizeof(fe))) {
		*full_leaf = ext_leaf;
		rv = FS_APFS_E_NOALLOC;
		goto give_back;
	}

	if (blocks != 0 && merge) {
		/*
		 * Lengthen the record that is already there.  Its key does not
		 * move -- it is keyed on where the run starts in the file, and
		 * that is unchanged -- so this is the same shape of edit as
		 * stamping a modification time.
		 */
		btree_layout(ne.le_node[ext_slot], &bl);
		rv = FS_APFS_E_NOTFOUND;
		for (pos = 0; pos < bl.bl_nkeys; pos++) {
			struct apfs_file_extent_val	*ex;

			btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);
			if (klen < 16 || vlen < sizeof(*ex))
				continue;
			k   = bl.bl_keys + koff;
			raw = *(const uint64_t *)k;
			if ((raw & APFS_J_OBJ_ID_MASK) != (id &
			    APFS_J_OBJ_ID_MASK))
				continue;
			if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) !=
			    APFS_TYPE_FILE_EXTENT)
				continue;
			if (*(const uint64_t *)(k + 8) != last.el_logical)
				continue;
			ex = (struct apfs_file_extent_val *)(bl.bl_vals - voff);
			ex->fe_len_and_flags += blocks * APFS_BLOCK_SIZE;
			rv = FS_APFS_E_OK;
			break;
		}
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the extent at file offset %llu is gone "
			    "-- cannot lengthen it\n",
			    (unsigned long long)last.el_logical);
			goto give_back;
		}
	} else if (blocks != 0) {
		btree_layout(ne.le_node[ext_slot], &bl);
		for (pos = 0; pos < bl.bl_nkeys; pos++) {
			btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);
			if (jkey_cmp(bl.bl_keys + koff, klen,
			    (const uint8_t *)key, (uint32_t)sizeof(key)) > 0)
				break;
		}
		fe.fe_len_and_flags  = blocks * APFS_BLOCK_SIZE;
		fe.fe_phys_block_num = first;
		fe.fe_crypto_id      = 0;
		rv = leaf_insert(ne.le_node[ext_slot], pos, key,
		    (uint32_t)sizeof(key), &fe, (uint32_t)sizeof(fe));
		if (rv != FS_APFS_E_OK)
			goto give_back;
	}

	/* And the length, in the inode record, wherever that lives. */
	btree_layout(ne.le_node[ino_slot], &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		k   = bl.bl_keys + koff;
		raw = *(const uint64_t *)k;
		if ((raw & APFS_J_OBJ_ID_MASK) != ino)
			continue;
		if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_INODE)
			continue;
		iv = (struct apfs_inode_val *)(bl.bl_vals - voff);
		if (vlen < sizeof(*iv) + sizeof(*blob))
			break;
		blob  = (struct apfs_xf_blob *)((uint8_t *)iv + sizeof(*iv));
		nexts = blob->xb_num_exts;
		ent   = (uint32_t)(sizeof(*iv) + sizeof(*blob));
		data  = ent + nexts * (uint32_t)sizeof(*xf);
		if (data > vlen)
			break;
		for (pos = 0; pos < nexts; pos++) {
			xf = (struct apfs_x_field *)((uint8_t *)iv + ent +
			    pos * sizeof(*xf));
			if (xf->xf_size > vlen - data)
				break;
			if (xf->xf_type == APFS_INO_EXT_TYPE_DSTREAM &&
			    xf->xf_size >= sizeof(*ds)) {
				ds = (struct apfs_dstream *)((uint8_t *)iv +
				    data);
				ds->ds_size          = new_size;
				ds->ds_alloced_size  = alloced +
				    blocks * APFS_BLOCK_SIZE;
				rv = FS_APFS_E_OK;
			}
			data += ((uint32_t)xf->xf_size + 7u) & ~7u;
			if (data > vlen)
				break;
		}
		break;
	}
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: inode %llu has no dstream to lengthen\n",
		    (unsigned long long)ino);
		goto give_back;
	}

	if (blocks != 0 && merge) {
		merge_n++;
		rv = extref_extend(last.el_phys, blocks, xid, node);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the run at %llu grew but the extent "
			    "reference tree still calls it %llu blocks "
			    "(%d)\n",
			    (unsigned long long)last.el_phys,
			    (unsigned long long)(last.el_len / APFS_BLOCK_SIZE),
			    rv);
			goto broken;
		}
	} else if (blocks != 0) {
		rv = extref_insert(first, blocks, ino, xid, node);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the file grew but the extent reference "
			    "tree does not know who owns %llu (%d)\n",
			    (unsigned long long)first, rv);
			goto broken;
		}
	}

	/*
	 * The volume owns more blocks than it did, and it says so itself -- a
	 * count apfsck recomputes from the extents it can reach.  Set before
	 * the spine copies the superblock that carries it, because that copy is
	 * the only chance to write it.
	 */
	g_apfs.ac_fs_alloc_count += blocks;

	/*
	 * And every leaf that changed, with the root whose record count an
	 * insert moved.  A merge adds no record, so it moves no count.
	 */
	rv = edit_commit(&ne, (blocks != 0 && !merge) ? 1 : 0, xid, node);
	if (rv != FS_APFS_E_OK)
		goto broken;
	edit_free(&ne);
	kfree(node);
	return (FS_APFS_E_OK);

give_back:
	if (blocks != 0)
		(void)free_blocks(first, (uint32_t)blocks);
out:
	edit_free(&ne);
	kfree(node);
	return (rv);

broken:
	kprintf("apfs: growing inode %llu failed part way (%d) -- this "
	    "checkpoint must not be written\n", (unsigned long long)ino, rv);
	edit_free(&ne);
	kfree(node);
	return (rv);
}

/*
 * And the same, with one retry behind a split.
 *
 * Split first and grow afterwards, rather than splitting halfway through: a
 * split moves the leaf, the root and the object map, so every address the
 * grow had worked out is stale the moment it happens.  Starting over is a
 * wasted allocation and a re-walk, once, against a page of state to unpick.
 *
 * Once and not in a loop.  A single record cannot need two splits to fit, so
 * a second refusal means something other than a full node -- and a writer that
 * kept splitting in the hope it would help would turn that into a tree full of
 * half-empty nodes rather than an error message.
 */
int
fs_apfs_grow(uint64_t ino, uint64_t id, uint64_t new_size)
{
	uint8_t		*scratch;
	uint64_t	 full;
	int		 rv;

	full = 0;
	rv = grow_once(ino, id, new_size, &full);
	if (rv != FS_APFS_E_NOALLOC || full == 0)
		return (rv);

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL)
		return (FS_APFS_E_NOMEM);
	rv = node_split_at(full, 0, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);

	full = 0;
	return (grow_once(ino, id, new_size, &full));
}

/*
 * The records a truncation has to touch: every one whose run reaches past
 * where the file is about to end.
 *
 * Bounded, because the collecting happens inside a tree walk and the walk's
 * callback has nowhere to allocate from.  A file with more runs than this past
 * its new end is refused out loud rather than shortened halfway.
 */
#define	APFS_TRUNC_MAX	8

/* Every one of them may be in a leaf of its own, and the inode in one more. */
_Static_assert(APFS_TRUNC_MAX + 1 <= APFS_EDIT_LEAVES,
    "a truncate can touch more leaves than one edit can hold");

struct extent_cut {
	uint64_t	ec_id;			/* the dstream being cut  */
	uint64_t	ec_keep;		/* bytes of run to retain */
	uint64_t	ec_logical[APFS_TRUNC_MAX];
	uint64_t	ec_len[APFS_TRUNC_MAX];
	uint64_t	ec_phys[APFS_TRUNC_MAX];
	uint64_t	ec_bno[APFS_TRUNC_MAX];
	uint32_t	ec_n;
	bool		ec_over;
};

static bool
extent_cut_pick(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	const struct apfs_file_extent_val	*fe;
	struct extent_cut			*ec;
	uint64_t				 logical;
	uint64_t				 len;

	ec = arg;
	if (type != APFS_TYPE_FILE_EXTENT || oid != ec->ec_id)
		return (true);
	if (klen < 16 || vlen < sizeof(*fe))
		return (true);
	logical = *(const uint64_t *)(key + 8);
	fe      = (const struct apfs_file_extent_val *)val;
	len     = fe->fe_len_and_flags & APFS_FILE_EXTENT_LEN_MASK;
	if (logical + len <= ec->ec_keep)
		return (true);
	if (ec->ec_n >= APFS_TRUNC_MAX) {
		ec->ec_over = true;
		return (false);
	}
	ec->ec_logical[ec->ec_n] = logical;
	ec->ec_len[ec->ec_n]     = len;
	ec->ec_phys[ec->ec_n]    = fe->fe_phys_block_num;
	ec->ec_bno[ec->ec_n]     = bno;
	ec->ec_n++;
	return (true);
}

/*
 * Make a file shorter.
 *
 * The mirror of fs_apfs_grow, and the ladder it climbs was measured against
 * apfsck on a copy of this container before a line of it was written.  Every
 * rung is an obligation, and the checker names the one that is missing:
 *
 *	the file's own extent record and the length in its inode
 *		-- and nothing complains yet, which is the trap
 *	...and the record in the extent reference tree
 *		"Physical extent record: bad reference count"
 *	...and apfs_fs_alloc_count, the volume's own block count
 *		"Volume superblock: bad block count"
 *	...and the blocks themselves, given back
 *		"Space manager: bad allocation bitmap"
 *	...and, for a record removed outright, its key and value bytes
 *	   threaded onto the node's free lists
 *		"B-tree: wrong free space total for key area"
 *
 * The first rung is the one worth remembering: a truncate that shortens only
 * the file's own record leaves a container the checker still calls valid on
 * three of its five questions, and a reader still reads the file correctly.
 * The damage is entirely in what nobody is asked.
 *
 * The blocks go to the free QUEUE rather than straight back to the bitmap, and
 * this is what the queue is for: checkpoints still on the platter name the
 * leaf this one is about to replace, and that leaf still names these blocks.
 * Clearing their bits now would let the next allocation overwrite bytes an
 * older checkpoint promises.  fq_release hands them back when no checkpoint
 * that could still be read names them.
 *
 * Cutting inside a block costs nothing but a length.  A file of 12235 bytes
 * shortened to 12000 keeps all three of its blocks, because its allocation was
 * always the size rounded up to a block and still is; that falls out of
 * rounding the new size up here rather than being a case anybody wrote.
 */
int
fs_apfs_truncate(uint64_t ino, uint64_t id, uint64_t new_size)
{
	struct apfs_dstream		*ds;
	struct apfs_inode_val		*iv;
	struct apfs_xf_blob		*blob;
	struct apfs_x_field		*xf;
	struct btree_layout		 bl;
	struct extent_cut		 ec;
	struct inode_info		 ii;
	struct inode_locate		 il;
	struct leaf_edit		 ne;
	uint8_t				*node;
	uint8_t				*leaf;
	const uint8_t			*k;
	uint64_t			 hold[APFS_TRUNC_MAX];	/* bytes kept */
	uint64_t			 gone[APFS_TRUNC_MAX];	/* first block */
	uint64_t			 ngone[APFS_TRUNC_MAX];	/* ...how many */
	uint64_t			 keep;
	uint64_t			 freed;
	uint64_t			 raw;
	uint64_t			 xid;
	uint64_t			 ino_leaf;
	uint32_t			 slot[APFS_TRUNC_MAX];	/* ...whose leaf */
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 ent, data, nexts;
	uint32_t			 dropped;
	uint32_t			 pos;
	uint32_t			 i;
	int				 rv;
	bool				 stopped;
	bool				 shed;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);
	if (inode_info(ino, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	if (new_size >= ii.ii_size)
		return (FS_APFS_E_OK);

	keep = (new_size + APFS_BLOCK_SIZE - 1) &
	    ~(uint64_t)(APFS_BLOCK_SIZE - 1);
	xid  = g_apfs.ac_xid + 1;

	ec.ec_id   = id;
	ec.ec_keep = keep;
	ec.ec_n    = 0;
	ec.ec_over = false;
	stopped    = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, extent_cut_pick, &ec, 0,
	    &stopped))
		return (FS_APFS_E_IO);
	if (ec.ec_over) {
		kprintf("apfs: inode %llu has more than %u runs past %llu -- "
		    "cutting that many at once is a different rung\n",
		    (unsigned long long)ino, (unsigned)APFS_TRUNC_MAX,
		    (unsigned long long)new_size);
		return (FS_APFS_E_NOALLOC);
	}

	il.il_oid   = ino;
	il.il_bno   = 0;
	il.il_found = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0,
	    &stopped))
		return (FS_APFS_E_IO);
	if (!il.il_found)
		return (FS_APFS_E_NOTFOUND);
	ino_leaf = il.il_bno;

	/*
	 * WHICH LEAVES THIS TOUCHES, worked out before anything is changed.
	 *
	 * A file's records look like neighbours -- the inode and every extent
	 * share an object id -- and they are, in key order.  That is not the
	 * same as being in one node: a split falls where the tree needs it to,
	 * not where a file would like it to, and the more a volume is used the
	 * likelier it is that a file's bytes and its length end up in different
	 * leaves.  This used to refuse that outright, which was honest while
	 * only one node could be edited at a time, and stopped being tolerable
	 * the moment a shell could redirect into a file: `>` on an existing
	 * file is a truncate.
	 */
	edit_init(&ne);
	for (i = 0; i < ec.ec_n; i++) {
		slot[i] = edit_leaf(&ne, ec.ec_bno[i]);
		if (slot[i] == APFS_EDIT_LEAVES) {
			kprintf("apfs: inode %llu keeps its runs in more than "
			    "%u leaves -- cutting that many at once is a "
			    "different rung\n", (unsigned long long)ino,
			    (unsigned)APFS_EDIT_LEAVES);
			return (FS_APFS_E_SPREAD);
		}
	}
	if (edit_leaf(&ne, ino_leaf) == APFS_EDIT_LEAVES) {
		kprintf("apfs: inode %llu has no room left in this edit for "
		    "its own record's leaf\n", (unsigned long long)ino);
		return (FS_APFS_E_SPREAD);
	}

	/*
	 * And every block about to be given back has to be in a chunk this
	 * kernel can reach, asked NOW rather than when the giving back happens:
	 * by then the leaf has moved and there is nothing left to refuse.
	 */
	freed = 0;
	for (i = 0; i < ec.ec_n; i++) {
		hold[i]  = (ec.ec_logical[i] < keep) ?
		    keep - ec.ec_logical[i] : 0;
		gone[i]  = ec.ec_phys[i] + hold[i] / APFS_BLOCK_SIZE;
		ngone[i] = (ec.ec_len[i] - hold[i]) / APFS_BLOCK_SIZE;
		freed   += ngone[i];
		if (ngone[i] == 0 || chunk_for(gone[i]) != NULL)
			continue;
		kprintf("apfs: the %llu blocks at %llu are in a chunk this "
		    "kernel does not hold -- cannot give them back\n",
		    (unsigned long long)ngone[i], (unsigned long long)gone[i]);
		return (FS_APFS_E_NOALLOC);
	}

	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (FS_APFS_E_NOMEM);
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The records, each in the copy of the leaf it lives in.  Each is found
	 * by its KEY and not by a slot number remembered from the walk: a
	 * delete slides every entry after it down one, and a second pass
	 * trusting the old numbering would shorten whatever had moved into the
	 * place.
	 */
	dropped = 0;
	for (i = 0; i < ec.ec_n; i++) {
		struct apfs_file_extent_val	*fe;

		leaf = ne.le_node[slot[i]];
		btree_layout(leaf, &bl);
		rv = FS_APFS_E_NOTFOUND;
		for (pos = 0; pos < bl.bl_nkeys; pos++) {
			btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);
			if (klen < 16 || vlen < sizeof(*fe))
				continue;
			k   = bl.bl_keys + koff;
			raw = *(const uint64_t *)k;
			if ((raw & APFS_J_OBJ_ID_MASK) !=
			    (id & APFS_J_OBJ_ID_MASK))
				continue;
			if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) !=
			    APFS_TYPE_FILE_EXTENT)
				continue;
			if (*(const uint64_t *)(k + 8) != ec.ec_logical[i])
				continue;
			if (hold[i] == 0) {
				rv = leaf_delete(leaf, pos);
				if (rv != FS_APFS_E_OK)
					goto out;
				dropped++;
			} else {
				fe = (struct apfs_file_extent_val *)
				    (bl.bl_vals - voff);
				fe->fe_len_and_flags =
				    (fe->fe_len_and_flags &
				    ~APFS_FILE_EXTENT_LEN_MASK) | hold[i];
				short_n++;
				rv = FS_APFS_E_OK;
			}
			break;
		}
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the run at file offset %llu is gone -- "
			    "cannot shorten it\n",
			    (unsigned long long)ec.ec_logical[i]);
			goto out;
		}
	}

	/* And the length, in the inode record, wherever that lives. */
	leaf = ne.le_node[edit_leaf(&ne, ino_leaf)];
	btree_layout(leaf, &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		k   = bl.bl_keys + koff;
		raw = *(const uint64_t *)k;
		if ((raw & APFS_J_OBJ_ID_MASK) != ino)
			continue;
		if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_INODE)
			continue;
		iv = (struct apfs_inode_val *)(bl.bl_vals - voff);
		if (vlen < sizeof(*iv) + sizeof(*blob))
			break;
		blob  = (struct apfs_xf_blob *)((uint8_t *)iv + sizeof(*iv));
		nexts = blob->xb_num_exts;
		ent   = (uint32_t)(sizeof(*iv) + sizeof(*blob));
		data  = ent + nexts * (uint32_t)sizeof(*xf);
		if (data > vlen)
			break;
		for (pos = 0; pos < nexts; pos++) {
			xf = (struct apfs_x_field *)((uint8_t *)iv + ent +
			    pos * sizeof(*xf));
			if (xf->xf_size > vlen - data)
				break;
			if (xf->xf_type == APFS_INO_EXT_TYPE_DSTREAM &&
			    xf->xf_size >= sizeof(*ds)) {
				ds = (struct apfs_dstream *)((uint8_t *)iv +
				    data);
				ds->ds_size         = new_size;
				ds->ds_alloced_size = keep;
				rv = FS_APFS_E_OK;
			}
			data += ((uint32_t)xf->xf_size + 7u) & ~7u;
			if (data > vlen)
				break;
		}
		break;
	}
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: inode %llu has no dstream to shorten\n",
		    (unsigned long long)ino);
		goto out;
	}

	drop_n += dropped;

	/* What the runs are now, in the tree that answers for the blocks. */
	for (i = 0; i < ec.ec_n; i++) {
		if (ngone[i] == 0)
			continue;
		shed = false;
		rv = extref_shrink(ec.ec_phys[i], hold[i] / APFS_BLOCK_SIZE,
		    xid, node, &shed);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the run at %llu lost %llu blocks but "
			    "the extent reference tree still calls it %llu "
			    "(%d)\n", (unsigned long long)ec.ec_phys[i],
			    (unsigned long long)ngone[i],
			    (unsigned long long)(ec.ec_len[i] /
			    APFS_BLOCK_SIZE), rv);
			goto broken;
		}
	}

	/* The blocks, to the queue that knows when they are really free. */
	for (i = 0; i < ec.ec_n; i++) {
		if (ngone[i] == 0)
			continue;
		rv = free_blocks(gone[i], (uint32_t)ngone[i]);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the %llu blocks at %llu will not go "
			    "back (%d)\n", (unsigned long long)ngone[i],
			    (unsigned long long)gone[i], rv);
			goto broken;
		}
	}

	/*
	 * The volume owns fewer blocks than it did, and it says so itself.
	 * Set before the spine copies the superblock that carries it, because
	 * that copy is the only chance to write it -- the same order, and the
	 * same reason, as growing.
	 */
	g_apfs.ac_fs_alloc_count -= freed;

	/*
	 * And every leaf that changed, in one transaction with the root whose
	 * record count a delete moved.  Last, because everything above it can
	 * still refuse: until this call nothing about the file's records has
	 * reached the disk.
	 */
	rv = edit_commit(&ne, -(int64_t)dropped, xid, node);
	if (rv != FS_APFS_E_OK)
		goto broken;
	kprintf("apfs: inode %llu cut to %llu bytes -- %u run(s) shortened, "
	    "%u dropped, %llu block(s) queued for release, %u leaf(s) moved\n",
	    (unsigned long long)ino, (unsigned long long)new_size,
	    (unsigned)(ec.ec_n - dropped), (unsigned)dropped,
	    (unsigned long long)freed, (unsigned)ne.le_n);
	edit_free(&ne);
	kfree(node);
	return (FS_APFS_E_OK);

out:
	edit_free(&ne);
	kfree(node);
	return (rv);

broken:
	kprintf("apfs: cutting inode %llu failed part way (%d) -- this "
	    "checkpoint must not be written\n", (unsigned long long)ino, rv);
	edit_free(&ne);
	kfree(node);
	return (rv);
}

/* ---- names --------------------------------------------------------------- */

/*
 * A DIRECTORY ENTRY HAS TO GO WHERE THE VOLUME ALREADY PUTS THEM
 *
 * The reader never had to know how.  It descends on the object id -- the
 * primary sort key, the same on every volume -- and compares names once it is
 * in the right directory, which is exact and hash-independent.  A writer has no
 * such luxury: a new entry must sort where an implementation that DOES hash
 * would have put it, or what comes out is a volume only this kernel can read.
 *
 * So the hash had to be recovered, and it was recovered from the container
 * rather than from the specification: CRC-32C over the name's code points, each
 * written out as four little-endian bytes, case-folded, started at all ones and
 * taken without a final complement, of which the low 22 bits are kept.  All
 * twenty-six names already in this volume come out right.
 *
 * The near miss is the part worth keeping.  The same computation WITHOUT case
 * folding reproduces twenty-five of the twenty-six: every name in the container
 * is lower case except one, "Cellar", and that single directory is the entire
 * evidence separating the two candidates.  A writer built on the wrong one
 * would misplace mixed-case names only, which is exactly the kind of wrong that
 * survives a test suite.
 *
 * Folding is ASCII, and anything else is refused out loud.  Doing it the way
 * Apple does means normalising to NFD and folding through the full Unicode
 * tables, neither of which this kernel carries -- and a guess at them would be
 * silently wrong rather than absent, which is the worse of the two.
 */
static uint32_t
crc32c(uint32_t crc, const uint8_t *p, uint32_t n)
{
	uint32_t	i;

	while (n-- > 0) {
		crc ^= *p++;
		for (i = 0; i < 8; i++) {
			if ((crc & 1u) != 0)
				crc = (crc >> 1) ^ 0x82F63B78u;
			else
				crc >>= 1;
		}
	}
	return (crc);
}

/*
 * Build the key a directory entry sorts under: the parent's object id with the
 * record type on top, then either the hash-and-length word or a bare length,
 * then the name and the NUL that the recorded length counts.
 */
static int
drec_key(uint64_t parent, const char *name, uint32_t nlen, uint8_t *out,
    uint32_t *klen_out)
{
	uint8_t		wide[4];
	uint32_t	crc;
	uint32_t	i;
	uint8_t		c;

	*(uint64_t *)out = (parent & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_DIR_REC << APFS_J_OBJ_TYPE_SHIFT);
	if (!g_apfs.ac_drec_hashed) {
		*(uint16_t *)(out + 8) = (uint16_t)(nlen + 1u);
		mem_copy(out + 10, (const uint8_t *)name, nlen);
		out[10 + nlen] = '\0';
		*klen_out = 10u + nlen + 1u;
		return (FS_APFS_E_OK);
	}

	crc = 0xFFFFFFFFu;
	for (i = 0; i < nlen; i++) {
		c = (uint8_t)name[i];
		if (c >= 0x80u) {
			kprintf("apfs: \"%s\" is not ASCII -- folding a name "
			    "the way this volume hashes them needs Unicode "
			    "tables this kernel does not carry\n", name);
			return (FS_APFS_E_INVAL);
		}
		if (c >= 'A' && c <= 'Z')
			c = (uint8_t)(c + ('a' - 'A'));
		wide[0] = c;
		wide[1] = 0;
		wide[2] = 0;
		wide[3] = 0;
		crc = crc32c(crc, wide, (uint32_t)sizeof(wide));
	}
	*(uint32_t *)(out + 8) =
	    ((crc & APFS_DREC_HASH_BITS) << APFS_DREC_HASH_SHIFT) |
	    ((nlen + 1u) & APFS_DREC_LEN_MASK);
	mem_copy(out + 12, (const uint8_t *)name, nlen);
	out[12 + nlen] = '\0';
	*klen_out = 12u + nlen + 1u;
	return (FS_APFS_E_OK);
}

/*
 * A NODE OF THE FILE-SYSTEM TREE, INTO THE CHECKPOINT BEING BUILT
 *
 * Not cow_physical, which is for objects whose oid IS their block: a tree node
 * is VIRTUAL, its oid is a name the object map resolves, and the copy keeps
 * that name.  Every writer here had this open-coded; a create moves three nodes
 * at once and open-coding it a third time is where it stops being a shape and
 * starts being a function.
 */
static int
node_cow(uint8_t *node, uint64_t old_bno, uint64_t xid, uint64_t *new_bno)
{
	struct apfs_obj_phys	*o;
	int			 rv;

	rv = alloc_blocks(1, old_bno, new_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	o = (struct apfs_obj_phys *)node;
	o->o_xid = xid;
	rv = fs_apfs_write_block(*new_bno, node);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(*new_bno, 1);
		return (rv);
	}
	rv = free_blocks(old_bno, 1);
	if (rv != FS_APFS_E_OK)
		return (rv);
	cow_n_spine++;
	return (FS_APFS_E_OK);
}

/*
 * Find a record by key in a node the caller is holding, and say where it is.
 *
 * By KEY and not by a slot remembered from a walk: an insert or a delete slides
 * everything after it, and a second pass trusting the old numbering would edit
 * whatever had moved into the place.  That has its own comment in the truncate
 * above because it was learned there.
 */
static bool
node_slot(const uint8_t *node, uint64_t oid, uint32_t type, uint32_t *pos_out,
    uint32_t *voff_out, uint32_t *vlen_out)
{
	struct btree_layout	bl;
	uint64_t		raw;
	uint32_t		koff, klen, voff, vlen;
	uint32_t		i;

	btree_layout(node, &bl);
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		if (klen < 8)
			continue;
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw & APFS_J_OBJ_ID_MASK) != (oid & APFS_J_OBJ_ID_MASK))
			continue;
		if ((uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) != type)
			continue;
		*pos_out  = i;
		*voff_out = voff;
		*vlen_out = vlen;
		return (true);
	}
	return (false);
}

/* Where a key belongs, in a node the caller is holding. */
static uint32_t
node_place(const uint8_t *node, const uint8_t *key, uint32_t klen)
{
	struct btree_layout	bl;
	uint32_t		koff, klen2, voff, vlen;
	uint32_t		i;

	btree_layout(node, &bl);
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen2, &voff, &vlen);
		if (jkey_cmp(bl.bl_keys + koff, klen2, key, klen) > 0)
			return (i);
	}
	return (bl.bl_nkeys);
}

/*
 * The number of children a directory's inode record says it has, edited in a
 * node the caller is holding, together with the two times that change when a
 * directory gains or loses a name.
 */
static int
dir_children_add(uint8_t *node, uint64_t dir, int32_t delta, uint64_t now)
{
	struct apfs_inode_val	*iv;
	struct btree_layout	 bl;
	uint32_t		 pos, voff, vlen;

	if (!node_slot(node, dir, APFS_TYPE_INODE, &pos, &voff, &vlen))
		return (FS_APFS_E_NOTFOUND);
	btree_layout(node, &bl);
	if (vlen < sizeof(*iv))
		return (FS_APFS_E_INVAL);
	iv = (struct apfs_inode_val *)((uint8_t *)bl.bl_vals - voff);
	if (iv->ai_nchildren_or_nlink + delta < 0)
		return (FS_APFS_E_INVAL);
	iv->ai_nchildren_or_nlink += delta;
	iv->ai_mod_time    = now;
	iv->ai_change_time = now;
	return (FS_APFS_E_OK);
}

/*
 * MAKING AND UNMAKING A NAME
 *
 * The ladder was measured against apfsck on a copy of this container before a
 * line of either was written, and the two are not symmetric.  Leaving one
 * obligation out at a time, on an otherwise complete edit:
 *
 *	CREATE, leaving out		apfsck answers
 *	  apfs_next_obj_id		"Inode record: free inode number in use"
 *	  the directory entry		"Inode record: wrong directory child count"
 *	  the inode record		"Inode record: wrong link count"
 *	  the dstream id record		"Data stream: missing reference count"
 *	  the parent's child count	"Inode record: wrong directory child count"
 *	  the tree's key count		"Catalog: wrong key count in info footer"
 *	  apfs_num_files		nothing at all
 *
 *	UNLINK, leaving out
 *	  the entry, or the parent's count	as above
 *	  the inode and dstream records	"Inode record: wrong link count"
 *	  the tree's key count		"Catalog: wrong key count in info footer"
 *	  the extent reference record	"Physical extent record: bad reference count"
 *	  apfs_fs_alloc_count		"Volume superblock: bad block count"
 *	  the blocks, given back	"Space manager: bad allocation bitmap"
 *	  the deleted bytes, threaded	"B-tree: wrong free space total for key area"
 *	  apfs_num_files		nothing at all
 *
 * Two of those are worth remembering.  The first thing the checker notices
 * about a created file is not the file: it is that the volume still calls the
 * number free, and that one complaint MASKS every other -- a cumulative ladder
 * built in the obvious order says the same thing at every rung and teaches
 * nothing.  And apfs_num_files, the count that looks most like the thing a
 * create ought to be updating, is never checked in either direction.  It is
 * updated anyway, for the same reason the truncate updated what nobody asked
 * about: something other than this checker will read it.
 *
 * The unlink half of the ladder is the truncate's ladder with three rungs on
 * top, and that is not a coincidence -- unlink DOES a truncate.  Cutting the
 * file to nothing is what gives back its blocks, its extent records and its
 * ownership records, all of which are already written and already proven; what
 * is left over is three records and two counters.
 */

/* Longest name this kernel will make, as against the 255 it will read. */
#define	APFS_MAKE_NAME_MAX	64

/*
 * Put a name into a directory, and an empty file under it.
 *
 * The file has a dstream from the moment it exists, holding no bytes and no
 * blocks.  That is not the same as having none: an inode without one names
 * something with no length at all, and every path that makes a file longer
 * looks for a length to move.  Creating without one would produce a file that
 * could be opened, read as empty, and never written to.
 *
 * A leaf with no room refuses, out loud, and this does NOT split and retry the
 * way growing a file does.  That is the honest edge and not an oversight: a
 * create puts records into two leaves at once, a split moves both of them and
 * everything above, and every address worked out below would be stale --
 * fs_apfs_grow can start over because it has one leaf to reconsider.
 */
int
fs_apfs_create(uint64_t dir, const char *name, uint64_t now, uint64_t *ino_out)
{
	struct apfs_inode_val	*iv;
	struct apfs_xf_blob	*blob;
	struct apfs_x_field	*xf;
	struct apfs_drec_val	 dv;
	struct dirent_search	 ds;
	struct inode_info	 ii;
	struct inode_locate	 il;
	struct leaf_find	 lf;
	struct leaf_edit	 ne;
	uint8_t			 dkey[12 + APFS_MAKE_NAME_MAX + 1];
	uint8_t			 rec[sizeof(struct apfs_inode_val) +
				     sizeof(struct apfs_xf_blob) +
				     2 * sizeof(struct apfs_x_field) +
				     APFS_MAKE_NAME_MAX + 8 +
				     sizeof(struct apfs_dstream)];
	uint8_t			*scratch;
	uint64_t		 ikey;
	uint64_t		 skey;
	uint64_t		 ino;
	uint64_t		 par_leaf;
	uint64_t		 drec_leaf;
	uint64_t		 ino_leaf;
	uint32_t		 refs;
	uint32_t		 dklen;
	uint32_t		 vlen;
	uint32_t		 nlen;
	uint32_t		 data;
	uint32_t		 pad;
	uint32_t		 slot;
	int			 rv;
	bool			 stopped;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	nlen = (uint32_t)str_len(name);
	if (nlen == 0 || nlen > APFS_MAKE_NAME_MAX) {
		kprintf("apfs: a name of %u bytes is not one this kernel will "
		    "make -- the limit is %u\n", (unsigned)nlen,
		    (unsigned)APFS_MAKE_NAME_MAX);
		return (FS_APFS_E_INVAL);
	}
	if (inode_info(dir, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	if ((ii.ii_mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		kprintf("apfs: inode %llu is not a directory -- nothing can be "
		    "made in it\n", (unsigned long long)dir);
		return (FS_APFS_E_NOTFOUND);
	}

	/* And the name must be free, which is a question only the tree can answer. */
	ds.ds_name    = name;
	ds.ds_namelen = nlen;
	ds.ds_parent  = dir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, dirent_match, &ds, 0, &stopped))
		return (FS_APFS_E_IO);
	if (ds.ds_found != 0)
		return (FS_APFS_E_EXIST);

	ino = g_apfs.ac_next_ino;
	rv  = drec_key(dir, name, nlen, dkey, &dklen);
	if (rv != FS_APFS_E_OK)
		return (rv);
	ikey = (ino & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_INODE << APFS_J_OBJ_TYPE_SHIFT);
	skey = (ino & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_DSTREAM_ID << APFS_J_OBJ_TYPE_SHIFT);

	dv.dv_file_id    = ino;
	dv.dv_date_added = now;
	dv.dv_flags      = APFS_DT_REG;
	refs             = 1;

	/*
	 * The inode record: a fixed part, then the two extended fields a file
	 * has, in ascending order of type, then their data each padded up to a
	 * multiple of eight.  Both the order and the padding were read off the
	 * inodes already in this volume rather than taken from the layout.
	 */
	mem_zero(rec, (uint32_t)sizeof(rec));
	pad = (nlen + 1u + 7u) & ~7u;
	iv  = (struct apfs_inode_val *)rec;
	iv->ai_parent_id          = dir;
	iv->ai_private_id         = ino;
	iv->ai_create_time        = now;
	iv->ai_mod_time           = now;
	iv->ai_change_time        = now;
	iv->ai_access_time        = now;
	iv->ai_internal_flags     = APFS_INODE_NO_RSRC_FORK;
	iv->ai_nchildren_or_nlink = 1;
	iv->ai_mode               = APFS_S_IFREG | 0644;
	blob = (struct apfs_xf_blob *)(rec + sizeof(*iv));
	blob->xb_num_exts  = 2;
	blob->xb_used_data =
	    (uint16_t)(pad + sizeof(struct apfs_dstream));
	xf = (struct apfs_x_field *)(rec + sizeof(*iv) + sizeof(*blob));
	xf[0].xf_type  = APFS_INO_EXT_TYPE_NAME;
	xf[0].xf_flags = APFS_XF_DO_NOT_COPY;
	xf[0].xf_size  = (uint16_t)(nlen + 1u);
	xf[1].xf_type  = APFS_INO_EXT_TYPE_DSTREAM;
	xf[1].xf_flags = APFS_XF_SYSTEM_FIELD;
	xf[1].xf_size  = (uint16_t)sizeof(struct apfs_dstream);
	data = (uint32_t)(sizeof(*iv) + sizeof(*blob) + 2 * sizeof(*xf));
	mem_copy(rec + data, (const uint8_t *)name, nlen);
	/* The NUL, the padding and the empty dstream are already zero. */
	vlen = data + pad + (uint32_t)sizeof(struct apfs_dstream);

	/* Where each of the three belongs. */
	il.il_oid   = dir;
	il.il_bno   = 0;
	il.il_found = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!il.il_found)
		return (FS_APFS_E_NOTFOUND);
	par_leaf = il.il_bno;

	lf.lf_key   = dkey;
	lf.lf_klen  = dklen;
	lf.lf_bno   = 0;
	lf.lf_first = 0;
	lf.lf_any   = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_find, &lf, 0, &stopped))
		return (FS_APFS_E_IO);
	drec_leaf = lf.lf_bno != 0 ? lf.lf_bno : lf.lf_first;

	lf.lf_key   = (const uint8_t *)&ikey;
	lf.lf_klen  = (uint32_t)sizeof(ikey);
	lf.lf_bno   = 0;
	lf.lf_first = 0;
	lf.lf_any   = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_find, &lf, 0, &stopped))
		return (FS_APFS_E_IO);
	ino_leaf = lf.lf_bno != 0 ? lf.lf_bno : lf.lf_first;

	edit_init(&ne);
	(void)edit_leaf(&ne, par_leaf);
	(void)edit_leaf(&ne, drec_leaf);
	(void)edit_leaf(&ne, ino_leaf);
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * Every edit, in memory.  The inode record and its dstream id go in
	 * together because their keys are adjacent -- same object, neighbouring
	 * types -- so a leaf that holds one holds the other.
	 */
	slot = edit_leaf(&ne, drec_leaf);
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], dkey, dklen), dkey, dklen, &dv,
	    (uint32_t)sizeof(dv));
	if (rv != FS_APFS_E_OK)
		goto out;

	slot = edit_leaf(&ne, ino_leaf);
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], (const uint8_t *)&ikey,
	    (uint32_t)sizeof(ikey)), &ikey, (uint32_t)sizeof(ikey), rec, vlen);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], (const uint8_t *)&skey,
	    (uint32_t)sizeof(skey)), &skey, (uint32_t)sizeof(skey), &refs,
	    (uint32_t)sizeof(refs));
	if (rv != FS_APFS_E_OK)
		goto out;

	slot = edit_leaf(&ne, par_leaf);
	rv = dir_children_add(ne.le_node[slot], dir, 1, now);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The volume's own claims, set before the spine copies the superblock
	 * that carries them -- the same ordering, and the same reason, as the
	 * block count in every writer above.
	 */
	g_apfs.ac_next_ino  = ino + 1;
	g_apfs.ac_num_files += 1;

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto undo;
	}
	rv = edit_commit(&ne, 3, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		if (!edit_moved(&ne)) {
			kprintf("apfs: making \"%s\" was refused before anything "
			    "moved (%d) -- the volume is as it was\n", name, rv);
			goto undo;
		}
		kprintf("apfs: making \"%s\" failed after a leaf had moved "
		    "(%d) -- this checkpoint must not be written\n", name, rv);
		goto out;
	}

	make_n++;
	if (ino_out != NULL)
		*ino_out = ino;
	kprintf("apfs: \"%s\" made in inode %llu as inode %llu -- %u leaves "
	    "moved\n", name, (unsigned long long)dir, (unsigned long long)ino,
	    (unsigned)ne.le_n);
	edit_free(&ne);
	return (FS_APFS_E_OK);

undo:
	g_apfs.ac_next_ino   = ino;
	g_apfs.ac_num_files -= 1;
out:
	edit_free(&ne);
	return (rv);
}

/*
 * And take a name back out, with the file under it when nothing else holds it.
 *
 * The blocks are not this function's problem: cutting the file to nothing is,
 * and that is a call to the truncate above, which already gives back the runs,
 * the records in both trees that name them and the volume's block count.  What
 * is left is the three records the create made and the two counters it moved.
 */
int
fs_apfs_unlink(uint64_t dir, const char *name, uint64_t now)
{
	struct dirent_search	 ds;
	struct inode_info	 ii;
	struct inode_locate	 il;
	struct leaf_edit	 ne;
	uint8_t			 dkey[12 + APFS_MAKE_NAME_MAX + 1];
	uint8_t			*scratch;
	uint64_t		 child;
	uint64_t		 par_leaf;
	uint64_t		 drec_leaf;
	uint64_t		 ino_leaf;
	uint32_t		 dklen;
	uint32_t		 nlen;
	uint32_t		 pos, voff, vlen;
	uint32_t		 slot;
	int			 rv;
	bool			 stopped;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	nlen = (uint32_t)str_len(name);
	if (nlen == 0 || nlen > APFS_MAKE_NAME_MAX)
		return (FS_APFS_E_INVAL);

	ds.ds_name    = name;
	ds.ds_namelen = nlen;
	ds.ds_parent  = dir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, dirent_match, &ds, 0, &stopped))
		return (FS_APFS_E_IO);
	if (ds.ds_found == 0)
		return (FS_APFS_E_NOTFOUND);
	if (ds.ds_is_dir) {
		kprintf("apfs: \"%s\" is a directory -- taking one of those "
		    "out is a different rung\n", name);
		return (FS_APFS_E_ISDIR);
	}
	child = ds.ds_found;
	if (inode_info(child, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	if (ii.ii_nlink != 1) {
		kprintf("apfs: inode %llu has %u links and this kernel makes "
		    "none -- unlinking one of several is a different rung\n",
		    (unsigned long long)child, (unsigned)ii.ii_nlink);
		return (FS_APFS_E_NOALLOC);
	}

	/*
	 * The bytes first, and through the truncate rather than beside it.  It
	 * moves leaves, so everything below has to be located afterwards.
	 */
	if (ii.ii_size != 0 || ii.ii_alloced != 0) {
		rv = fs_apfs_truncate(child, ii.ii_private_id, 0);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}

	rv = drec_key(dir, name, nlen, dkey, &dklen);
	if (rv != FS_APFS_E_OK)
		return (rv);

	il.il_oid   = dir;
	il.il_bno   = 0;
	il.il_found = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!il.il_found)
		return (FS_APFS_E_NOTFOUND);
	par_leaf = il.il_bno;

	il.il_oid   = child;
	il.il_bno   = 0;
	il.il_found = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!il.il_found)
		return (FS_APFS_E_NOTFOUND);
	ino_leaf = il.il_bno;

	{
		struct leaf_find	lf;

		lf.lf_key   = dkey;
		lf.lf_klen  = dklen;
		lf.lf_bno   = 0;
		lf.lf_first = 0;
		lf.lf_any   = false;
		stopped     = false;
		if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_find, &lf, 0,
		    &stopped))
			return (FS_APFS_E_IO);
		drec_leaf = lf.lf_bno != 0 ? lf.lf_bno : lf.lf_first;
	}

	edit_init(&ne);
	(void)edit_leaf(&ne, par_leaf);
	(void)edit_leaf(&ne, drec_leaf);
	(void)edit_leaf(&ne, ino_leaf);
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The entry, found by its exact key rather than by object and type:
	 * a directory has one record per name and they differ only past the
	 * eighth byte, so "the DIR_REC of this parent" names all of them.
	 */
	slot = edit_leaf(&ne, drec_leaf);
	{
		struct btree_layout	bl;
		uint32_t		koff, klen;
		uint32_t		i;

		btree_layout(ne.le_node[slot], &bl);
		rv = FS_APFS_E_NOTFOUND;
		for (i = 0; i < bl.bl_nkeys; i++) {
			btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
			if (klen != dklen)
				continue;
			if (jkey_cmp(bl.bl_keys + koff, klen, dkey, dklen) != 0)
				continue;
			rv = leaf_delete(ne.le_node[slot], i);
			break;
		}
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: \"%s\" was in inode %llu a moment ago "
			    "and is not in leaf %llu now\n", name,
			    (unsigned long long)dir,
			    (unsigned long long)drec_leaf);
			goto out;
		}
	}

	slot = edit_leaf(&ne, ino_leaf);
	if (node_slot(ne.le_node[slot], child, APFS_TYPE_DSTREAM_ID, &pos,
	    &voff, &vlen)) {
		rv = leaf_delete(ne.le_node[slot], pos);
		if (rv != FS_APFS_E_OK)
			goto out;
	} else {
		/*
		 * A file this kernel made always has one.  One that came off
		 * the image need not, and removing a record that is not there
		 * would take the record after it.
		 */
		kprintf("apfs: inode %llu has no dstream id record -- one "
		    "fewer to take out\n", (unsigned long long)child);
	}
	if (!node_slot(ne.le_node[slot], child, APFS_TYPE_INODE, &pos, &voff,
	    &vlen)) {
		rv = FS_APFS_E_NOTFOUND;
		goto out;
	}
	rv = leaf_delete(ne.le_node[slot], pos);
	if (rv != FS_APFS_E_OK)
		goto out;

	slot = edit_leaf(&ne, par_leaf);
	rv = dir_children_add(ne.le_node[slot], dir, -1, now);
	if (rv != FS_APFS_E_OK)
		goto out;

	g_apfs.ac_num_files -= 1;

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		g_apfs.ac_num_files += 1;
		rv = FS_APFS_E_NOMEM;
		goto out;
	}
	rv = edit_commit(&ne, -3, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		if (!edit_moved(&ne)) {
			g_apfs.ac_num_files += 1;
			kprintf("apfs: unmaking \"%s\" was refused before "
			    "anything moved (%d) -- the volume is as it was\n",
			    name, rv);
			goto out;
		}
		kprintf("apfs: unmaking \"%s\" failed after a leaf had moved "
		    "(%d) -- this checkpoint must not be written\n", name, rv);
		goto out;
	}

	kill_n++;
	kprintf("apfs: \"%s\" taken out of inode %llu -- inode %llu is gone, "
	    "%u leaves moved\n", name, (unsigned long long)dir,
	    (unsigned long long)child, (unsigned)ne.le_n);
	edit_free(&ne);
	return (FS_APFS_E_OK);

out:
	edit_free(&ne);
	return (rv);
}

uint64_t
fs_apfs_makes(void)
{

	return (make_n);
}

uint64_t
fs_apfs_kills(void)
{

	return (kill_n);
}

uint64_t
fs_apfs_holes(void)
{

	return (hole_n);
}

int
fs_apfs_size(uint64_t ino, uint64_t *size_out)
{
	struct inode_info	ii;
	int			rv;

	if (size_out == NULL)
		return (FS_APFS_E_IO);
	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	rv = inode_info(ino, &ii);
	if (rv != FS_APFS_E_OK)
		return (rv);
	*size_out = ii.ii_size;
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
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	const struct apfs_drec_val	*dv;
	struct readdir_search		*rs;
	const char			*name;
	uint32_t			 nlen;
	uint32_t			 i;

	(void)bno;
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

	/*
	 * The ephemeral layer.  Not required to mount -- nothing a file read
	 * touches lives there -- so a container whose checkpoint maps cannot be
	 * read still mounts, and only the accounting goes unanswered.
	 */
	if (read_checkpoint_maps(scratch) == FS_APFS_E_OK) {
		if (read_spaceman(scratch) == FS_APFS_E_OK) {
			/*
			 * The chunk walk needs three blocks live at once --
			 * the space manager, a chunk-info block and a bitmap
			 * -- so it borrows two more for the length of the
			 * walk rather than keeping them for the mount.
			 */
			void	*cib_buf;
			void	*bm_buf;

			cib_buf = kmalloc(APFS_BLOCK_SIZE);
			bm_buf  = kmalloc(APFS_BLOCK_SIZE);
			if (cib_buf != NULL && bm_buf != NULL)
				(void)verify_chunk_bitmaps(scratch, cib_buf,
				    bm_buf);
			else
				kprintf("apfs: no memory for the chunk walk\n");
			kfree(cib_buf);
			kfree(bm_buf);
			/*
			 * After the walk, because the strongest check the
			 * pool can be given is that the two blocks the walk
			 * found are inside it and marked taken in it.
			 */
			(void)ip_load();
		}
	} else
		kprintf("apfs: no readable checkpoint map -- space accounting "
		    "unavailable\n");

	rv = mount_volume(scratch);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: volume 0 unreadable (%d) -- APFS unavailable\n",
		    rv);
		goto out;
	}

	/*
	 * Now that the volume is known, metadata comes from the chunk its own
	 * metadata already lives in.  Before this it was whichever chunk the
	 * bitmap walk met first, which is fine for taking blocks and useless
	 * for keeping a copy near the thing it replaces.
	 */
	if (g_apfs.ac_ip_valid) {
		struct alloc_chunk	*ch;

		ch = chunk_for(g_apfs.ac_root_tree_bno);
		if (ch != NULL) {
			g_home = ch;
			kprintf("apfs: metadata comes from the chunk @%llu that "
			    "holds the volume's own\n",
			    (unsigned long long)ch->ch_base);
		}
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

/* The resident chunk covering `bno`, without trying to bring one in. */
static struct alloc_chunk *
chunk_resident(uint64_t bno)
{
	struct alloc_chunk	*ch;
	uint32_t		 i;

	for (i = 0; i < g_chunk_n; i++) {
		ch = &g_chunk[i];
		if (bno >= ch->ch_base && bno < ch->ch_base + ch->ch_blocks)
			return (ch);
	}
	return (NULL);
}

/*
 * Bring the chunk covering `bno` into memory, or say why not.
 *
 * A wholly free chunk has no bitmap block at all -- the format's way of saying
 * "nothing here is taken" -- and giving it one means allocating a block for it
 * and telling the chunk-info, which is a different operation from this one.
 * Nothing needs it yet: everything this kernel touches is in a chunk that has
 * a bitmap already.
 */
static struct alloc_chunk *
chunk_admit(uint64_t bno)
{
	const struct apfs_chunk_info_block	*cib;
	const struct apfs_chunk_info		*ci;
	struct alloc_chunk			*ch;
	uint32_t				 count;
	uint32_t				 i;

	if (g_cib == NULL)
		return (NULL);
	if (g_chunk_n >= APFS_CHUNKS_RESIDENT) {
		kprintf("apfs: %u chunk bitmaps are held and block %llu wants "
		    "another -- this transaction cannot reach it\n",
		    (unsigned)g_chunk_n, (unsigned long long)bno);
		return (NULL);
	}
	cib = (const struct apfs_chunk_info_block *)g_cib;
	count = cib->cib_chunk_info_count;
	if (count > APFS_CI_MAX_PER_CIB)
		count = APFS_CI_MAX_PER_CIB;

	for (i = 0; i < count; i++) {
		ci = &cib->cib_chunk_info[i];
		if (bno < ci->ci_addr || bno >= ci->ci_addr + ci->ci_block_count)
			continue;
		if (ci->ci_bitmap_addr == 0) {
			kprintf("apfs: chunk @%llu is wholly free and has no "
			    "bitmap -- making one is a different rung\n",
			    (unsigned long long)ci->ci_addr);
			return (NULL);
		}
		if (ci->ci_block_count > APFS_BLOCK_SIZE * 8u) {
			kprintf("apfs: chunk @%llu claims %u blocks, more than "
			    "a bitmap block holds\n",
			    (unsigned long long)ci->ci_addr,
			    (unsigned)ci->ci_block_count);
			return (NULL);
		}
		ch = &g_chunk[g_chunk_n];
		if (ch->ch_bm == NULL)
			ch->ch_bm = kmalloc(APFS_BLOCK_SIZE);
		if (ch->ch_bm == NULL)
			return (NULL);
		/* Raw: a bitmap is bits, with no header to check. */
		if (read_block_raw(ci->ci_bitmap_addr, ch->ch_bm) !=
		    FS_APFS_E_OK) {
			kprintf("apfs: chunk bitmap %llu would not read\n",
			    (unsigned long long)ci->ci_bitmap_addr);
			return (NULL);
		}
		ch->ch_base       = ci->ci_addr;
		ch->ch_bitmap     = ci->ci_bitmap_addr;
		ch->ch_blocks     = ci->ci_block_count;
		ch->ch_slot       = i;
		ch->ch_dirty      = false;
		ch->ch_free_admit = ci->ci_free_count;
		ch->ch_bits_admit = bitmap_free_count(ch->ch_bm,
		    ci->ci_block_count);
		g_chunk_n++;
		chunk_n_admit++;
		kprintf("apfs: holding the chunk @%llu -- %u free of %u, "
		    "bitmap at %llu\n", (unsigned long long)ch->ch_base,
		    (unsigned)ci->ci_free_count, (unsigned)ch->ch_blocks,
		    (unsigned long long)ch->ch_bitmap);
		return (ch);
	}
	kprintf("apfs: no chunk in the chunk-info covers block %llu\n",
	    (unsigned long long)bno);
	return (NULL);
}

/* The chunk covering `bno`, admitting it if it is not already held. */
static struct alloc_chunk *
chunk_for(uint64_t bno)
{
	struct alloc_chunk	*ch;

	ch = chunk_resident(bno);
	return (ch != NULL ? ch : chunk_admit(bno));
}

/*
 * Move `count` blocks between the free and the used state, starting at bit
 * `first` of chunk `ch`: set the bits when `take` is true, clear them when it
 * is false, and move both counters the matching way.
 *
 * COPY-ON-WRITE, and this is where the checkpoint starts paying for itself.
 * The bitmap and the chunk-info block are not written back where they came
 * from; each is written to a block taken from the internal pool, and the
 * space manager -- which lives in memory now -- is pointed at the new one.
 * Until the checkpoint commits, the old pair is still on the platter saying
 * exactly what the live checkpoint believes, so a crash anywhere in here
 * loses the allocation and nothing else.
 *
 * The three edits still have to be correct together -- a bitmap that says a
 * block is taken while the chunk-info counts it free is a container apfsck
 * rejects, in those words -- but they no longer need a transaction to make
 * that so: nothing here is visible until a checkpoint publishes all of it.
 * That is why fs_txn is gone from this path and still used by the one that
 * writes an inode in place.
 *
 * The bitmap goes through the raw path because it has no header to check or
 * to seal; the chunk-info block is a physical object, so its oid is its own
 * block number and it carries the xid of the checkpoint being built.
 *
 * Returns 0, or a negative FS_APFS_E_*.
 */
static int
alloc_bits(struct alloc_chunk *ch, uint32_t first, uint32_t count, bool take)
{
	struct apfs_chunk_info_block	*cib;
	struct apfs_chunk_info		*ci;
	struct apfs_spaceman		*sm;
	uint32_t			 i;
	uint32_t			 bit;

	if (ch == NULL || ch->ch_bm == NULL || g_cib == NULL || g_sm == NULL)
		return (FS_APFS_E_INVAL);

	cib = (struct apfs_chunk_info_block *)g_cib;
	ci  = &cib->cib_chunk_info[ch->ch_slot];
	sm  = sm_mem();
	if (take && (ci->ci_free_count < count ||
	    sm->sm_dev[APFS_SD_MAIN].sm_free_count < count)) {
		kprintf("apfs: alloc: %u blocks wanted, chunk has %u and the "
		    "device %llu\n", (unsigned)count,
		    (unsigned)ci->ci_free_count,
		    (unsigned long long)sm->sm_dev[APFS_SD_MAIN].sm_free_count);
		return (FS_APFS_E_NOALLOC);
	}

	for (i = 0; i < count; i++) {
		bit = first + i;
		/*
		 * Refuse to take a block already taken, or give back one that
		 * was never held.  Either means the caller's idea of the chunk
		 * and the chunk itself have diverged, and carrying on would
		 * put the counters out of step with the bits.
		 */
		if (((ch->ch_bm[bit >> 3] & (uint8_t)(1u << (bit & 7u))) != 0) ==
		    take) {
			kprintf("apfs: alloc: block %llu is already %s\n",
			    (unsigned long long)(ch->ch_base + bit),
			    take ? "taken" : "free");
			return (FS_APFS_E_INVAL);
		}
		if (take)
			ch->ch_bm[bit >> 3] |= (uint8_t)(1u << (bit & 7u));
		else
			ch->ch_bm[bit >> 3] &= (uint8_t)~(1u << (bit & 7u));
	}

	if (take) {
		ci->ci_free_count -= count;
		sm->sm_dev[APFS_SD_MAIN].sm_free_count -= count;
	} else {
		ci->ci_free_count += count;
		sm->sm_dev[APFS_SD_MAIN].sm_free_count += count;
	}
	g_apfs.ac_sm_free = sm->sm_dev[APFS_SD_MAIN].sm_free_count;
	ch->ch_dirty = true;
	return (FS_APFS_E_OK);
}

/* Are these `count` blocks of this chunk all free right now? */
static bool
chunk_run_free(const struct alloc_chunk *ch, uint32_t first, uint32_t count)
{
	uint32_t	i;

	if ((uint64_t)first + count > ch->ch_blocks)
		return (false);
	for (i = 0; i < count; i++) {
		if ((ch->ch_bm[(first + i) >> 3] &
		    (uint8_t)(1u << ((first + i) & 7u))) != 0)
			return (false);
	}
	return (true);
}

/* A run of `count` free blocks in this one chunk, taken, or E_NOALLOC. */
static int
alloc_run_in(struct alloc_chunk *ch, uint32_t count, uint64_t *first_out)
{
	uint32_t	i;
	uint32_t	seen;
	int		rv;

	seen = 0;
	for (i = 0; i < ch->ch_blocks; i++) {
		if ((ch->ch_bm[i >> 3] & (uint8_t)(1u << (i & 7u))) != 0) {
			seen = 0;
			continue;
		}
		if (++seen < count)
			continue;
		rv = alloc_bits(ch, i + 1 - count, count, true);
		if (rv != FS_APFS_E_OK)
			return (rv);
		*first_out = ch->ch_base + (i + 1 - count);
		alloc_n_taken += count;
		return (FS_APFS_E_OK);
	}
	return (FS_APFS_E_NOALLOC);
}

/*
 * Take a run of `count` consecutive blocks, near `near` if that can be
 * arranged, or refuse.  A block waiting in the free queue is still marked in
 * use, so this cannot pick one up.
 *
 * The hint is not cosmetic.  A copy that lands in the chunk it came from is a
 * copy whose release is reachable in the same transaction -- and for a file's
 * bytes it is also what keeps a file's extents from scattering one write at a
 * time.  When the hinted chunk cannot serve, metadata's own chunk is tried,
 * and only then does this fail.
 */
static int
alloc_blocks(uint32_t count, uint64_t near, uint64_t *first_out)
{
	struct alloc_chunk	*ch;
	int			 rv;

	if (!g_apfs.ac_alloc_have || count == 0)
		return (FS_APFS_E_INVAL);

	ch = (near != 0) ? chunk_for(near) : NULL;
	if (ch != NULL) {
		/*
		 * EXACTLY there first, when exactly there is free.  A caller
		 * naming a block usually wants the run to continue from it,
		 * and first-fit gave it to them only four times in six -- the
		 * metadata copies of the same transaction kept taking the
		 * block in between.  Landing on it is the difference between
		 * lengthening a record and adding one.
		 */
		if (near >= ch->ch_base && chunk_run_free(ch,
		    (uint32_t)(near - ch->ch_base), count)) {
			rv = alloc_bits(ch, (uint32_t)(near - ch->ch_base),
			    count, true);
			if (rv == FS_APFS_E_OK) {
				*first_out = near;
				alloc_n_taken += count;
				return (FS_APFS_E_OK);
			}
		}
		rv = alloc_run_in(ch, count, first_out);
		if (rv != FS_APFS_E_NOALLOC)
			return (rv);
	}
	if (g_home == NULL)
		return (FS_APFS_E_INVAL);
	if (ch != g_home) {
		rv = alloc_run_in(g_home, count, first_out);
		if (rv != FS_APFS_E_NOALLOC)
			return (rv);
	}
	kprintf("apfs: no run of %u free blocks in any chunk being held\n",
	    (unsigned)count);
	return (FS_APFS_E_NOALLOC);
}

/*
 * Give a run back: into the device's free queue, keyed by the transaction
 * doing the releasing.  The bits stay set and the counters do not move --
 * the block is not free, it is spoken for by checkpoints that still name it,
 * and fq_release is the only thing that ever makes it free again.
 *
 * The chunk is admitted here rather than at release time so that a run this
 * kernel cannot reach is refused by the caller that still has a choice, not by
 * a checkpoint that has none.
 */
static int
free_blocks(uint64_t first, uint32_t count)
{
	struct alloc_chunk	*ch;

	if (!g_apfs.ac_alloc_have)
		return (FS_APFS_E_INVAL);
	ch = chunk_for(first);
	if (ch == NULL)
		return (FS_APFS_E_INVAL);
	if (first + count > ch->ch_base + ch->ch_blocks) {
		kprintf("apfs: free_blocks(%llu, %u) runs off the end of the "
		    "chunk @%llu\n", (unsigned long long)first, (unsigned)count,
		    (unsigned long long)ch->ch_base);
		return (FS_APFS_E_INVAL);
	}
	alloc_n_given += count;
	return (fq_insert(APFS_SFQ_MAIN, g_apfs.ac_xid + 1, first, count));
}

/*
 * Put the bitmap and the chunk-info block down, if anything moved.  Called
 * once by the checkpoint writer, however many allocations there have been.
 *
 * COPY-ON-WRITE, and this is where the checkpoint starts paying for itself.
 * Neither block is written back where it came from; each goes to a block
 * taken from the internal pool, and the space manager is pointed at the new
 * chunk-info block.  Until the checkpoint commits, the old pair is still on
 * the platter saying exactly what the live checkpoint believes, so a crash
 * anywhere in here loses the allocations and nothing else.
 *
 * Once per checkpoint rather than once per allocation, and that is not a
 * refinement: a spine update takes half a dozen calls, each would have cost
 * two pool blocks, and the pool is fifteen.
 *
 * The bitmap goes through the raw path because it has no header to check or
 * to seal; the chunk-info block is a physical object, so its oid is its own
 * block number and it carries the xid of the checkpoint being built.
 */
static int
alloc_flush(uint64_t xid)
{
	struct apfs_chunk_info_block	*cib;
	struct alloc_chunk		*ch;
	uint64_t			 new_bm;
	uint64_t			 new_cib;
	uint64_t			 old_cib;
	uint32_t			 dirty;
	uint32_t			 i;

	dirty = 0;
	for (i = 0; i < g_chunk_n; i++)
		if (g_chunk[i].ch_dirty)
			dirty++;
	if (dirty == 0)
		return (FS_APFS_E_OK);
	if (!g_apfs.ac_ip_valid || g_sm == NULL || g_cib == NULL)
		return (FS_APFS_E_INVAL);

	/*
	 * Every bitmap that changed moves, and the chunk-info block that names
	 * them moves once at the end.  A chunk left clean is left where it is:
	 * its block is still exactly what the live checkpoint believes, so
	 * copying it would spend a pool block to say nothing.
	 */
	cib = (struct apfs_chunk_info_block *)g_cib;
	for (i = 0; i < g_chunk_n; i++) {
		ch = &g_chunk[i];
		if (!ch->ch_dirty)
			continue;
		new_bm = ip_alloc();
		if (new_bm == 0)
			return (FS_APFS_E_NOALLOC);
		if (write_block_raw(new_bm, ch->ch_bm) != FS_APFS_E_OK) {
			kprintf("apfs: new bitmap %llu for the chunk @%llu "
			    "would not write\n", (unsigned long long)new_bm,
			    (unsigned long long)ch->ch_base);
			ip_free(new_bm);
			return (FS_APFS_E_IO);
		}
		cib->cib_chunk_info[ch->ch_slot].ci_xid         = xid;
		cib->cib_chunk_info[ch->ch_slot].ci_bitmap_addr = new_bm;
		ip_free(ch->ch_bitmap);
		ch->ch_bitmap = new_bm;
		ch->ch_dirty  = false;
		cow_n_meta++;
	}

	new_cib = ip_alloc();
	if (new_cib == 0)
		return (FS_APFS_E_NOALLOC);
	old_cib = g_apfs.ac_alloc_cib;
	cib->cib_o.o_oid = new_cib;		/* physical: oid == block */
	cib->cib_o.o_xid = xid;
	if (fs_apfs_write_block(new_cib, g_cib) != FS_APFS_E_OK) {
		kprintf("apfs: new chunk-info %llu would not write\n",
		    (unsigned long long)new_cib);
		ip_free(new_cib);
		return (FS_APFS_E_IO);
	}

	*(uint64_t *)(g_sm + g_apfs.ac_sm_addr_offset) = new_cib;
	g_apfs.ac_alloc_cib = new_cib;
	ip_free(old_cib);
	cow_n_meta++;
	return (FS_APFS_E_OK);
}

/*
 * THE SPINE
 *
 * A virtual object -- a node of the file-system tree, a volume superblock --
 * is found by asking an object map where its oid lives.  Copy one, and the
 * copy is unreachable until that map says so; change the map, and the map is
 * a physical object that must itself be copied; and so on, all the way to the
 * container superblock, which is the one thing a checkpoint writes by name.
 *
 * The chain was measured on the container rather than reasoned about, and it
 * is seven objects long for a single inode timestamp:
 *
 *	leaf -> volume omap tree -> volume omap -> volume superblock
 *	     -> container omap tree -> container omap -> nx_superblock
 *
 * The measurement also removed half the work I had budgeted.  A virtual tree
 * addresses its children BY OID, so copying a leaf does not move anything the
 * nodes above it hold: the fs-tree root is not touched, and the path copy is
 * one node long.  The container's own history shows it -- its root sits at
 * xid 4 while both its children are still at xid 3, at the addresses they
 * had.
 */
static int
cow_physical(uint64_t old_bno, uint64_t xid, void *buf, uint64_t *new_bno)
{
	struct apfs_obj_phys	*o;
	uint64_t		 bno;
	int			 rv;

	rv = alloc_blocks(1, 0, &bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	o = (struct apfs_obj_phys *)buf;
	o->o_oid = bno;		/* physical: the oid IS the block number */
	o->o_xid = xid;
	rv = fs_apfs_write_block(bno, buf);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(bno, 1);
		return (rv);
	}
	rv = free_blocks(old_bno, 1);
	if (rv != FS_APFS_E_OK)
		return (rv);
	cow_n_spine++;
	*new_bno = bno;
	return (FS_APFS_E_OK);
}

/*
 * Take an entry out of a node whose records are all one size.
 *
 * The variable-KV delete threads the freed bytes onto a chain, because the
 * records around them are all different lengths and a hole is only useful to a
 * record that fits it.  Here every key is sixteen bytes and so is every value,
 * which makes the chain unnecessary and, worse, a leak: leaf_insert_fixed takes
 * from the free span and never looks at a chain, so bytes put on one would
 * never come back, and an object map that gains and loses an entry per
 * transaction is a cycle -- the same shape that ate the free queue's node and
 * then the catalog's.
 *
 * So the hole is FILLED rather than remembered.  The last record placed in the
 * key area is moved into the freed key's bytes and the table entry that named
 * it is pointed at the new place; the same on the value side, measured from the
 * other end.  Which record that is has nothing to do with which record is last
 * in key ORDER -- the table of contents is what puts records in order, and the
 * areas it points into are just storage.
 */
static int
omap_slot_drop(uint8_t *node, uint32_t pos)
{
	struct apfs_btree_node_phys	*n;
	struct btree_layout		 bl;
	struct apfs_kvoff		*kv;
	uint8_t				*keys;
	uint8_t				*vals;
	uint32_t			 klen = sizeof(struct apfs_omap_key);
	uint32_t			 vlen = sizeof(struct apfs_omap_val);
	uint32_t			 last_koff;
	uint32_t			 last_voff;
	uint32_t			 koff, voff;
	uint32_t			 i;

	n = (struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	if (!bl.bl_fixed || pos >= bl.bl_nkeys)
		return (FS_APFS_E_INVAL);
	kv   = (struct apfs_kvoff *)(node + APFS_BTNODE_HDR_SIZE +
	    n->btn_table_space.nl_off);
	keys = node + APFS_BTNODE_HDR_SIZE + n->btn_table_space.nl_off +
	    n->btn_table_space.nl_len;
	vals = node + APFS_BLOCK_SIZE -
	    (((n->btn_flags & APFS_BTNODE_ROOT) != 0) ?
	    APFS_BTREE_INFO_SIZE : 0);
	koff = kv[pos].k;
	voff = kv[pos].v;

	/*
	 * Where the last key and the last value were put: the key area is
	 * filled forwards to btn_free_space.nl_off, and the value area
	 * backwards from the end of the node to just past the free span.
	 */
	last_koff = n->btn_free_space.nl_off - klen;
	last_voff = (uint32_t)(vals - (keys + n->btn_free_space.nl_off +
	    n->btn_free_space.nl_len));

	if (koff != last_koff) {
		mem_copy(keys + koff, keys + last_koff, klen);
		for (i = 0; i < bl.bl_nkeys; i++)
			if (kv[i].k == last_koff) {
				kv[i].k = (uint16_t)koff;
				break;
			}
	}
	n->btn_free_space.nl_off = (uint16_t)last_koff;
	n->btn_free_space.nl_len = (uint16_t)(n->btn_free_space.nl_len + klen);

	if (voff != last_voff) {
		mem_copy(vals - voff, vals - last_voff, vlen);
		for (i = 0; i < bl.bl_nkeys; i++)
			if (kv[i].v == last_voff) {
				kv[i].v = (uint16_t)voff;
				break;
			}
	}
	n->btn_free_space.nl_len = (uint16_t)(n->btn_free_space.nl_len + vlen);

	for (i = pos; i + 1 < bl.bl_nkeys; i++)
		kv[i] = kv[i + 1];
	n->btn_nkeys--;
	return (FS_APFS_E_OK);
}

/*
 * Point an object map's entry for `oid` at `paddr`, and copy the node.
 *
 * REPLACES the entry rather than adding one.  A map may hold several versions
 * of an oid, keyed by the transaction that made them, and that is how a
 * snapshot keeps seeing the old one; with no snapshots there is nothing to
 * keep, and replacing avoids the question of what to do when the node has no
 * room left -- which is a B-tree split, and a different rung.
 */
static int
omap_replace_cow(uint64_t node_bno, const struct omap_edit *oe, uint64_t xid,
    void *buf, uint64_t *new_node)
{
	struct btree_layout	 bl;
	struct apfs_omap_key	*k;
	struct apfs_omap_val	*v;
	uint64_t		*count;
	uint32_t		 koff;
	uint32_t		 voff;
	uint32_t		 done;
	uint32_t		 i;
	uint32_t		 j;
	int			 rv;

	rv = fs_apfs_read_block(node_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	btree_layout(buf, &bl);
	if (bl.bl_level != 0) {
		kprintf("apfs: the object map at %llu has grown to level %u "
		    "-- this writer only knows a single node\n",
		    (unsigned long long)node_bno, (unsigned)bl.bl_level);
		return (FS_APFS_E_INVAL);
	}
	/*
	 * SEVERAL AT ONCE, and that is not a convenience.  One insert moves
	 * both the leaf it lands in and the root whose key count it changes;
	 * doing those as two passes would copy this node twice, and copying it
	 * twice means the second copy replaces an entry in a node the first has
	 * already released -- correct, and six spine objects more expensive
	 * every time.
	 */
	done = 0;
	for (i = 0; i < bl.bl_nkeys && done < oe->oe_n; i++) {
		btree_entry_off(&bl, i, &koff, &voff);
		k = (struct apfs_omap_key *)(bl.bl_keys + koff);
		for (j = 0; j < oe->oe_n; j++) {
			if (k->ok_oid != oe->oe_oids[j])
				continue;
			v = (struct apfs_omap_val *)(bl.bl_vals - voff);
			k->ok_xid   = xid;
			v->ov_paddr = oe->oe_paddrs[j];
			done++;
			break;
		}
	}
	if (done != oe->oe_n) {
		kprintf("apfs: object map at %llu answered for %u of %u oids\n",
		    (unsigned long long)node_bno, (unsigned)done,
		    (unsigned)oe->oe_n);
		return (FS_APFS_E_NOTFOUND);
	}

	/*
	 * And oids that name nothing any more.  Before the inserts, because a
	 * deletion gives the span back and an insert that follows can use it.
	 */
	for (j = 0; j < oe->oe_ngone; j++) {
		btree_layout(buf, &bl);
		for (i = 0; i < bl.bl_nkeys; i++) {
			btree_entry_off(&bl, i, &koff, &voff);
			k = (struct apfs_omap_key *)(bl.bl_keys + koff);
			if (k->ok_oid == oe->oe_gone[j])
				break;
		}
		if (i == bl.bl_nkeys) {
			kprintf("apfs: the object map at %llu does not name oid "
			    "%llu, so it cannot stop naming it\n",
			    (unsigned long long)node_bno,
			    (unsigned long long)oe->oe_gone[j]);
			return (FS_APFS_E_NOTFOUND);
		}
		rv = omap_slot_drop(buf, i);
		if (rv != FS_APFS_E_OK)
			return (rv);
		count = (uint64_t *)((uint8_t *)buf + APFS_BLOCK_SIZE -
		    APFS_BTREE_INFO_SIZE + APFS_BTREE_INFO_KEYCOUNT);
		*count -= 1;
	}

	/*
	 * And wholly NEW objects, when some have just been made.  A split is
	 * the only thing that does this: the upper half is an object nothing
	 * has ever heard of, and writing its block does not make it
	 * reachable -- this entry does.
	 *
	 * A LIST, since the tree learned to gain a level: a root that splits
	 * makes TWO objects at once, because it has to keep its own oid -- the
	 * volume superblock names it -- so both halves are new.  Inserting them
	 * one call at a time would copy this node twice for the same reason the
	 * replacements above are a list.
	 */
	for (j = 0; j < oe->oe_nnew; j++) {
		struct apfs_omap_key	 ik;
		struct apfs_omap_val	 iv;

		btree_layout(buf, &bl);
		for (i = 0; i < bl.bl_nkeys; i++) {
			btree_entry_off(&bl, i, &koff, &voff);
			k = (struct apfs_omap_key *)(bl.bl_keys + koff);
			if (k->ok_oid > oe->oe_new[j])
				break;
		}
		ik.ok_oid   = oe->oe_new[j];
		ik.ok_xid   = xid;
		iv.ov_flags = 0;
		iv.ov_size  = APFS_BLOCK_SIZE;
		iv.ov_paddr = oe->oe_new_paddrs[j];
		rv = leaf_insert_fixed(buf, i, &ik, (uint32_t)sizeof(ik), &iv,
		    (uint32_t)sizeof(iv));
		if (rv != FS_APFS_E_OK)
			return (rv);
		count = (uint64_t *)((uint8_t *)buf + APFS_BLOCK_SIZE -
		    APFS_BTREE_INFO_SIZE + APFS_BTREE_INFO_KEYCOUNT);
		*count += 1;
	}
	return (cow_physical(node_bno, xid, buf, new_node));
}

/*
 * A run of blocks has moved: tell the OTHER tree that names it.
 *
 * The file-system tree answers "where are this file's bytes"; the extent
 * reference tree answers the reverse -- who owns this run, and how many
 * references it has -- which is what lets a block be shared between clones and
 * counted.  Both describe the same run, and apfsck checks one against the
 * other, so a file whose bytes move without this is a file the checker finds.
 *
 * The record's KEY is the run's first block, which is what makes this
 * different from patching a file extent: the key changes, so the record sorts
 * somewhere else.  Nothing is inserted or removed for all that -- the count is
 * the same and so are the sizes, so the key and value keep the bytes they
 * already occupy and only their entry in the table of contents moves.  That is
 * deliberate: a B-tree insert has to find room and a node with none has to
 * split, which is the rung after this one.
 */
static int
extref_move(uint64_t old_start, uint64_t new_start, uint64_t blocks,
    uint64_t xid, void *buf)
{
	struct apfs_btree_node_phys	*n;
	struct apfs_phys_ext_val	*pv;
	struct btree_layout		 bl;
	struct apfs_kvloc		*kv;
	struct apfs_kvloc		 save;
	uint8_t				*node;
	uint64_t			 raw;
	uint64_t			 new_bno;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	uint32_t			 pos;
	int				 rv;

	if (g_apfs.ac_extref_bno == 0)
		return (FS_APFS_E_NOTFOUND);
	rv = fs_apfs_read_block(g_apfs.ac_extref_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	node = buf;
	n    = (struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	if (bl.bl_level != 0) {
		kprintf("apfs: the extent reference tree at %llu has grown to "
		    "level %u -- this writer only knows a single node\n",
		    (unsigned long long)g_apfs.ac_extref_bno,
		    (unsigned)bl.bl_level);
		return (FS_APFS_E_INVAL);
	}
	if (bl.bl_fixed || bl.bl_nkeys == 0)
		return (FS_APFS_E_INVAL);

	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_EXTENT)
			continue;
		if ((raw & APFS_J_OBJ_ID_MASK) != old_start)
			continue;
		if (vlen < sizeof(*pv))
			return (FS_APFS_E_INVAL);
		break;
	}
	if (i == bl.bl_nkeys) {
		kprintf("apfs: no physical extent record starts at %llu -- the "
		    "run being moved is not the whole of one\n",
		    (unsigned long long)old_start);
		return (FS_APFS_E_NOTFOUND);
	}

	/*
	 * Length in BLOCKS here, in bytes in the file extent.  Checked rather
	 * than trusted: a run relocated as a whole must be exactly one record,
	 * and moving a record that describes a different span would leave the
	 * two trees each internally consistent and disagreeing with each other.
	 */
	pv = (struct apfs_phys_ext_val *)(bl.bl_vals - voff);
	if ((pv->pe_len_and_kind & APFS_PEXT_LEN_MASK) != blocks) {
		kprintf("apfs: the extent at %llu is %llu blocks here and %llu "
		    "in the file -- not moving it\n",
		    (unsigned long long)old_start,
		    (unsigned long long)(pv->pe_len_and_kind &
		    APFS_PEXT_LEN_MASK), (unsigned long long)blocks);
		return (FS_APFS_E_INVAL);
	}

	*(uint64_t *)(bl.bl_keys + koff) = new_start |
	    ((uint64_t)APFS_TYPE_EXTENT << APFS_J_OBJ_TYPE_SHIFT);

	/* Lift the entry out, find where the new key sorts, put it back. */
	kv   = (struct apfs_kvloc *)(node + APFS_BTNODE_HDR_SIZE +
	    n->btn_table_space.nl_off);
	save = kv[i];
	for (pos = i; pos + 1 < bl.bl_nkeys; pos++)
		kv[pos] = kv[pos + 1];
	for (pos = 0; pos + 1 < bl.bl_nkeys; pos++) {
		raw = *(const uint64_t *)(bl.bl_keys + kv[pos].k.nl_off);
		if ((raw & APFS_J_OBJ_ID_MASK) > new_start)
			break;
	}
	for (i = bl.bl_nkeys - 1; i > pos; i--)
		kv[i] = kv[i - 1];
	kv[pos] = save;

	rv = cow_physical(g_apfs.ac_extref_bno, xid, buf, &new_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	/*
	 * PHYSICAL, so nothing resolves an oid to find it: the volume
	 * superblock names its block outright, and spine_update writes this
	 * number in when it copies that superblock.
	 */
	g_apfs.ac_extref_bno = new_bno;
	return (FS_APFS_E_OK);
}

/*
 * A virtual object has moved to `paddr`.  Make that the answer everything
 * from here to the container superblock gives.
 *
 * Every step is a copy, so at no point does the live checkpoint stop being
 * true; the last of them leaves the new container object map in ac_omap_oid,
 * and the checkpoint writer puts THAT into the superblock it commits.  Until
 * it does, none of this is reachable from anything on the disk.
 */
static int
spine_update_n(const struct omap_edit *oe, uint64_t xid, void *buf)
{
	struct apfs_omap_phys		*om;
	struct apfs_superblock		*vsb;
	struct apfs_obj_phys		*o;
	struct omap_edit		 ctr;
	uint64_t			 bno;
	uint64_t			 new_ctr_omap;
	uint64_t			 new_ctr_tree;
	uint64_t			 new_vol_omap;
	uint64_t			 new_vol_tree;
	uint64_t			 new_vsb;
	uint64_t			 fs_oid;
	int				 rv;

	/* 1. the volume's object map: those oids now live at those addresses */
	rv = omap_replace_cow(g_apfs.ac_vol_omap_tree, oe, xid, buf,
	    &new_vol_tree);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/* 2. the object map object, which names that tree */
	rv = fs_apfs_read_block(g_apfs.ac_vol_omap_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	om = (struct apfs_omap_phys *)buf;
	om->om_tree_oid = new_vol_tree;
	rv = cow_physical(g_apfs.ac_vol_omap_bno, xid, buf, &new_vol_omap);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/*
	 * 3. the volume superblock, which names that object map.  This one is
	 * VIRTUAL: its oid is a name the container's map resolves, so the
	 * copy keeps the oid it had and only its address changes.
	 */
	rv = fs_apfs_read_block(g_apfs.ac_vol_sb_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	vsb = (struct apfs_superblock *)buf;
	vsb->apfs_omap_oid = new_vol_omap;
	/*
	 * The extent reference tree is named from here too, and it moves for
	 * its own reasons -- a file's bytes changing address -- which have
	 * nothing to do with the object map.  Writing it unconditionally is
	 * what keeps the two from having to know about each other: whoever
	 * moved it left the new address in ac_extref_bno, and if nobody did,
	 * this writes back what is already there.
	 */
	vsb->apfs_extentref_tree_oid = g_apfs.ac_extref_bno;
	vsb->apfs_fs_alloc_count     = g_apfs.ac_fs_alloc_count;
	/*
	 * And what the volume says about its own contents.  Same terms as the
	 * two above: whoever changed one left it in g_apfs, and if nobody did,
	 * this writes back what was already there.
	 *
	 * The two are not equally load-bearing, and that was measured rather
	 * than assumed.  apfs_next_obj_id is checked -- a created inode whose
	 * number the volume still calls free stops apfsck before it looks at
	 * anything else.  apfs_num_files is not checked at all, in either
	 * direction; it is written because it is the volume's own claim about
	 * itself and something other than this checker will read it.
	 */
	vsb->apfs_next_obj_id        = g_apfs.ac_next_ino;
	vsb->apfs_num_files          = g_apfs.ac_num_files;
	vsb->apfs_num_directories    = g_apfs.ac_num_dirs;
	rv = alloc_blocks(1, 0, &bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	o = (struct apfs_obj_phys *)buf;
	o->o_xid = xid;				/* o_oid stays: it is a name */
	rv = fs_apfs_write_block(bno, buf);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(bno, 1);
		return (rv);
	}
	rv = free_blocks(g_apfs.ac_vol_sb_bno, 1);
	if (rv != FS_APFS_E_OK)
		return (rv);
	cow_n_spine++;
	new_vsb = bno;

	/* 4. the container's object map: the volume superblock has moved */
	fs_oid = g_apfs.ac_fs_oid;
	omap_edit_init(&ctr);
	ctr.oe_oids   = &fs_oid;
	ctr.oe_paddrs = &new_vsb;
	ctr.oe_n      = 1;
	rv = omap_replace_cow(g_apfs.ac_ctr_omap_tree, &ctr, xid, buf,
	    &new_ctr_tree);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/* 5. and the object it hangs from, which the superblock names */
	rv = fs_apfs_read_block(g_apfs.ac_omap_oid, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	om = (struct apfs_omap_phys *)buf;
	om->om_tree_oid = new_ctr_tree;
	rv = cow_physical(g_apfs.ac_omap_oid, xid, buf, &new_ctr_omap);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/*
	 * Believed only now, and all together.  A failure anywhere above
	 * leaves blocks allocated to a chain nothing points at -- which the
	 * next checkpoint publishes as a leak, and which is one of the
	 * reasons the free queue is the rung after this one.
	 */
	g_apfs.ac_vol_omap_tree = new_vol_tree;
	g_apfs.ac_vol_omap_bno  = new_vol_omap;
	g_apfs.ac_vol_sb_bno    = new_vsb;
	g_apfs.ac_ctr_omap_tree = new_ctr_tree;
	g_apfs.ac_omap_oid      = new_ctr_omap;
	g_apfs.ac_dirty         = true;
	return (FS_APFS_E_OK);
}

/* One object moved, which is what most callers have. */
static int
spine_update(uint64_t oid, uint64_t paddr, uint64_t xid, void *buf)
{
	struct omap_edit	oe;

	omap_edit_init(&oe);
	oe.oe_oids   = &oid;
	oe.oe_paddrs = &paddr;
	oe.oe_n      = 1;
	return (spine_update_n(&oe, xid, buf));
}

/*
 * The three numbers, read OFF THE DISK from blocks the caller names.
 *
 * Naming them is the point.  Once the allocator copies rather than
 * overwrites, "the bitmap" is two different blocks depending on whether the
 * question is about the checkpoint that is live or the one being built, and a
 * reader that always follows the current pointers cannot ask the first
 * question at all.
 */
struct alloc_snap {
	uint64_t	as_dev_free;
	uint32_t	as_chunk_free;
	uint32_t	as_clear_bits;
};

static int
alloc_snapshot(uint64_t cib_bno, uint64_t bm_bno, uint64_t sm_bno,
    void *cib_buf, void *sm_buf, void *bm_buf, struct alloc_snap *out)
{
	const struct apfs_chunk_info_block	*cib;
	const struct apfs_spaceman		*sm;

	if (fs_apfs_read_block(cib_bno, cib_buf) != FS_APFS_E_OK ||
	    fs_apfs_read_block(sm_bno, sm_buf) != FS_APFS_E_OK ||
	    fs_apfs_read_block_raw(bm_bno, bm_buf) != FS_APFS_E_OK)
		return (FS_APFS_E_IO);

	cib = (const struct apfs_chunk_info_block *)cib_buf;
	sm  = (const struct apfs_spaceman *)sm_buf;
	out->as_chunk_free = cib->cib_chunk_info[g_home->ch_slot].ci_free_count;
	out->as_dev_free   = sm->sm_dev[APFS_SD_MAIN].sm_free_count;
	out->as_clear_bits = bitmap_free_count((const uint8_t *)bm_buf,
	    g_home->ch_blocks);
	return (FS_APFS_E_OK);
}

static bool
alloc_snap_eq(const struct alloc_snap *a, const struct alloc_snap *b)
{

	return (a->as_dev_free == b->as_dev_free &&
	    a->as_chunk_free == b->as_chunk_free &&
	    a->as_clear_bits == b->as_clear_bits);
}

/*
 * Is every block of this run marked taken on the DISK right now?  1 yes,
 * 0 no, negative if the bitmap would not read.
 *
 * Asked of specific blocks rather than of the free counts, because the counts
 * move for reasons the caller does not control -- every checkpoint releases
 * whatever the queues have finished holding -- while these eight bits mean
 * exactly one thing.
 */
static int
alloc_run_taken(uint64_t first, uint32_t count, void *bm_buf)
{
	const struct alloc_chunk	*ch;
	const uint8_t			*bm;
	uint64_t			 bit;
	uint32_t			 i;

	ch = chunk_resident(first);
	if (ch == NULL)
		return (-1);
	if (fs_apfs_read_block_raw(ch->ch_bitmap, bm_buf) != FS_APFS_E_OK)
		return (-1);
	bm  = (const uint8_t *)bm_buf;
	bit = first - ch->ch_base;
	for (i = 0; i < count; i++) {
		if ((bm[(bit + i) >> 3] & (uint8_t)(1u << ((bit + i) & 7u)))
		    == 0)
			return (0);
	}
	return (1);
}

/*
 * ALLOCATE, LOOK, PUT BACK.
 *
 * The rung this belongs to was going to be "an allocator that only allocates"
 * -- take blocks, never give any back, and so never need the free queues or a
 * transaction id to key them by.  Trying it on the image first settled it: a
 * container with a block marked in use that nothing references is not valid,
 * and apfsck says so in one line.
 *
 *	Space manager: bad allocation bitmap.
 *
 * That is not a complaint about the edit.  Rewriting a metadata block
 * resealed is invisible to apfsck, and a bitmap bit set with both counters
 * moved to match passes the chunk-info check -- it is the NEXT check that
 * fails, the one comparing the bitmap against the set of blocks something
 * actually points at.  In this format an allocation is not a thing on its own;
 * it is half of an operation whose other half is a reference, and the two are
 * only valid together.
 *
 * So what can be proved without the other half is everything up to it: find a
 * run, take it, see the disk agree, give it back, see the disk return to what
 * it was.  The volume is briefly in the state apfsck rejects, which is honest
 * -- it is exactly the state a half-finished allocation leaves -- and it does
 * not outlive the call.
 *
 * Since the metadata is copied rather than overwritten, the test can now also
 * ask the question that matters more than the arithmetic: after the
 * allocation and before the checkpoint, does the LIVE checkpoint still read
 * exactly as it did?  A writer that got copy-on-write subtly wrong -- copying
 * the bitmap but not the chunk-info, say, or moving the pointer before the
 * block -- passes every count in this test and fails that one.
 */
void
fs_apfs_alloc_selftest(void)
{
	struct alloc_snap	 base;
	struct alloc_snap	 live;
	struct alloc_chunk	*other;
	void			*cib_buf;
	void			*sm_buf;
	void			*bm_buf;
	uint64_t		 new_bm;
	uint64_t		 new_cib;
	uint64_t		 old_bm;
	uint64_t		 old_cib;
	uint64_t		 old_sm;
	uint64_t		 first;
	uint64_t		 second;
	uint64_t		 again;
	uint64_t		 away;
	struct apfs_btree_node_phys *fqn;
	uint32_t		 run;
	uint32_t		 room;
	uint32_t		 held;
	uint32_t		 i;
	int			 taken;

	if (!g_apfs.ac_mounted || !g_apfs.ac_bm_valid || !g_apfs.ac_alloc_have) {
		kprintf("apfs-alloc: no chunk to work in -- skipped\n");
		return;
	}

	cib_buf = kmalloc(APFS_BLOCK_SIZE);
	sm_buf  = kmalloc(APFS_BLOCK_SIZE);
	bm_buf  = kmalloc(APFS_BLOCK_SIZE);
	if (cib_buf == NULL || sm_buf == NULL || bm_buf == NULL) {
		kprintf("apfs-alloc: no memory -- skipped\n");
		goto out;
	}

	run     = 8;
	old_bm  = g_home->ch_bitmap;
	old_cib = g_apfs.ac_alloc_cib;
	old_sm  = g_apfs.ac_sm_paddr;
	if (alloc_snapshot(old_cib, old_bm, old_sm, cib_buf, sm_buf, bm_buf,
	    &base) != FS_APFS_E_OK) {
		kprintf("apfs-alloc: FAIL cannot read the chunk\n");
		goto out;
	}

	if (alloc_blocks(run, 0, &first) != FS_APFS_E_OK) {
		kprintf("apfs-alloc: FAIL could not take a run of %u\n",
		    (unsigned)run);
		goto out;
	}

	/*
	 * THE FIRST CLAIM.  The allocation is complete as far as this kernel
	 * is concerned, and the checkpoint that is still live must not be
	 * able to tell.  Asked of the blocks it names, everything reads
	 * exactly as it did before.
	 */
	if (alloc_snapshot(old_cib, old_bm, old_sm, cib_buf, sm_buf, bm_buf,
	    &live) != FS_APFS_E_OK) {
		kprintf("apfs-alloc: FAIL the live checkpoint's blocks no "
		    "longer read\n");
		goto out;
	}
	if (!alloc_snap_eq(&live, &base)) {
		kprintf("apfs-alloc: FAIL the live checkpoint changed under "
		    "it: chunk %u vs %u, device %llu vs %llu, clear bits %u "
		    "vs %u\n", (unsigned)live.as_chunk_free,
		    (unsigned)base.as_chunk_free,
		    (unsigned long long)live.as_dev_free,
		    (unsigned long long)base.as_dev_free,
		    (unsigned)live.as_clear_bits,
		    (unsigned)base.as_clear_bits);
		goto out;
	}

	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-alloc: FAIL the checkpoint was refused -- the "
		    "allocation is lost, which is the correct outcome\n");
		goto out;
	}
	new_bm  = g_home->ch_bitmap;
	new_cib = g_apfs.ac_alloc_cib;
	if (new_bm == old_bm || new_cib == old_cib) {
		kprintf("apfs-alloc: FAIL the bitmap (%llu) or chunk-info "
		    "(%llu) was written in place\n",
		    (unsigned long long)old_bm, (unsigned long long)old_cib);
		goto out;
	}
	taken = alloc_run_taken(first, run, bm_buf);
	if (taken != 1) {
		kprintf("apfs-alloc: FAIL the run at %llu is not marked taken "
		    "after the checkpoint (%d)\n", (unsigned long long)first,
		    taken);
		goto out;
	}

	/*
	 * THE SECOND CLAIM, and the one the free queue exists for.  Giving
	 * the run back does NOT make it free: the checkpoints behind this one
	 * still describe a container in which those blocks are in use, and
	 * handing them out again would turn every one of those superblocks
	 * into a lie.  So after the release, and after a checkpoint publishes
	 * it, the bits are still set.
	 */
	if (free_blocks(first, run) != FS_APFS_E_OK ||
	    fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-alloc: FAIL could not give the run back\n");
		goto out;
	}
	taken = alloc_run_taken(first, run, bm_buf);
	if (taken != 1) {
		kprintf("apfs-alloc: FAIL the run at %llu was freed the "
		    "moment it was released (%d) -- the checkpoints behind "
		    "this one now point at reusable blocks\n",
		    (unsigned long long)first, taken);
		goto out;
	}

	/*
	 * AND THE THIRD.  It does not stay held for ever either.  Once the
	 * transaction that released it is APFS_FQ_KEEP checkpoints behind,
	 * the queue lets go and the blocks are free again.
	 */
	for (i = 0; i <= APFS_FQ_KEEP; i++) {
		if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
			kprintf("apfs-alloc: FAIL checkpoint %u of the wait "
			    "was refused\n", (unsigned)i);
			goto out;
		}
	}
	taken = alloc_run_taken(first, run, bm_buf);
	if (taken != 0) {
		kprintf("apfs-alloc: FAIL the run at %llu is still held %u "
		    "checkpoints later (%d)\n", (unsigned long long)first,
		    (unsigned)(APFS_FQ_KEEP + 1), taken);
		goto out;
	}

	/*
	 * AND THE FOURTH, which is about the queue's own node rather than the
	 * blocks recorded in it.
	 *
	 * A release puts the key and count it took out onto the node's free
	 * lists, and an insert has to take them back.  There is a reason that
	 * looks unnecessary, and it is the reason this claim is written the
	 * awkward way it is: a queue that reaches EMPTY has its node reset
	 * outright -- span restored, chains cleared -- and in a quiet container
	 * the queue empties all the time.  A BUSY one never does; there is
	 * always something from the last few checkpoints in it, the reset never
	 * fires, and a node that only ever ate into its span loses an entry's
	 * worth of room per cycle until it refuses to record a release at all.
	 * That refusal is a leaked block, and it is what a boot printed the
	 * first time truncation gave it enough work to get there: "free queue 1
	 * is still full" with EIGHT keys in a node that had been holding
	 * ninety.
	 *
	 * So the queue is deliberately kept busy -- one release per checkpoint,
	 * which is exactly what stops it emptying -- and the node is measured
	 * over two stretches of that at the same depth.  The second stretch
	 * must cost it nothing.
	 *
	 * Written this way because the obvious version does not work: two
	 * take-and-release cycles with a drain between them PASS on a node that
	 * reuses nothing, because the drain resets it.  That version was
	 * written first, and it passed against a kernel broken on purpose.
	 */
	fqn = (struct apfs_btree_node_phys *)g_fq[APFS_SFQ_MAIN];
	for (i = 0; i < 2u * (APFS_FQ_KEEP + 1u); i++) {
		if (alloc_blocks(run, 0, &again) != FS_APFS_E_OK ||
		    free_blocks(again, run) != FS_APFS_E_OK ||
		    fs_apfs_checkpoint() != FS_APFS_E_OK) {
			kprintf("apfs-alloc: FAIL cycle %u of keeping the free "
			    "queue busy was refused\n", (unsigned)i);
			goto out;
		}
	}
	room = fqn->btn_free_space.nl_len;
	held = fqn->btn_nkeys;
	for (i = 0; i < APFS_FQ_KEEP + 1u; i++) {
		if (alloc_blocks(run, 0, &again) != FS_APFS_E_OK ||
		    free_blocks(again, run) != FS_APFS_E_OK ||
		    fs_apfs_checkpoint() != FS_APFS_E_OK) {
			kprintf("apfs-alloc: FAIL cycle %u of the measured "
			    "stretch was refused\n", (unsigned)i);
			goto out;
		}
	}
	if (fqn->btn_nkeys != held) {
		kprintf("apfs-alloc: FAIL the free queue held %u entries and "
		    "now holds %u -- one release per checkpoint should hold it "
		    "at a steady depth, and without that the room it has left "
		    "cannot be compared\n", (unsigned)held,
		    (unsigned)fqn->btn_nkeys);
		goto out;
	}
	if (fqn->btn_free_space.nl_len != room) {
		kprintf("apfs-alloc: FAIL the free queue's node went from %u "
		    "bytes of free span to %u while holding the same %u "
		    "entries -- it is not reusing the holes its own releases "
		    "leave, and a queue that never empties will run out of "
		    "room while nearly empty\n", (unsigned)room,
		    (unsigned)fqn->btn_free_space.nl_len, (unsigned)held);
		goto out;
	}

	/*
	 * AND THE FIFTH, which is the rung this test grew for.  A file's bytes
	 * are not where its metadata is: in this container the one real file
	 * keeps its content in the chunk at block 0 while everything being
	 * copied around it lives in the chunk at 98304.  Relocating those bytes
	 * therefore takes a run in one chunk and gives one back in another,
	 * inside a single transaction, and the two are different bitmaps -- set
	 * apart, dirtied apart, and written apart by the checkpoint.
	 *
	 * Asked of a chunk that is NOT the one metadata comes from, so that
	 * failing to reach it fails here rather than at the first write.
	 */
	away  = (g_home->ch_base == 0) ?
	    g_home->ch_base + g_home->ch_blocks : 1;
	other = chunk_for(away);
	if (other == NULL || other == g_home) {
		kprintf("apfs-alloc: only one chunk can be reached -- the "
		    "half of this test about a second one is skipped\n");
		second = 0;
	} else {
		if (alloc_blocks(run, away, &second) != FS_APFS_E_OK) {
			kprintf("apfs-alloc: FAIL no run of %u in the chunk "
			    "@%llu\n", (unsigned)run,
			    (unsigned long long)other->ch_base);
			goto out;
		}
		if (second >= g_home->ch_base &&
		    second < g_home->ch_base + g_home->ch_blocks) {
			kprintf("apfs-alloc: FAIL the run at %llu came out of "
			    "the chunk metadata uses (@%llu) -- the hint was "
			    "ignored, and a file's bytes cannot be moved where "
			    "they are\n", (unsigned long long)second,
			    (unsigned long long)g_home->ch_base);
			(void)free_blocks(second, run);
			goto out;
		}
		if (free_blocks(second, run) != FS_APFS_E_OK) {
			kprintf("apfs-alloc: FAIL the run at %llu was taken "
			    "but cannot be given back\n",
			    (unsigned long long)second);
			goto out;
		}
		for (i = 0; i <= APFS_FQ_KEEP; i++) {
			if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
				kprintf("apfs-alloc: FAIL checkpoint %u of "
				    "the second wait was refused\n",
				    (unsigned)i);
				goto out;
			}
		}
		taken = alloc_run_taken(second, run, bm_buf);
		if (taken != 0) {
			kprintf("apfs-alloc: FAIL the run at %llu never came "
			    "back (%d) -- a chunk that is not the one metadata "
			    "uses was written to and not accounted\n",
			    (unsigned long long)second, taken);
			goto out;
		}
	}

	kprintf("apfs-alloc: PASS -- took %u blocks at %llu, the live "
	    "checkpoint saw nothing, the release held them for %u "
	    "checkpoints, then the queue let them go\n", (unsigned)run,
	    (unsigned long long)first, (unsigned)APFS_FQ_KEEP);
	if (second != 0)
		kprintf("apfs-alloc: and %u more at %llu, in the chunk @%llu "
		    "rather than the chunk @%llu metadata comes from -- two "
		    "bitmaps, taken and returned apart\n", (unsigned)run,
		    (unsigned long long)second,
		    (unsigned long long)other->ch_base,
		    (unsigned long long)g_home->ch_base);
	kprintf("apfs-alloc: metadata moved -- bitmap %llu -> %llu, "
	    "chunk-info %llu -> %llu, pool bitmap in ring slot %u\n",
	    (unsigned long long)old_bm, (unsigned long long)new_bm,
	    (unsigned long long)old_cib, (unsigned long long)new_cib,
	    (unsigned)g_apfs.ac_ipbm_slot);

out:
	kfree(cib_buf);
	kfree(sm_buf);
	kfree(bm_buf);
}

/*
 * A WRITE MOVES THE BYTES, AND THE CHECKPOINT BEHIND IT KEEPS ITS OWN
 *
 * The claim in one sentence: after a write, the block the live checkpoint
 * still names holds exactly what it held.  Everything the last several rungs
 * built is worth nothing to a file's contents unless that is true -- an intact
 * ring of superblocks leading to bytes that have since been overwritten is a
 * ring of superblocks that lies.
 *
 * Asked of the block rather than of the file, and read RAW, because the point
 * is what is on the platter at an address nothing in this kernel is pointing
 * at any more.  A reader that followed the current records would be shown the
 * new copy and would agree with itself all the way to being wrong.
 */
#define	APFS_DATA_PATTERN	"style9 relocated these bytes."

void
fs_apfs_data_selftest(const char *path)
{
	uint8_t		*block;
	uint8_t		 before[32];
	uint8_t		 after[32];
	const char	*pat = APFS_DATA_PATTERN;
	uint64_t	 id;
	uint64_t	 size;
	uint64_t	 ino;
	uint64_t	 old_phys;
	uint64_t	 new_phys;
	uint32_t	 put;
	uint32_t	 n;
	uint32_t	 i;
	int		 rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-data: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_open(path, &id, &size, &ino) != FS_APFS_E_OK) {
		kprintf("apfs-data: %s absent -- skipped\n", path);
		return;
	}
	n = (uint32_t)str_len(pat);
	if (size < sizeof(before) || n > sizeof(before)) {
		kprintf("apfs-data: %s too small -- skipped\n", path);
		return;
	}

	block = kmalloc(APFS_BLOCK_SIZE);
	if (block == NULL) {
		kprintf("apfs-data: no memory -- skipped\n");
		return;
	}

	if (extent_at(id, 0, &old_phys) != FS_APFS_E_OK || old_phys == 0) {
		kprintf("apfs-data: FAIL no extent describes byte 0\n");
		goto out;
	}
	if (fs_apfs_read_block_raw(old_phys, block) != FS_APFS_E_OK) {
		kprintf("apfs-data: FAIL block %llu will not read\n",
		    (unsigned long long)old_phys);
		goto out;
	}
	for (i = 0; i < sizeof(before); i++)
		before[i] = block[i];

	rv = fs_apfs_pwrite(id, size, 0, (const uint8_t *)pat, n, &put);
	if (rv != FS_APFS_E_OK || put != n) {
		kprintf("apfs-data: FAIL the write was refused (%d, %u of "
		    "%u)\n", rv, (unsigned)put, (unsigned)n);
		goto out;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-data: FAIL the checkpoint was refused -- the "
		    "write is lost, which is the correct outcome\n");
		goto out;
	}

	/*
	 * THE FIRST CLAIM.  The bytes are somewhere else now.  A write that
	 * landed back on the same block passes every read-back check in this
	 * kernel and fails this one, which is the whole rung.
	 */
	if (extent_at(id, 0, &new_phys) != FS_APFS_E_OK) {
		kprintf("apfs-data: FAIL the extent is gone after the write\n");
		goto out;
	}
	if (new_phys == old_phys) {
		kprintf("apfs-data: FAIL the file's bytes were written in "
		    "place at %llu -- every checkpoint behind this one now "
		    "describes contents it never had\n",
		    (unsigned long long)old_phys);
		goto out;
	}

	/* THE SECOND.  The new block really did receive the write. */
	if (fs_apfs_read_block_raw(new_phys, block) != FS_APFS_E_OK) {
		kprintf("apfs-data: FAIL block %llu will not read\n",
		    (unsigned long long)new_phys);
		goto out;
	}
	for (i = 0; i < n; i++) {
		if (block[i] == (uint8_t)pat[i])
			continue;
		kprintf("apfs-data: FAIL byte %u of the new block at %llu is "
		    "0x%02x, wanted 0x%02x\n", (unsigned)i,
		    (unsigned long long)new_phys, (unsigned)block[i],
		    (unsigned)(uint8_t)pat[i]);
		goto out;
	}

	/*
	 * THE THIRD, and the one the rung exists for.  The old block is the
	 * one every checkpoint written before this still names, and it has to
	 * read as it did -- not merely "as something valid", but byte for byte
	 * what was there before the write.
	 */
	if (fs_apfs_read_block_raw(old_phys, block) != FS_APFS_E_OK) {
		kprintf("apfs-data: FAIL block %llu will not read back\n",
		    (unsigned long long)old_phys);
		goto out;
	}
	for (i = 0; i < sizeof(before); i++)
		after[i] = block[i];
	for (i = 0; i < sizeof(before); i++) {
		if (after[i] == before[i])
			continue;
		kprintf("apfs-data: FAIL byte %u of the block the live "
		    "checkpoint names (%llu) changed from 0x%02x to 0x%02x\n",
		    (unsigned)i, (unsigned long long)old_phys,
		    (unsigned)before[i], (unsigned)after[i]);
		goto out;
	}

	/* Put the file back, which relocates it once more. */
	rv = fs_apfs_pwrite(id, size, 0, before, sizeof(before), &put);
	if (rv != FS_APFS_E_OK || fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-data: FAIL the restore did not land (%d)\n", rv);
		goto out;
	}

	kprintf("apfs-data: PASS -- %s moved %llu -> %llu, the new run has "
	    "the write, and the run the live checkpoint still names is "
	    "unchanged\n", path, (unsigned long long)old_phys,
	    (unsigned long long)new_phys);
out:
	kfree(block);
}

/* Every record, and the biggest non-root leaf seen holding them. */
struct leaf_probe {
	uint64_t	lp_bno;		/* the leaf the last record was in */
	uint64_t	lp_count;	/* records seen so far             */
	uint64_t	lp_best;	/* a leaf worth splitting          */
	uint32_t	lp_here;	/* records seen in lp_bno          */
	uint32_t	lp_most;
};

static bool
leaf_probe(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct leaf_probe	*lp;

	(void)oid;
	(void)type;
	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	lp = arg;
	lp->lp_count++;
	if (bno != lp->lp_bno) {
		lp->lp_bno  = bno;
		lp->lp_here = 0;
	}
	lp->lp_here++;
	if (lp->lp_here > lp->lp_most) {
		lp->lp_most = lp->lp_here;
		lp->lp_best = bno;
	}
	return (true);
}

/*
 * A NODE SPLITS AND NOTHING IS LOST
 *
 * The organic route to this stopped being organic.  Appending was meant to
 * fill a leaf four records at a time, and then extents that touch started
 * being merged instead -- which is right, and which means a sequential writer
 * never fills anything.  So the split is asked for directly.
 *
 * Two things are checked, and finding out that one was not enough is what
 * this comment is for.  Counting the records through the ordinary walk proves
 * none was lost or duplicated -- but a WALK CANNOT SEE A WRONG SEPARATOR.  It
 * visits every child of the index in turn whatever the keys say, so the count
 * comes out right, the file still reads, and the tree is quietly out of order.
 * Written with a separator taken from the wrong half on purpose, this test
 * passed; apfsck did not:
 *
 *	B-tree: keys are out of order.
 *
 * So the index is checked here as well, by the invariant a split has to keep:
 * the key the parent stores for a child is that child's own first key.  That
 * is a claim this kernel can make about itself, rather than one it has to send
 * a container away to have checked.
 */

/*
 * Every separator in the tree against the child it names, at every level.
 *
 * Recursive since the tree can be more than two levels deep, and the level
 * that used to be the only one is now the least interesting: a wrong
 * separator written INTO an interior node by a split of another interior node
 * is invisible from the root, which still names the same child by the same
 * key it always did.
 */
static bool
index_check(uint64_t bno, uint32_t depth)
{
	struct btree_layout	 bl;
	struct btree_layout	 cl;
	uint8_t			*node;
	uint8_t			*child;
	uint64_t		 oid;
	uint64_t		 cbno;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 ckoff, cklen, cvoff, cvlen;
	uint32_t		 i;
	bool			 ok;

	if (depth >= APFS_TREE_MAX_DEPTH)
		return (false);
	node  = kmalloc(APFS_BLOCK_SIZE);
	child = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL || child == NULL) {
		kfree(node);
		kfree(child);
		return (false);
	}
	ok = fs_apfs_read_block(bno, node) == FS_APFS_E_OK;
	if (!ok)
		kprintf("apfs-split: FAIL the node at %llu will not read\n",
		    (unsigned long long)bno);
	else
		btree_layout(node, &bl);
	if (ok && (bl.bl_flags & APFS_BTNODE_LEAF) != 0)
		goto done;
	for (i = 0; ok && i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		oid = *(const uint64_t *)(bl.bl_vals - voff);
		if (fs_apfs_omap_lookup(g_apfs.ac_vol_omap_tree, oid,
		    view_xid(), &cbno) != FS_APFS_E_OK ||
		    fs_apfs_read_block(cbno, child) != FS_APFS_E_OK) {
			kprintf("apfs-split: FAIL the node at %llu names child "
			    "oid %llu, which the object map cannot place\n",
			    (unsigned long long)bno, (unsigned long long)oid);
			ok = false;
			break;
		}
		btree_layout(child, &cl);
		btree_entry_loc(&cl, 0, &ckoff, &cklen, &cvoff, &cvlen);
		if (jkey_cmp(bl.bl_keys + koff, klen, cl.bl_keys + ckoff,
		    cklen) != 0) {
			kprintf("apfs-split: FAIL the node at %llu says child "
			    "%u starts at one key and the child at %llu starts "
			    "at another -- the separator is from the wrong "
			    "node\n", (unsigned long long)bno, (unsigned)i,
			    (unsigned long long)cbno);
			ok = false;
			break;
		}
		if (cl.bl_level + 1 != bl.bl_level) {
			kprintf("apfs-split: FAIL the node at %llu is at level "
			    "%u and its child at %llu at level %u\n",
			    (unsigned long long)bno, (unsigned)bl.bl_level,
			    (unsigned long long)cbno, (unsigned)cl.bl_level);
			ok = false;
			break;
		}
		ok = index_check(cbno, depth + 1);
	}
done:
	kfree(node);
	kfree(child);
	return (ok);
}

void
fs_apfs_split_selftest(void)
{
	struct fs_apfs_statbuf	 st;
	struct btree_layout	 bl;
	struct leaf_probe	 lp;
	uint8_t			*scratch;
	uint64_t		 before;
	uint64_t		 victim;
	uint64_t		 after;
	uint64_t		 splits;
	uint64_t		 deeper;
	uint32_t		 was;
	bool			 stopped;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-split: nothing writable -- skipped\n");
		return;
	}

	lp.lp_bno   = 0;
	lp.lp_count = 0;
	lp.lp_best  = 0;
	lp.lp_here  = 0;
	lp.lp_most  = 0;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_probe, &lp, 0,
	    &stopped)) {
		kprintf("apfs-split: FAIL the tree will not walk\n");
		return;
	}
	before = lp.lp_count;
	if (lp.lp_best == 0 || lp.lp_best == g_apfs.ac_root_tree_bno) {
		kprintf("apfs-split: the tree is one node deep -- skipped\n");
		return;
	}

	victim  = lp.lp_best;
	splits  = split_n;
	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-split: no memory -- skipped\n");
		return;
	}

	/*
	 * HOW DEEP THE TREE IS BEFORE, because this test is the reason it
	 * gets deeper.  It takes a node per boot and never gives one back, so
	 * an image booted enough times reaches the day the root's table of
	 * contents is full -- which used to be where this stopped and said so.
	 * Now the split grows the tree instead, and the two are told apart
	 * here by the level the root reports and by the counter the growth
	 * bumps.
	 */
	deeper = deep_n;
	was    = 0;
	if (fs_apfs_read_block(g_apfs.ac_root_tree_bno, scratch) !=
	    FS_APFS_E_OK) {
		kprintf("apfs-split: FAIL the root at %llu will not read\n",
		    (unsigned long long)g_apfs.ac_root_tree_bno);
		kfree(scratch);
		return;
	}
	btree_layout(scratch, &bl);
	was = (uint32_t)bl.bl_level + 1u;

	if (node_split_at(victim, 0, g_apfs.ac_xid + 1, scratch) !=
	    FS_APFS_E_OK) {
		kprintf("apfs-split: FAIL the leaf at %llu would not split\n",
		    (unsigned long long)victim);
		kfree(scratch);
		return;
	}
	kfree(scratch);
	/*
	 * At least one, and more than one when the leaf's parent was full and
	 * had to split before it could take a separator.  Both are the same
	 * operation asking itself the same question a level up.
	 */
	if (split_n <= splits) {
		kprintf("apfs-split: FAIL the split was not counted\n");
		return;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-split: FAIL the checkpoint was refused -- the "
		    "split is lost, which is the correct outcome\n");
		return;
	}

	lp.lp_bno   = 0;
	lp.lp_count = 0;
	lp.lp_best  = 0;
	lp.lp_here  = 0;
	lp.lp_most  = 0;
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_probe, &lp, 0,
	    &stopped)) {
		kprintf("apfs-split: FAIL the tree will not walk after the "
		    "split -- a separator or a child oid is wrong\n");
		return;
	}
	after = lp.lp_count;
	if (after != before) {
		kprintf("apfs-split: FAIL %llu records before the split and "
		    "%llu after -- the halves do not add up\n",
		    (unsigned long long)before, (unsigned long long)after);
		return;
	}

	/*
	 * Every separator against the child it names, at every level.  This is
	 * the half the walk is blind to, and the half a wrong split shows up
	 * in.
	 */
	if (!index_check(g_apfs.ac_root_tree_bno, 0))
		return;

	/* And a file, because a count can be right while a lookup is not. */
	if (fs_apfs_stat("/var/db/big.txt", &st) != FS_APFS_E_OK) {
		kprintf("apfs-split: FAIL /var/db/big.txt cannot be found "
		    "through the split tree\n");
		return;
	}

	/*
	 * A GROWTH IS NOT A SPLIT AND HAS TO BE TOLD APART FROM ONE.  Both
	 * leave a tree that walks and counts right; only one of them changes
	 * how deep it is, and a level gained without the counter moving (or
	 * the other way about) means the two disagree about what happened.
	 */
	{
		uint8_t			*root;
		uint32_t		 now;

		root = kmalloc(APFS_BLOCK_SIZE);
		if (root == NULL) {
			kprintf("apfs-split: FAIL no memory to read the "
			    "root\n");
			return;
		}
		if (fs_apfs_read_block(g_apfs.ac_root_tree_bno, root) !=
		    FS_APFS_E_OK) {
			kprintf("apfs-split: FAIL the root will not read after "
			    "the split\n");
			kfree(root);
			return;
		}
		btree_layout(root, &bl);
		now = (uint32_t)bl.bl_level + 1u;
		kfree(root);
		if (now != was + (uint32_t)(deep_n - deeper)) {
			kprintf("apfs-split: FAIL the tree was %u levels deep "
			    "and is %u, and %llu levels were reported\n",
			    (unsigned)was, (unsigned)now,
			    (unsigned long long)(deep_n - deeper));
			return;
		}
		if (deep_n != deeper)
			kprintf("apfs-split: the root was full, so the tree "
			    "grew to %u levels before the leaf could split\n",
			    (unsigned)now);
		else if (split_n > splits + 1)
			kprintf("apfs-split: the leaf's parent was full, so "
			    "%llu nodes split rather than one, and the tree is "
			    "still %u levels\n",
			    (unsigned long long)(split_n - splits),
			    (unsigned)now);
	}

	kprintf("apfs-split: PASS -- leaf %llu split in two, %llu records "
	    "before and after, every separator at every level matching the "
	    "child it names, and /var/db/big.txt still resolves to %llu "
	    "bytes\n", (unsigned long long)victim, (unsigned long long)before,
	    (unsigned long long)st.afs_size);
}

/*
 * Where an inode's own record is: the leaf holding it, its slot in that leaf,
 * and how many records that leaf has.  Asked of the tree each time rather than
 * remembered, because a split between two questions moves the record into a
 * block with a different number and a slot it did not have before.
 */
static bool
inode_slot_of(uint64_t oid, uint8_t *scratch, uint64_t *bno_out,
    uint32_t *at_out, uint32_t *nkeys_out)
{
	struct btree_layout	bl;
	struct inode_locate	il;
	uint32_t		koff, klen, voff, vlen;
	uint32_t		pos;
	bool			stopped;

	il.il_oid   = oid;
	il.il_bno   = 0;
	il.il_found = false;
	stopped     = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, inode_locate, &il, 0,
	    &stopped) || !il.il_found)
		return (false);
	if (fs_apfs_read_block(il.il_bno, scratch) != FS_APFS_E_OK)
		return (false);

	btree_layout(scratch, &bl);
	for (pos = 0; pos < bl.bl_nkeys; pos++) {
		uint64_t	raw;

		btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw & APFS_J_OBJ_ID_MASK) == oid &&
		    (uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT) == APFS_TYPE_INODE)
			break;
	}
	if (pos == bl.bl_nkeys)
		return (false);

	*bno_out   = il.il_bno;
	*at_out    = pos;
	*nkeys_out = bl.bl_nkeys;
	return (true);
}

/*
 * A NODE STOPS STARTING WHERE ITS PARENT SAYS IT DOES
 *
 * Waiting for this to happen is not a test.  It needs a delete to take the
 * FIRST record out of a leaf, which depends entirely on where the splits have
 * fallen, and an image can run for twenty boots without it coming up -- and
 * then produce a volume the checker rejects, which is how it was found.
 *
 * So it is arranged, out of two files.  The leaf holding the first one's inode
 * record is split AT that record, which makes the record the upper half's
 * first and therefore the key the index above files that half under; then the
 * file is unlinked and the correction has to happen.  The second file is made
 * afterwards, so its key is just past the first's and it goes to the same
 * half: it is what stops that half emptying, which is a different question and
 * has its own test below.
 *
 * Both are taken away again, so the volume ends the boot as it began -- which
 * it could not do until a node that lost its last record could leave the tree.
 *
 * What has to survive it is the invariant apfsck states as
 *
 *	B-tree: index key absent from child node.
 *
 * and this checks it the same way the split test does, from inside, at every
 * level -- plus the counter, because an index that is right because nothing
 * needed correcting proves nothing about the correcting.
 */
void
fs_apfs_index_selftest(uint64_t now)
{
	uint8_t		*scratch;
	uint64_t	 parent;
	uint64_t	 ino_a;
	uint64_t	 ino_b;
	uint64_t	 bno;
	uint64_t	 fixed;
	uint32_t	 nkeys;
	uint32_t	 at;
	int		 is_dir;
	int		 rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-index: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup("/etc", &parent, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-index: /etc is not there -- skipped\n");
		return;
	}

	/* Whatever an interrupted run left, so this starts from nothing. */
	(void)fs_apfs_unlink(parent, "idxa.txt", now);
	(void)fs_apfs_unlink(parent, "idxb.txt", now);

	rv = fs_apfs_create(parent, "idxa.txt", now, &ino_a);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(parent, "idxb.txt", now, &ino_b);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-index: no room in /etc for the two files this "
		    "needs (%d) -- skipped\n", rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-index: FAIL the checkpoint after making them "
		    "was refused\n");
		return;
	}

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-index: no memory -- skipped\n");
		goto clean;
	}
	if (!inode_slot_of(ino_a, scratch, &bno, &at, &nkeys)) {
		kprintf("apfs-index: FAIL inode %llu is not in the tree\n",
		    (unsigned long long)ino_a);
		kfree(scratch);
		return;
	}
	if (at == 0 || bno == g_apfs.ac_root_tree_bno) {
		kprintf("apfs-index: inode %llu sits at slot %u of the node at "
		    "%llu, which cannot be split there -- skipped\n",
		    (unsigned long long)ino_a, (unsigned)at,
		    (unsigned long long)bno);
		kfree(scratch);
		goto clean;
	}

	/*
	 * Split so that the record starts the upper half.  The parent now
	 * files that half under this record's key, and the file it belongs to
	 * is about to go.
	 */
	rv = node_split_at(bno, at, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-index: the node at %llu would not split at %u "
		    "(%d) -- skipped\n", (unsigned long long)bno, (unsigned)at,
		    rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-index: FAIL the checkpoint after the split was "
		    "refused\n");
		return;
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-index: FAIL the index is already wrong, before "
		    "anything was deleted\n");
		return;
	}

	fixed = reidx_n;
	rv = fs_apfs_unlink(parent, "idxa.txt", now);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-index: FAIL cannot unlink the file whose record "
		    "starts a node (%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-index: FAIL the checkpoint after the unlink was "
		    "refused\n");
		return;
	}
	if (reidx_n == fixed) {
		kprintf("apfs-index: FAIL a node lost its first record and no "
		    "index key was corrected\n");
		goto clean;
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-index: FAIL the index is wrong after the "
		    "delete\n");
		return;
	}

	rv = fs_apfs_unlink(parent, "idxb.txt", now);
	if (rv != FS_APFS_E_OK || fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-index: FAIL cannot take the second file back "
		    "out (%d)\n", rv);
		return;
	}
	kprintf("apfs-index: PASS -- a node was made to start at inode %llu's "
	    "record, that record was deleted, and %llu index key(s) were "
	    "corrected so that every node still starts where its parent says "
	    "it does\n", (unsigned long long)ino_a,
	    (unsigned long long)(reidx_n - fixed));
	return;

clean:
	(void)fs_apfs_unlink(parent, "idxa.txt", now);
	(void)fs_apfs_unlink(parent, "idxb.txt", now);
	(void)fs_apfs_checkpoint();
}

/*
 * AND THE NODE ITSELF GOES
 *
 * The tree only ever gained nodes, and the writer refused a delete that would
 * have left an empty one behind rather than leave it -- which made an ordinary
 * unlink fail for a reason that had nothing to do with the file.
 *
 * Arranged the same way and more simply than the index test above, because a
 * freshly made file has the highest key on the volume: split the leaf holding
 * its inode record AT that record, and the upper half holds that file's
 * records and nothing else.  Unlink it and the half has to go.
 *
 * The claim is checked from three sides -- the counter moved, the tree says it
 * holds one node fewer than it did, and every node still starts where its
 * parent says it does -- because the first alone would pass on a writer that
 * unhooked the node and forgot to say so, which is precisely the state apfsck
 * calls "wrong node count in info footer".
 *
 * AND THE CASCADE IS ARRANGED TOO, when the tree is deep enough to allow it.
 * A node that empties takes its parent's last entry, and a parent left holding
 * nothing has to go the same way -- which no ordinary delete on this volume
 * reaches, because a split always leaves its parent with at least two
 * children.  So the node ABOVE the file's is split as well, at the entry that
 * names it, which leaves a node whose only child is the one about to empty.
 * Then the unlink takes two nodes out instead of one, and the count says so.
 */
void
fs_apfs_drop_selftest(uint64_t now)
{
	struct tree_path	 tp;
	struct btree_layout	 bl;
	uint8_t			*scratch;
	uint64_t		 parent;
	uint64_t		 ino;
	uint64_t		 bno;
	uint64_t		 oid;
	uint64_t		 before;
	uint64_t		 after;
	uint64_t		 dropped;
	uint32_t		 nkeys;
	uint32_t		 at;
	uint32_t		 expect;
	int			 is_dir;
	int			 rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-drop: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup("/etc", &parent, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-drop: /etc is not there -- skipped\n");
		return;
	}

	(void)fs_apfs_unlink(parent, "drop.txt", now);
	rv = fs_apfs_create(parent, "drop.txt", now, &ino);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-drop: no room in /etc for the file this needs "
		    "(%d) -- skipped\n", rv);
		(void)fs_apfs_checkpoint();
		return;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-drop: FAIL the checkpoint after making it was "
		    "refused\n");
		return;
	}

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-drop: no memory -- skipped\n");
		goto clean;
	}
	if (!inode_slot_of(ino, scratch, &bno, &at, &nkeys)) {
		kprintf("apfs-drop: FAIL inode %llu is not in the tree\n",
		    (unsigned long long)ino);
		kfree(scratch);
		return;
	}
	if (at == 0 || bno == g_apfs.ac_root_tree_bno) {
		kprintf("apfs-drop: inode %llu sits at slot %u of the node at "
		    "%llu, which cannot be split there -- skipped\n",
		    (unsigned long long)ino, (unsigned)at,
		    (unsigned long long)bno);
		kfree(scratch);
		goto clean;
	}
	rv = node_split_at(bno, at, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-drop: the node at %llu would not split at %u "
		    "(%d) -- skipped\n", (unsigned long long)bno, (unsigned)at,
		    rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-drop: FAIL the checkpoint after the split was "
		    "refused\n");
		return;
	}

	/*
	 * What the upper half holds now.  Asked rather than assumed: the file
	 * is expected to be alone in it, and a half holding anything else
	 * would not empty when the file goes -- so the test would pass without
	 * having tried what it says it tries.
	 */
	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-drop: no memory -- skipped\n");
		goto clean;
	}
	if (!inode_slot_of(ino, scratch, &bno, &at, &nkeys) || at != 0) {
		kprintf("apfs-drop: FAIL inode %llu does not start a node "
		    "after splitting there\n", (unsigned long long)ino);
		kfree(scratch);
		return;
	}
	if (nkeys > 2) {
		kprintf("apfs-drop: the node at %llu holds %u records and not "
		    "just inode %llu's, so it would not empty -- skipped\n",
		    (unsigned long long)bno, (unsigned)nkeys,
		    (unsigned long long)ino);
		kfree(scratch);
		goto clean;
	}

	/*
	 * And the node above it, split at the entry that names this one -- so
	 * that entry ends up alone in a node too, and the delete has to take
	 * two nodes out rather than one.  Every step of it is a question the
	 * tree might answer no to (the parent may BE the root, or may hold
	 * this child anywhere but last), and a no just means the simpler
	 * arrangement, which is still worth testing.
	 */
	expect = 1;
	oid    = ((const struct apfs_obj_phys *)scratch)->o_oid;

	/*
	 * A node hanging straight off the root cannot be left an only child:
	 * the root is the one node that may not go, so emptying it is refused
	 * rather than cascaded.  A tree that shallow is grown a level first --
	 * by the same operation a full root goes through -- which puts a node
	 * between the leaf and the root for the arrangement below to use.
	 */
	if (path_to(bno, &tp) && tp.tp_n < 3) {
		kprintf("apfs-drop: the tree is %u level(s) deep, so the node "
		    "above %llu is the root itself -- growing it one to have "
		    "a cascade to arrange\n", (unsigned)tp.tp_n,
		    (unsigned long long)bno);
		if (tree_grow(g_apfs.ac_xid + 1, scratch) != FS_APFS_E_OK ||
		    fs_apfs_checkpoint() != FS_APFS_E_OK)
			kprintf("apfs-drop: the tree would not grow -- no "
			    "cascade this boot\n");
	}

	if (!path_to(bno, &tp) || tp.tp_n < 3) {
		kprintf("apfs-drop: the node at %llu hangs straight off the "
		    "root, so there is no cascade to arrange\n",
		    (unsigned long long)bno);
	} else if (fs_apfs_read_block(tp.tp_bno[tp.tp_n - 2], scratch) !=
	    FS_APFS_E_OK) {
		kprintf("apfs-drop: the node above %llu will not read\n",
		    (unsigned long long)bno);
	} else {
		uint32_t	koff, klen, voff, vlen;
		uint32_t	slot;

		btree_layout(scratch, &bl);
		for (slot = 0; slot < bl.bl_nkeys; slot++) {
			btree_entry_loc(&bl, slot, &koff, &klen, &voff, &vlen);
			if (vlen == sizeof(oid) &&
			    *(const uint64_t *)(bl.bl_vals - voff) == oid)
				break;
		}
		if (slot == 0 || slot + 1 != bl.bl_nkeys) {
			kprintf("apfs-drop: oid %llu is child %u of %u under "
			    "the node at %llu, and only the last one can be "
			    "left alone there -- no cascade this boot\n",
			    (unsigned long long)oid, (unsigned)slot,
			    (unsigned)bl.bl_nkeys,
			    (unsigned long long)tp.tp_bno[tp.tp_n - 2]);
		} else {
			rv = node_split_at(tp.tp_bno[tp.tp_n - 2], slot,
			    g_apfs.ac_xid + 1, scratch);
			if (rv != FS_APFS_E_OK)
				kprintf("apfs-drop: the node above would not "
				    "split at %u (%d) -- no cascade this "
				    "boot\n", (unsigned)slot, rv);
			else if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
				kprintf("apfs-drop: FAIL the checkpoint after "
				    "the second split was refused\n");
				kfree(scratch);
				return;
			} else
				expect = 2;
		}
	}

	if (fs_apfs_read_block(g_apfs.ac_root_tree_bno, scratch) !=
	    FS_APFS_E_OK || !tree_nodes_of(scratch, &before)) {
		kprintf("apfs-drop: FAIL the tree will not say how many nodes "
		    "it has\n");
		kfree(scratch);
		return;
	}
	kfree(scratch);

	dropped = drop_n;
	rv = fs_apfs_unlink(parent, "drop.txt", now);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-drop: FAIL cannot unlink the file that is alone "
		    "in a node (%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-drop: FAIL the checkpoint after the unlink was "
		    "refused\n");
		return;
	}
	if (drop_n - dropped != expect) {
		kprintf("apfs-drop: FAIL %u node(s) should have left the tree "
		    "and %llu did\n", (unsigned)expect,
		    (unsigned long long)(drop_n - dropped));
		return;
	}
	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-drop: no memory to read the node count back\n");
		return;
	}
	if (fs_apfs_read_block(g_apfs.ac_root_tree_bno, scratch) !=
	    FS_APFS_E_OK || !tree_nodes_of(scratch, &after)) {
		kprintf("apfs-drop: FAIL the tree will not say how many nodes "
		    "it has now\n");
		kfree(scratch);
		return;
	}
	kfree(scratch);
	if (after + expect != before) {
		kprintf("apfs-drop: FAIL the tree held %llu nodes and holds "
		    "%llu after %u left it\n", (unsigned long long)before,
		    (unsigned long long)after, (unsigned)expect);
		return;
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-drop: FAIL the index is wrong after a node "
		    "left the tree\n");
		return;
	}

	kprintf("apfs-drop: PASS -- inode %llu was alone in the node at %llu%s, "
	    "and taking it away took %u node(s) with it: %llu where there were "
	    "%llu, and every one of them still starts where its parent says it "
	    "does\n", (unsigned long long)ino, (unsigned long long)bno,
	    expect > 1 ? ", which was alone under the node above it" : "",
	    (unsigned)expect, (unsigned long long)after,
	    (unsigned long long)before);
	return;

clean:
	(void)fs_apfs_unlink(parent, "drop.txt", now);
	(void)fs_apfs_checkpoint();
}

/*
 * WRITING A CHECKPOINT
 *
 * Everything above this line changes the container by writing a block back
 * where it came from.  That is legal only while the container stays in the
 * checkpoint it booted in: a block's contents and its transaction id are one
 * statement, and rewriting the first without the second is a lie the format
 * cannot catch.  It also has no crash story -- there is no instant before
 * which the change had not happened and after which it had.
 *
 * A checkpoint is that instant.  It is built entirely out of blocks nobody
 * is reading:
 *
 *	1. the ephemeral objects, copied into the next free slots of the data
 *	   ring, each carrying the new xid;
 *	2. a checkpoint map naming where they landed, into the next free slot
 *	   of the descriptor ring;
 *	3. a superblock after it, whose landing IS the commit -- before that
 *	   write the container is the old checkpoint entire, after it the new
 *	   one entire, and there is no third state;
 *	4. block zero, a copy of that superblock.
 *
 * Step 4 is not bookkeeping.  A container whose block zero names an older
 * checkpoint than the ring holds is one apfsck calls "not unmounted cleanly"
 * -- measured on a real container, not assumed: a checkpoint written without
 * it is accepted in every other respect, and adding the copy is exactly what
 * silences the complaint.  A crash between 3 and 4 leaves that state on
 * purpose: consistent, mountable at the new xid, and honestly marked as
 * having been interrupted.
 *
 * ORDERING IS NOT ASSUMED EITHER.  It is a property of the path these writes
 * take: bio_write reaches the device before it touches the cache (fs/bio.c),
 * and ata_kwrite ends every write with FLUSH CACHE (dev/ata_drv.c).  Each
 * block here is therefore on the platter before the next one starts, and the
 * order written below is the order the disk sees.  That is also why this does
 * not go through fs_txn: a transaction is a SET of blocks with no order among
 * them, and here the order is the whole meaning.
 *
 * WHAT THIS DELIBERATELY DOES NOT DO is change anything in the object trees.
 * The checkpoint it writes says what the last one said, one xid later.  That
 * is the point: the skeleton can be proven alone -- the container advances,
 * the previous checkpoint stays intact and mountable, apfsck accepts both --
 * before anything is hung on it.  Copy-on-write of metadata is the next rung,
 * and it needs this one to have somewhere to put the top of its chain.
 */

static uint64_t	ckpt_n_written;		/* checkpoints committed */
static uint64_t	ckpt_n_refused;		/* asked for and declined */

/*
 * Is slot `s` part of the run of `len` slots starting at `start` in a ring of
 * `blocks`?  Used to refuse writing a checkpoint over the one currently being
 * read -- which a ring makes possible after enough of them, and which nothing
 * later would catch, because the result checksums perfectly.
 */
static bool
slot_in_run(uint32_t s, uint32_t start, uint32_t len, uint32_t blocks)
{
	uint32_t	k;

	for (k = 0; k < len; k++) {
		if ((start + k) % blocks == s)
			return (true);
	}
	return (false);
}

int
fs_apfs_checkpoint(void)
{
	struct apfs_checkpoint_map_phys	*cpm;
	struct apfs_nx_superblock	*nx;
	struct apfs_obj_phys		*o;
	uint64_t			 moved[APFS_EPH_MAX];
	uint8_t				*buf;
	uint8_t				*map;
	uint8_t				*sb;
	const uint8_t			*src;
	uint64_t			 xid;
	uint32_t			 data_slot;
	uint32_t			 ip_slot;
	uint32_t			 map_slot;
	uint32_t			 sb_slot;
	uint32_t			 b;
	uint32_t			 i;
	int				 rv;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	ip_slot = g_apfs.ac_ipbm_slot;

	/*
	 * A checkpoint that does not re-emit the ephemeral objects is a
	 * checkpoint whose space manager is whatever the previous one left --
	 * and the previous one's slots are the next to be reused.  So a table
	 * that is empty, or that is known to be missing entries, is a reason
	 * to refuse rather than to write three quarters of a checkpoint.
	 */
	if (g_apfs.ac_eph_count == 0 || g_apfs.ac_eph_over != 0) {
		kprintf("apfs-ckpt: refusing -- %u ephemeral objects known, "
		    "%u dropped for space\n", (unsigned)g_apfs.ac_eph_count,
		    (unsigned)g_apfs.ac_eph_over);
		ckpt_n_refused++;
		return (FS_APFS_E_INVAL);
	}
	if (g_apfs.ac_xp_data_blocks == 0 ||
	    g_apfs.ac_eph_count > g_apfs.ac_xp_data_blocks ||
	    g_apfs.ac_xp_desc_blocks < 2) {
		kprintf("apfs-ckpt: refusing -- rings too small (%u desc, "
		    "%u data, %u objects)\n", (unsigned)g_apfs.ac_xp_desc_blocks,
		    (unsigned)g_apfs.ac_xp_data_blocks,
		    (unsigned)g_apfs.ac_eph_count);
		ckpt_n_refused++;
		return (FS_APFS_E_INVAL);
	}

	map_slot = g_apfs.ac_xp_desc_next % g_apfs.ac_xp_desc_blocks;
	sb_slot  = (map_slot + 1) % g_apfs.ac_xp_desc_blocks;
	if (slot_in_run(map_slot, g_apfs.ac_xp_desc_index,
	    g_apfs.ac_xp_desc_len, g_apfs.ac_xp_desc_blocks) ||
	    slot_in_run(sb_slot, g_apfs.ac_xp_desc_index,
	    g_apfs.ac_xp_desc_len, g_apfs.ac_xp_desc_blocks)) {
		kprintf("apfs-ckpt: refusing -- the descriptor ring has come "
		    "round onto the live checkpoint (slots %u,%u vs %u+%u)\n",
		    (unsigned)map_slot, (unsigned)sb_slot,
		    (unsigned)g_apfs.ac_xp_desc_index,
		    (unsigned)g_apfs.ac_xp_desc_len);
		ckpt_n_refused++;
		return (FS_APFS_E_INVAL);
	}
	for (i = 0; i < g_apfs.ac_eph_count; i++) {
		data_slot = (g_apfs.ac_xp_data_next + i) %
		    g_apfs.ac_xp_data_blocks;
		if (!slot_in_run(data_slot, g_apfs.ac_xp_data_index,
		    g_apfs.ac_xp_data_len, g_apfs.ac_xp_data_blocks))
			continue;
		kprintf("apfs-ckpt: refusing -- the data ring has come round "
		    "onto the live checkpoint (slot %u)\n",
		    (unsigned)data_slot);
		ckpt_n_refused++;
		return (FS_APFS_E_INVAL);
	}

	/*
	 * The xid to write.  The superblock states what it expects to follow
	 * it, and that should be one past its own; the two are derived
	 * differently, so a disagreement means one of the two readings is
	 * wrong and is worth saying out loud even though the answer taken is
	 * the same either way.
	 */
	xid = g_apfs.ac_xid + 1;
	if (g_apfs.ac_next_xid != 0 && g_apfs.ac_next_xid != xid)
		kprintf("apfs-ckpt: WARNING superblock expects xid %llu next, "
		    "this is %llu\n", (unsigned long long)g_apfs.ac_next_xid,
		    (unsigned long long)xid);

	buf = kmalloc(APFS_BLOCK_SIZE);
	map = kmalloc(APFS_BLOCK_SIZE);
	sb  = kmalloc(APFS_BLOCK_SIZE);
	if (buf == NULL || map == NULL || sb == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}

	/*
	 * The superblock is read FIRST, before anything is written: it is the
	 * one block whose contents this depends on, and finding it changed
	 * under us -- or unreadable -- has to stop the checkpoint while the
	 * disk is still untouched.
	 */
	if (fs_apfs_read_block(g_apfs.ac_sb_bno, sb) != FS_APFS_E_OK) {
		kprintf("apfs-ckpt: superblock at %llu unreadable\n",
		    (unsigned long long)g_apfs.ac_sb_bno);
		rv = FS_APFS_E_IO;
		goto out;
	}
	nx = (struct apfs_nx_superblock *)sb;
	if (nx->nx_o.o_xid != g_apfs.ac_xid || nx->nx_magic != APFS_NX_MAGIC) {
		kprintf("apfs-ckpt: block %llu is no longer the checkpoint we "
		    "adopted (xid %llu, wanted %llu)\n",
		    (unsigned long long)g_apfs.ac_sb_bno,
		    (unsigned long long)nx->nx_o.o_xid,
		    (unsigned long long)g_apfs.ac_xid);
		rv = FS_APFS_E_INVAL;
		goto out;
	}

	/*
	 * 0. the allocation metadata, and then the pool's own bitmap.
	 *
	 * In that order, and it is the only order that works: putting the
	 * chunk bitmap down takes two pool blocks and returns two, so a pool
	 * bitmap written before it would be written stale.  Both go before
	 * the space manager of step 1, which names what they landed in.
	 */
	if (g_apfs.ac_ip_valid) {
		/*
		 * What the queues have been holding, for anything old enough
		 * that no checkpoint worth mounting still names it.  Before
		 * the bitmap is written, because this is what changes it.
		 */
		if (xid > APFS_FQ_KEEP) {
			fq_release(APFS_SFQ_MAIN, xid - APFS_FQ_KEEP);
			fq_release(APFS_SFQ_IP, xid - APFS_FQ_KEEP);
		}
		rv = alloc_flush(xid);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs-ckpt: the allocation bitmap would not "
			    "move (%d) -- nothing is committed\n", rv);
			goto out;
		}
		rv = ip_rotate(xid, &ip_slot);
		if (rv != FS_APFS_E_OK)
			goto out;
	}

	/* 1. the ephemeral objects, into fresh data-ring slots */
	for (i = 0; i < g_apfs.ac_eph_count; i++) {
		if (g_apfs.ac_eph[i].e_size != APFS_BLOCK_SIZE) {
			kprintf("apfs-ckpt: ephemeral oid %llu is %u bytes -- "
			    "only single-block objects are handled\n",
			    (unsigned long long)g_apfs.ac_eph[i].e_oid,
			    (unsigned)g_apfs.ac_eph[i].e_size);
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		/*
		 * The space manager comes from memory, because that is where
		 * it lives; every other ephemeral object is still only ever
		 * read, so its previous copy is its current value.
		 */
		if (g_sm != NULL &&
		    g_apfs.ac_eph[i].e_oid == g_apfs.ac_spaceman_oid) {
			for (b = 0; b < APFS_BLOCK_SIZE; b++)
				buf[b] = g_sm[b];
		} else if (fq_mem(g_apfs.ac_eph[i].e_oid) != NULL) {
			src = fq_mem(g_apfs.ac_eph[i].e_oid);
			for (b = 0; b < APFS_BLOCK_SIZE; b++)
				buf[b] = src[b];
		} else if (fs_apfs_read_block(g_apfs.ac_eph[i].e_paddr, buf) !=
		    FS_APFS_E_OK) {
			kprintf("apfs-ckpt: ephemeral oid %llu at %llu "
			    "unreadable\n",
			    (unsigned long long)g_apfs.ac_eph[i].e_oid,
			    (unsigned long long)g_apfs.ac_eph[i].e_paddr);
			rv = FS_APFS_E_IO;
			goto out;
		}
		o = (struct apfs_obj_phys *)buf;
		o->o_xid = xid;
		data_slot = (g_apfs.ac_xp_data_next + i) %
		    g_apfs.ac_xp_data_blocks;
		moved[i] = g_apfs.ac_xp_data_base + data_slot;
		if (fs_apfs_write_block(moved[i], buf) != FS_APFS_E_OK) {
			kprintf("apfs-ckpt: ephemeral oid %llu would not "
			    "write to %llu\n",
			    (unsigned long long)g_apfs.ac_eph[i].e_oid,
			    (unsigned long long)moved[i]);
			rv = FS_APFS_E_IO;
			goto out;
		}
	}

	/*
	 * 2. the map.  Its oid is its own block number: it is a physical
	 * object, and for those the two are the same number by definition.
	 */
	for (i = 0; i < APFS_BLOCK_SIZE; i++)
		map[i] = 0;
	cpm = (struct apfs_checkpoint_map_phys *)map;
	cpm->cpm_o.o_oid     = g_apfs.ac_xp_desc_base + map_slot;
	cpm->cpm_o.o_xid     = xid;
	cpm->cpm_o.o_type    = APFS_OBJ_PHYSICAL | APFS_OBJ_CHECKPOINT_MAP;
	cpm->cpm_o.o_subtype = 0;
	cpm->cpm_flags       = APFS_CPM_LAST;
	cpm->cpm_count       = g_apfs.ac_eph_count;
	for (i = 0; i < g_apfs.ac_eph_count; i++) {
		cpm->cpm_map[i].cpm_type    = g_apfs.ac_eph[i].e_type;
		cpm->cpm_map[i].cpm_subtype = g_apfs.ac_eph[i].e_subtype;
		cpm->cpm_map[i].cpm_size    = g_apfs.ac_eph[i].e_size;
		cpm->cpm_map[i].cpm_pad     = 0;
		cpm->cpm_map[i].cpm_fs_oid  = g_apfs.ac_eph[i].e_fs_oid;
		cpm->cpm_map[i].cpm_oid     = g_apfs.ac_eph[i].e_oid;
		cpm->cpm_map[i].cpm_paddr   = moved[i];
	}
	if (fs_apfs_write_block(g_apfs.ac_xp_desc_base + map_slot, map) !=
	    FS_APFS_E_OK) {
		kprintf("apfs-ckpt: checkpoint map would not write to %llu\n",
		    (unsigned long long)(g_apfs.ac_xp_desc_base + map_slot));
		rv = FS_APFS_E_IO;
		goto out;
	}

	/* 3. the superblock.  Everything above it is already on the platter. */
	nx->nx_o.o_oid       = APFS_OBJ_NX_SUPERBLOCK;
	nx->nx_o.o_xid       = xid;
	nx->nx_next_xid      = xid + 1;
	/*
	 * Where the next virtual object id comes from.  A node that SPLITS
	 * needs one for its new half, and the counter is the container's
	 * rather than the volume's -- apfs_next_obj_id numbers inodes, which
	 * is a different namespace entirely and was 40 here while the tree's
	 * own nodes were already at 1125.
	 */
	nx->nx_next_oid      = g_apfs.ac_next_oid;
	/*
	 * The one pointer a copy-on-write of anything in the volume ends at.
	 * Every object between an inode and here has been copied by now, and
	 * this is the write that makes the whole chain reachable.
	 */
	nx->nx_omap_oid      = g_apfs.ac_omap_oid;
	nx->nx_xp_desc_index = map_slot;
	nx->nx_xp_desc_len   = 2;
	nx->nx_xp_desc_next  = (sb_slot + 1) % g_apfs.ac_xp_desc_blocks;
	nx->nx_xp_data_index = g_apfs.ac_xp_data_next %
	    g_apfs.ac_xp_data_blocks;
	nx->nx_xp_data_len   = g_apfs.ac_eph_count;
	nx->nx_xp_data_next  = (g_apfs.ac_xp_data_next +
	    g_apfs.ac_eph_count) % g_apfs.ac_xp_data_blocks;
	if (fs_apfs_write_block(g_apfs.ac_xp_desc_base + sb_slot, sb) !=
	    FS_APFS_E_OK) {
		kprintf("apfs-ckpt: superblock would not write to %llu -- the "
		    "container is still the previous checkpoint\n",
		    (unsigned long long)(g_apfs.ac_xp_desc_base + sb_slot));
		rv = FS_APFS_E_IO;
		goto out;
	}

	/*
	 * 4. block zero.  Past this point the checkpoint has happened
	 * whatever else fails, so a failure here is reported and not
	 * propagated: the container is the new checkpoint either way, and the
	 * only difference is whether the next fsck calls it cleanly unmounted.
	 */
	if (fs_apfs_write_block(0, sb) != FS_APFS_E_OK)
		kprintf("apfs-ckpt: xid %llu is committed, but block zero "
		    "still names %llu -- fsck will call this unclean\n",
		    (unsigned long long)xid,
		    (unsigned long long)g_apfs.ac_xid);

	/* And the container this kernel believes in moves with it. */
	g_apfs.ac_xid           = xid;
	g_apfs.ac_next_xid      = xid + 1;
	g_apfs.ac_dirty         = false;
	g_apfs.ac_sb_bno        = g_apfs.ac_xp_desc_base + sb_slot;
	g_apfs.ac_xp_desc_index = map_slot;
	g_apfs.ac_xp_desc_len   = 2;
	g_apfs.ac_xp_desc_next  = (sb_slot + 1) % g_apfs.ac_xp_desc_blocks;
	g_apfs.ac_xp_data_index = nx->nx_xp_data_index;
	g_apfs.ac_xp_data_len   = g_apfs.ac_eph_count;
	g_apfs.ac_xp_data_next  = nx->nx_xp_data_next;
	for (i = 0; i < g_apfs.ac_eph_count; i++)
		g_apfs.ac_eph[i].e_paddr = moved[i];
	if (g_apfs.ac_sm_valid)
		g_apfs.ac_sm_paddr = resolve_ephemeral(g_apfs.ac_spaceman_oid);
	/*
	 * And the pool: the bitmap that was written is now the live one, and
	 * the blocks this checkpoint released stop being held back.  Nothing
	 * in the committed container points at them any more, and the only
	 * checkpoint that did has just been superseded.
	 */
	if (g_apfs.ac_ip_valid)
		g_apfs.ac_ipbm_slot = ip_slot;

	ckpt_n_written++;
	rv = FS_APFS_E_OK;
out:
	if (rv != FS_APFS_E_OK)
		ckpt_n_refused++;
	kfree(buf);
	kfree(map);
	kfree(sb);
	return (rv);
}

/*
 * Read the disk back and ask it the four questions a checkpoint claims to
 * have settled.  Every one of them is asked of the platter rather than of
 * g_apfs: the whole failure mode worth catching here is a kernel that
 * believes it wrote a checkpoint.
 */
static int
ckpt_verify(uint64_t want_xid, uint64_t prev_sb, uint64_t prev_xid,
    void *scratch)
{
	const struct apfs_checkpoint_map_phys	*cpm;
	const struct apfs_nx_superblock		*nx;
	const struct apfs_obj_phys		*o;
	uint64_t				 newest;
	uint32_t				 i;

	/* One: block zero, which is where a fresh mount starts. */
	if (read_block_raw(0, scratch) != FS_APFS_E_OK ||
	    !block_is_nxsb(scratch)) {
		kprintf("apfs-ckpt: block zero is not a superblock\n");
		return (FS_APFS_E_IO);
	}
	nx = (const struct apfs_nx_superblock *)scratch;
	if (nx->nx_o.o_xid != want_xid) {
		kprintf("apfs-ckpt: block zero says xid %llu, wanted %llu\n",
		    (unsigned long long)nx->nx_o.o_xid,
		    (unsigned long long)want_xid);
		return (FS_APFS_E_INVAL);
	}

	/* Two: the newest superblock in the ring is the one just written. */
	newest = 0;
	for (i = 0; i < g_apfs.ac_xp_desc_blocks; i++) {
		if (read_block_raw(g_apfs.ac_xp_desc_base + i, scratch) !=
		    FS_APFS_E_OK)
			return (FS_APFS_E_IO);
		if (!block_is_nxsb(scratch))
			continue;
		nx = (const struct apfs_nx_superblock *)scratch;
		if (nx->nx_o.o_xid > newest)
			newest = nx->nx_o.o_xid;
	}
	if (newest != want_xid) {
		kprintf("apfs-ckpt: newest superblock in the ring is xid %llu, "
		    "wanted %llu\n", (unsigned long long)newest,
		    (unsigned long long)want_xid);
		return (FS_APFS_E_INVAL);
	}

	/*
	 * Three: the checkpoint this one replaced is untouched.  This is the
	 * property the whole scheme rests on -- a container that lost its
	 * previous checkpoint has no state to fall back to, and would look
	 * perfectly healthy right up to the crash that needed it.
	 */
	if (read_block_raw(prev_sb, scratch) != FS_APFS_E_OK ||
	    !block_is_nxsb(scratch)) {
		kprintf("apfs-ckpt: the previous superblock at %llu no longer "
		    "reads\n", (unsigned long long)prev_sb);
		return (FS_APFS_E_INVAL);
	}
	nx = (const struct apfs_nx_superblock *)scratch;
	if (nx->nx_o.o_xid != prev_xid) {
		kprintf("apfs-ckpt: the previous superblock at %llu now says "
		    "xid %llu, was %llu\n", (unsigned long long)prev_sb,
		    (unsigned long long)nx->nx_o.o_xid,
		    (unsigned long long)prev_xid);
		return (FS_APFS_E_INVAL);
	}

	/*
	 * Four: the map on disk names the objects we think it does, and each
	 * one is where it says and carries the new xid.  Read from the block
	 * rather than from ac_eph[], so that a map written wrong cannot be
	 * confirmed by the table it was written from.
	 */
	if (fs_apfs_read_block(g_apfs.ac_xp_desc_base + g_apfs.ac_xp_desc_index,
	    scratch) != FS_APFS_E_OK) {
		kprintf("apfs-ckpt: the new checkpoint map does not read\n");
		return (FS_APFS_E_IO);
	}
	cpm = (const struct apfs_checkpoint_map_phys *)scratch;
	if ((cpm->cpm_o.o_type & APFS_OBJ_TYPE_MASK) !=
	    APFS_OBJ_CHECKPOINT_MAP || cpm->cpm_o.o_xid != want_xid ||
	    cpm->cpm_count != g_apfs.ac_eph_count) {
		kprintf("apfs-ckpt: the new map is type 0x%x xid %llu with %u "
		    "entries, wanted a map at xid %llu with %u\n",
		    (unsigned)(cpm->cpm_o.o_type & APFS_OBJ_TYPE_MASK),
		    (unsigned long long)cpm->cpm_o.o_xid,
		    (unsigned)cpm->cpm_count, (unsigned long long)want_xid,
		    (unsigned)g_apfs.ac_eph_count);
		return (FS_APFS_E_INVAL);
	}
	for (i = 0; i < cpm->cpm_count; i++) {
		if (cpm->cpm_map[i].cpm_oid != g_apfs.ac_eph[i].e_oid ||
		    cpm->cpm_map[i].cpm_paddr != g_apfs.ac_eph[i].e_paddr) {
			kprintf("apfs-ckpt: map entry %u says oid %llu at "
			    "%llu, we recorded oid %llu at %llu\n",
			    (unsigned)i,
			    (unsigned long long)cpm->cpm_map[i].cpm_oid,
			    (unsigned long long)cpm->cpm_map[i].cpm_paddr,
			    (unsigned long long)g_apfs.ac_eph[i].e_oid,
			    (unsigned long long)g_apfs.ac_eph[i].e_paddr);
			return (FS_APFS_E_INVAL);
		}
	}
	for (i = 0; i < g_apfs.ac_eph_count; i++) {
		if (fs_apfs_read_block(g_apfs.ac_eph[i].e_paddr, scratch) !=
		    FS_APFS_E_OK) {
			kprintf("apfs-ckpt: ephemeral oid %llu at %llu does "
			    "not read back\n",
			    (unsigned long long)g_apfs.ac_eph[i].e_oid,
			    (unsigned long long)g_apfs.ac_eph[i].e_paddr);
			return (FS_APFS_E_IO);
		}
		o = (const struct apfs_obj_phys *)scratch;
		if (o->o_oid != g_apfs.ac_eph[i].e_oid ||
		    o->o_xid != want_xid) {
			kprintf("apfs-ckpt: block %llu holds oid %llu xid "
			    "%llu, the map calls it oid %llu at xid %llu\n",
			    (unsigned long long)g_apfs.ac_eph[i].e_paddr,
			    (unsigned long long)o->o_oid,
			    (unsigned long long)o->o_xid,
			    (unsigned long long)g_apfs.ac_eph[i].e_oid,
			    (unsigned long long)want_xid);
			return (FS_APFS_E_INVAL);
		}
	}
	return (FS_APFS_E_OK);
}

/*
 * Walk the whole spine from the COMMITTED superblock and check it arrives
 * where this kernel thinks it does.
 *
 * Every pointer in that chain lives in a different object, and a copy that
 * forgets to tell one of them leaves a container that still mounts, still
 * checksums, and still answers reads correctly -- out of memory.  Nothing
 * in-kernel notices until the next boot, when the chain from block zero
 * leads somewhere else.  This is that boot, asked for early.
 */
static int
spine_verify(void *buf)
{
	const struct apfs_nx_superblock	*nx;
	const struct apfs_omap_phys	*om;
	const struct apfs_superblock	*vsb;
	uint64_t			 ctr_omap;
	uint64_t			 ctr_tree;
	uint64_t			 vol_omap;
	uint64_t			 vol_sb;
	uint64_t			 vol_tree;
	uint64_t			 root;
	int				 rv;

	rv = fs_apfs_read_block(g_apfs.ac_sb_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	nx = (const struct apfs_nx_superblock *)buf;
	ctr_omap = nx->nx_omap_oid;

	rv = fs_apfs_read_block(ctr_omap, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	om = (const struct apfs_omap_phys *)buf;
	ctr_tree = om->om_tree_oid;

	rv = fs_apfs_omap_lookup(ctr_tree, g_apfs.ac_fs_oid, g_apfs.ac_xid,
	    &vol_sb);
	if (rv != FS_APFS_E_OK)
		return (rv);

	rv = fs_apfs_read_block(vol_sb, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	vsb = (const struct apfs_superblock *)buf;
	if (vsb->apfs_magic != APFS_APSB_MAGIC)
		return (FS_APFS_E_INVAL);
	vol_omap = vsb->apfs_omap_oid;

	rv = fs_apfs_read_block(vol_omap, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	om = (const struct apfs_omap_phys *)buf;
	vol_tree = om->om_tree_oid;

	rv = fs_apfs_omap_lookup(vol_tree, g_apfs.ac_root_tree_oid,
	    g_apfs.ac_xid, &root);
	if (rv != FS_APFS_E_OK)
		return (rv);

	if (ctr_omap != g_apfs.ac_omap_oid || ctr_tree !=
	    g_apfs.ac_ctr_omap_tree || vol_sb != g_apfs.ac_vol_sb_bno ||
	    vol_omap != g_apfs.ac_vol_omap_bno ||
	    vol_tree != g_apfs.ac_vol_omap_tree ||
	    root != g_apfs.ac_root_tree_bno) {
		kprintf("apfs-spine: FAIL the disk leads elsewhere -- omap "
		    "%llu/%llu, tree %llu/%llu, volume %llu/%llu, volume omap "
		    "%llu/%llu, its tree %llu/%llu, root %llu/%llu\n",
		    (unsigned long long)ctr_omap,
		    (unsigned long long)g_apfs.ac_omap_oid,
		    (unsigned long long)ctr_tree,
		    (unsigned long long)g_apfs.ac_ctr_omap_tree,
		    (unsigned long long)vol_sb,
		    (unsigned long long)g_apfs.ac_vol_sb_bno,
		    (unsigned long long)vol_omap,
		    (unsigned long long)g_apfs.ac_vol_omap_bno,
		    (unsigned long long)vol_tree,
		    (unsigned long long)g_apfs.ac_vol_omap_tree,
		    (unsigned long long)root,
		    (unsigned long long)g_apfs.ac_root_tree_bno);
		return (FS_APFS_E_INVAL);
	}
	kprintf("apfs-spine: PASS -- from block %llu: omap %llu -> tree %llu "
	    "-> volume %llu -> omap %llu -> tree %llu -> fs root %llu\n",
	    (unsigned long long)g_apfs.ac_sb_bno,
	    (unsigned long long)ctr_omap, (unsigned long long)ctr_tree,
	    (unsigned long long)vol_sb, (unsigned long long)vol_omap,
	    (unsigned long long)vol_tree, (unsigned long long)root);
	return (FS_APFS_E_OK);
}

/*
 * Write two checkpoints and check the disk after each.
 *
 * Two, not one, because the second is the only thing that tests the state
 * this kernel keeps ABOUT the checkpoint it wrote.  A writer that commits
 * perfectly and then forgets to move its ring cursors passes once and then
 * writes its second checkpoint over its first -- and the result would still
 * checksum, still mount, and still be wrong.
 */
void
fs_apfs_ckpt_selftest(void)
{
	void		*scratch;
	uint64_t	 first_xid;
	uint64_t	 prev_sb;
	uint64_t	 prev_xid;
	uint32_t	 pass;

	if (!g_apfs.ac_mounted) {
		kprintf("apfs-ckpt: no container -- skipped\n");
		return;
	}
	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-ckpt: no memory -- skipped\n");
		return;
	}

	first_xid = g_apfs.ac_xid;
	for (pass = 0; pass < 2; pass++) {
		prev_sb  = g_apfs.ac_sb_bno;
		prev_xid = g_apfs.ac_xid;
		if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
			kprintf("apfs-ckpt: FAIL checkpoint %u was refused\n",
			    (unsigned)(pass + 1));
			goto out;
		}
		if (ckpt_verify(prev_xid + 1, prev_sb, prev_xid, scratch) !=
		    FS_APFS_E_OK) {
			kprintf("apfs-ckpt: FAIL after checkpoint %u\n",
			    (unsigned)(pass + 1));
			goto out;
		}
	}

	(void)spine_verify(scratch);

	kprintf("apfs-ckpt: PASS -- xid %llu -> %llu, %u ephemeral objects "
	    "re-emitted each time, block zero follows, xid %llu still reads\n",
	    (unsigned long long)first_xid, (unsigned long long)g_apfs.ac_xid,
	    (unsigned)g_apfs.ac_eph_count, (unsigned long long)first_xid);
out:
	kfree(scratch);
}

uint64_t
fs_apfs_splits(void)
{

	return (split_n);
}

uint64_t
fs_apfs_merges(void)
{

	return (merge_n);
}

uint64_t
fs_apfs_shortens(void)
{

	return (short_n);
}

uint64_t
fs_apfs_drops(void)
{

	return (drop_n);
}

int
fs_apfs_ready(void)
{

	return (g_apfs.ac_mounted ? 1 : 0);
}

void
fs_apfs_stats(void)
{

	if (!g_apfs.ac_mounted) {
		kprintf("apfs: not mounted\n");
		return;
	}
	kprintf("apfs: %llu tree walks -- %llu nodes read, %llu records visited"
	    " (%llu nodes, %llu records each)\n",
	    (unsigned long long)g_n_walks,
	    (unsigned long long)g_n_nodes,
	    (unsigned long long)g_n_recs,
	    (unsigned long long)(g_n_walks ? g_n_nodes / g_n_walks : 0),
	    (unsigned long long)(g_n_walks ? g_n_recs / g_n_walks : 0));

	if (!g_apfs.ac_sm_valid) {
		kprintf("apfs: space manager not read\n");
		return;
	}
	kprintf("apfs: space @%llu -- %llu of %llu blocks free, %llu chunks of "
	    "%u, %u chunk-info block(s)\n",
	    (unsigned long long)g_apfs.ac_sm_paddr,
	    (unsigned long long)g_apfs.ac_sm_free,
	    (unsigned long long)g_apfs.ac_block_count,
	    (unsigned long long)g_apfs.ac_sm_chunks,
	    (unsigned)g_apfs.ac_sm_blocks_per_chunk,
	    (unsigned)g_apfs.ac_sm_cib_count);
	/*
	 * Blocks that are neither in use nor available: given up by some
	 * transaction and waiting on its age.  This is the half of allocation
	 * that a bitmap alone cannot express, and the reason freeing here is a
	 * B-tree insert rather than clearing a bit.
	 */
	kprintf("apfs: free queues -- ip %llu blk (xid %llu), main %llu blk "
	    "(xid %llu), tier2 %llu blk; internal pool %llu blk @%llu\n",
	    (unsigned long long)g_apfs.ac_sm_fq_count[APFS_SFQ_IP],
	    (unsigned long long)g_apfs.ac_sm_fq_oldest[APFS_SFQ_IP],
	    (unsigned long long)g_apfs.ac_sm_fq_count[APFS_SFQ_MAIN],
	    (unsigned long long)g_apfs.ac_sm_fq_oldest[APFS_SFQ_MAIN],
	    (unsigned long long)g_apfs.ac_sm_fq_count[APFS_SFQ_TIER2],
	    (unsigned long long)g_apfs.ac_sm_ip_blocks,
	    (unsigned long long)g_apfs.ac_sm_ip_base);

	if (!g_apfs.ac_bm_valid) {
		kprintf("apfs: allocation bitmaps not checked\n");
		return;
	}
	/*
	 * Three numbers from three places.  They are printed together, and the
	 * count of chunks NOT bit-counted is printed with them, because a
	 * verification that does not say how much of the disk it looked at is
	 * not a verification.
	 */
	kprintf("apfs: %llu chunks over %llu blocks -- %llu wholly free, "
	    "%llu bit-counted, %llu taken on trust\n",
	    (unsigned long long)g_apfs.ac_bm_chunks,
	    (unsigned long long)g_apfs.ac_bm_blocks,
	    (unsigned long long)g_apfs.ac_bm_wholly_free,
	    (unsigned long long)g_apfs.ac_bm_scanned,
	    (unsigned long long)(g_apfs.ac_bm_chunks -
	    g_apfs.ac_bm_wholly_free - g_apfs.ac_bm_scanned));
	/*
	 * The three numbers, and they must be read at the same INSTANT to
	 * mean anything.  The walk's totals are from mount; the space
	 * manager's count moves as blocks are taken and released.  Comparing
	 * one against the other two says only that the boot did some work --
	 * which it printed as a disagreement the first time a free queue let
	 * go of blocks the container arrived with.  So when the bitmap and
	 * the chunk-info are in memory, all three come from there.
	 */
	if (g_chunk_n != 0 && g_cib != NULL) {
		const struct apfs_chunk_info_block	*mcib;
		const struct alloc_chunk		*ch;
		uint64_t				 said;
		uint64_t				 bits;
		uint32_t				 i;

		mcib = (const struct apfs_chunk_info_block *)g_cib;
		said = g_apfs.ac_bm_free_said;
		bits = g_apfs.ac_bm_free_counted;
		for (i = 0; i < g_chunk_n; i++) {
			ch    = &g_chunk[i];
			said += mcib->cib_chunk_info[ch->ch_slot].ci_free_count;
			said -= ch->ch_free_admit;
			bits += bitmap_free_count(ch->ch_bm, ch->ch_blocks);
			bits -= ch->ch_bits_admit;
		}
		kprintf("apfs: free blocks -- spaceman %llu, chunks %llu, "
		    "clear bits %llu -- %s\n",
		    (unsigned long long)g_apfs.ac_sm_free,
		    (unsigned long long)said, (unsigned long long)bits,
		    (g_apfs.ac_bm_disagreed == 0 &&
		    g_apfs.ac_sm_free == said && said == bits) ?
		    "all three agree" : "THEY DISAGREE");
	} else
		kprintf("apfs: free blocks -- spaceman %llu, chunks %llu, "
		    "clear bits %llu -- %s\n",
		    (unsigned long long)g_apfs.ac_sm_free,
		    (unsigned long long)g_apfs.ac_bm_free_said,
		    (unsigned long long)g_apfs.ac_bm_free_counted,
		    (g_apfs.ac_bm_disagreed == 0 &&
		    g_apfs.ac_sm_free == g_apfs.ac_bm_free_said &&
		    g_apfs.ac_bm_free_said == g_apfs.ac_bm_free_counted) ?
		    "all three agree" : "THEY DISAGREE");

	/*
	 * Two numbers, because one of them cannot show a checkpoint that was
	 * never attempted.  A boot that writes none is a boot where nothing
	 * asked for one; a boot that refuses them says so here rather than in
	 * a line that scrolled past an hour ago.
	 */
	if (ckpt_n_written != 0 || ckpt_n_refused != 0)
		kprintf("apfs: %llu checkpoint(s) written, %llu refused -- now "
		    "at xid %llu, superblock in block %llu\n",
		    (unsigned long long)ckpt_n_written,
		    (unsigned long long)ckpt_n_refused,
		    (unsigned long long)g_apfs.ac_xid,
		    (unsigned long long)g_apfs.ac_sb_bno);

	/*
	 * The pool and the spine, and what copy-on-write has cost them.
	 */
	if (g_apfs.ac_ip_valid)
		kprintf("apfs: pool %llu+%llu -- %llu taken, %llu returned; "
		    "%llu metadata, %llu spine and %llu file blocks moved; "
		    "device %llu taken, %llu released; %llu chunk bitmap(s) "
		    "held\n",
		    (unsigned long long)g_apfs.ac_ip_base,
		    (unsigned long long)g_apfs.ac_ip_blocks,
		    (unsigned long long)ip_n_alloc,
		    (unsigned long long)ip_n_free,
		    (unsigned long long)cow_n_meta,
		    (unsigned long long)cow_n_spine,
		    (unsigned long long)cow_n_data,
		    (unsigned long long)alloc_n_taken,
		    (unsigned long long)alloc_n_given,
		    (unsigned long long)chunk_n_admit);

	/*
	 * And what the tree has had done to its shape.  The last of these is
	 * the quiet one: an index key corrected because the node under it no
	 * longer starts where its parent said it did, which is a thing a
	 * delete does without looking like it does.
	 */
	if (split_n != 0 || deep_n != 0 || reidx_n != 0 || drop_n != 0)
		kprintf("apfs: tree shape -- %llu node(s) split, %llu dropped, "
		    "%llu level(s) gained, %llu index key(s) corrected\n",
		    (unsigned long long)split_n, (unsigned long long)drop_n,
		    (unsigned long long)deep_n, (unsigned long long)reidx_n);

	/*
	 * And the queues.  The number to watch is what is still waiting: it
	 * is how many blocks this kernel is holding out of use so that the
	 * checkpoints behind the live one stay true, and it should rise while
	 * a boot works and fall as the checkpoints age out.
	 */
	/*
	 * "Let go" can exceed "queued", and legitimately: a container arrives
	 * with entries already in its queues, and this kernel releases those
	 * too.  So what is still waiting is read from the queues themselves
	 * rather than subtracted from two counters that started counting at
	 * different times.
	 */
	if (g_sm != NULL && (fq_n_queued != 0 || fq_n_released != 0))
		kprintf("apfs: free queues -- %llu blocks queued, %llu let go, "
		    "%llu still waiting (pool %llu, device %llu; oldest xid "
		    "%llu)\n", (unsigned long long)fq_n_queued,
		    (unsigned long long)fq_n_released,
		    (unsigned long long)(
		    sm_mem()->sm_fq[APFS_SFQ_IP].sfq_count +
		    sm_mem()->sm_fq[APFS_SFQ_MAIN].sfq_count),
		    (unsigned long long)sm_mem()->sm_fq[APFS_SFQ_IP].sfq_count,
		    (unsigned long long)sm_mem()->sm_fq[APFS_SFQ_MAIN].sfq_count,
		    (unsigned long long)
		    sm_mem()->sm_fq[APFS_SFQ_MAIN].sfq_oldest_xid);
}
