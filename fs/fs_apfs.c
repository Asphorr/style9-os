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
	uint64_t	ac_num_files;		/* (m) */
	uint64_t	ac_num_dirs;		/* (m) */
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
static int	spine_update(uint64_t oid, uint64_t paddr, uint64_t xid,
		    void *buf);
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
 * key order.  Space comes from the node's free span rather than from its free
 * lists -- reusing a hole would mean finding one of the right size, and the
 * span is what a node that has just been reset is all of.
 */
static int
fq_insert(uint32_t q, uint64_t xid, uint64_t paddr, uint64_t count)
{
	struct apfs_btree_node_phys		*n;
	struct apfs_spaceman_free_queue_key	*k;
	struct apfs_spaceman			*sm;
	struct fq_node				 fn;
	uint16_t				*toc;
	uint64_t				 exid;
	uint64_t				 epaddr;
	uint32_t				 need;
	uint32_t				 pos;
	uint32_t				 i;
	uint16_t				 koff;
	uint16_t				 voff;

	if (q >= APFS_SFQ_COUNT || g_fq[q] == NULL || g_sm == NULL)
		return (FS_APFS_E_INVAL);
	n = (struct apfs_btree_node_phys *)g_fq[q];
	fq_layout(g_fq[q], &fn);

	need = (uint32_t)sizeof(*k) + (count > 1 ? 8u : 0u);
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

	koff = n->btn_free_space.nl_off;
	n->btn_free_space.nl_off = (uint16_t)(koff + sizeof(*k));
	n->btn_free_space.nl_len = (uint16_t)(n->btn_free_space.nl_len -
	    sizeof(*k));
	k = (struct apfs_spaceman_free_queue_key *)(fn.fn_keys + koff);
	k->sfqk_xid   = xid;
	k->sfqk_paddr = paddr;

	if (count > 1) {
		/*
		 * Values grow down from the end of the node, so the next one
		 * sits at the top of what is left of the free span.
		 */
		n->btn_free_space.nl_len =
		    (uint16_t)(n->btn_free_space.nl_len - 8u);
		voff = (uint16_t)(APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE -
		    (APFS_BTNODE_HDR_SIZE + n->btn_table_space.nl_off +
		    fn.fn_toc_len + n->btn_free_space.nl_off +
		    n->btn_free_space.nl_len));
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
 * Point an object map's entry for `oid` at `paddr`, and copy the node.
 *
 * REPLACES the entry rather than adding one.  A map may hold several versions
 * of an oid, keyed by the transaction that made them, and that is how a
 * snapshot keeps seeing the old one; with no snapshots there is nothing to
 * keep, and replacing avoids the question of what to do when the node has no
 * room left -- which is a B-tree split, and a different rung.
 */
static int
omap_replace_cow(uint64_t node_bno, uint64_t oid, uint64_t xid, uint64_t paddr,
    void *buf, uint64_t *new_node)
{
	struct btree_layout	 bl;
	struct apfs_omap_key	*k;
	struct apfs_omap_val	*v;
	uint32_t		 koff;
	uint32_t		 voff;
	uint32_t		 i;
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
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_off(&bl, i, &koff, &voff);
		k = (struct apfs_omap_key *)(bl.bl_keys + koff);
		if (k->ok_oid != oid)
			continue;
		v = (struct apfs_omap_val *)(bl.bl_vals - voff);
		k->ok_xid   = xid;
		v->ov_paddr = paddr;
		return (cow_physical(node_bno, xid, buf, new_node));
	}
	kprintf("apfs: object map at %llu has no entry for oid %llu\n",
	    (unsigned long long)node_bno, (unsigned long long)oid);
	return (FS_APFS_E_NOTFOUND);
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
spine_update(uint64_t oid, uint64_t paddr, uint64_t xid, void *buf)
{
	struct apfs_omap_phys		*om;
	struct apfs_superblock		*vsb;
	struct apfs_obj_phys		*o;
	uint64_t			 bno;
	uint64_t			 new_ctr_omap;
	uint64_t			 new_ctr_tree;
	uint64_t			 new_vol_omap;
	uint64_t			 new_vol_tree;
	uint64_t			 new_vsb;
	int				 rv;

	/* 1. the volume's object map: oid now lives at paddr */
	rv = omap_replace_cow(g_apfs.ac_vol_omap_tree, oid, xid, paddr, buf,
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
	rv = omap_replace_cow(g_apfs.ac_ctr_omap_tree, g_apfs.ac_fs_oid, xid,
	    new_vsb, buf, &new_ctr_tree);
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
	uint64_t		 away;
	uint32_t		 run;
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
	 * AND THE FOURTH, which is the rung this test grew for.  A file's bytes
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
