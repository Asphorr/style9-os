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
#include "apfs.h"
#include "apfs_priv.h"
#include "fs_txn.h"
#include "kmem.h"
#include "kprintf.h"

/*
 * APFS container reader and writer.  See apfs.h for the format, apfs_priv.h
 * for the state this file keeps and the self-tests read.  Block I/O goes
 * through the block cache (fs/bio.c), APFS_BLOCK_SIZE / 512 sectors to a
 * block; every descent re-reads the same few blocks.
 */

#define	ATA_SECTOR_BYTES	512
#define	APFS_SECTORS_PER_BLOCK	(APFS_BLOCK_SIZE / ATA_SECTOR_BYTES)

/*
 * The mounted container.  Its shape is in apfs_priv.h because the self-tests
 * compare what they read off the disk against it.
 */
struct apfs_mount	g_apfs;

/* What reading the tree costs: whole-tree walks against keyed descents. */
uint64_t	g_n_walks;	/* reads that visited every record */
uint64_t	g_n_seeks;	/* reads that descended on a key   */
uint64_t	g_n_nodes;	/* B-tree nodes read during them   */
uint64_t	g_n_recs;	/* records handed to a callback    */
uint64_t	g_n_cmps;	/* keys compared while descending  */

/*
 * The ephemeral layer, whose home is RAM and whose disk copies are
 * per-checkpoint: the space manager and the internal pool bitmap.  Read at
 * mount, changed here, written by fs_apfs_checkpoint.
 */
static uint8_t	*g_sm;		/* the space manager        */
uint8_t	*g_fq[APFS_SFQ_COUNT];	/* its free-queue B-trees   */
static uint8_t	*g_ipbm;	/* the internal pool bitmap */


/*
 * The device's own allocation metadata, on the same terms: the resident
 * chunk bitmaps (one block covering 32768 blocks each) and their chunk-info
 * block, written by alloc_flush when a checkpoint closes and only then.  One
 * transaction can free in one chunk and allocate in another, so bitmaps are
 * admitted on demand and held until the checkpoint writes them.  The set is
 * fixed; an admission that does not fit is refused out loud, since evicting
 * a dirty bitmap would copy it into the pool now and again on its next bit
 * set.
 */
#define	APFS_CHUNKS_RESIDENT	4

static struct alloc_chunk  g_chunk[APFS_CHUNKS_RESIDENT];
static uint32_t		   g_chunk_n;	/* how many are resident     */
struct alloc_chunk *g_home;	/* where metadata comes from */
static uint8_t	*g_cib;		/* the chunk-info block      */
static uint64_t	 alloc_n_taken;	/* device blocks allocated */
static uint64_t	 alloc_n_given;	/* ...and released         */
static uint64_t	 chunk_n_admit;	/* bitmaps brought into memory */

/*
 * Space management lives below; the writers need these first.  alloc_blocks
 * takes a hint, the block the caller would like to be near; zero means
 * anywhere, which for metadata is the chunk it already lives in.
 */
int	alloc_blocks(uint32_t count, uint64_t near, uint64_t *first_out);
int	free_blocks(uint64_t first, uint32_t count);
static int	alloc_flush(uint64_t xid);
struct alloc_chunk *chunk_for(uint64_t bno);

/*
 * Everything an object map is told, in one copy of its node: separate calls
 * would copy the node, and six spine objects with it, once each.  Which
 * fields a caller fills in says what it did:
 *
 *	oe_oids/oe_paddrs	an object moved; every write is a copy.
 *	oe_new/oe_new_paddrs	an object was made, and this entry makes it
 *				reachable: one per split, two when a root
 *				splits (it keeps its own oid).
 *	oe_gone			a node that lost its last record.  A map still
 *				naming it draws "Omap record: oid-xid
 *				combination is never used".
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
static int	node_rebuild_as(const uint8_t *src, uint32_t from, uint32_t to,
		    uint8_t *dst, uint16_t flags, uint32_t type);
static int	node_rebuild(const uint8_t *src, uint32_t from, uint32_t to,
		    uint8_t *dst);
static void	tree_nodes_add(uint8_t *root, int64_t delta);
static int	extref_move(uint64_t old_start, uint64_t new_start,
		    uint64_t blocks, uint64_t xid, void *buf);
static int	fq_insert(uint32_t q, uint64_t xid, uint64_t paddr,
		    uint64_t count);
static void	fq_release(uint32_t q, uint64_t upto_xid);
uint32_t	crc32c(uint32_t crc, const uint8_t *p, uint32_t n);
static int	drec_key(uint64_t parent, const char *name, uint32_t nlen,
		    uint8_t *out, uint32_t *klen_out, bool complain);

/*
 * The transaction id, volume object map and root a read should use.  A map
 * lookup takes the newest entry no later than the xid given, and once this
 * kernel has copied something the version it wants is keyed by the
 * uncommitted transaction.  A reader pointed at an older checkpoint (apfs.h,
 * "the published past") gets all three from it; they travel together, since
 * a root resolved through another checkpoint's map finds children that map
 * never named.  g_view is the view entered, or NULL for the live volume;
 * nothing here reads the three fields it shadows except through these.
 */
static const struct fs_apfs_view	*g_view;
static uint64_t	 view_n_open;	/* checkpoints resolved into views */
static uint64_t	 view_n_enter;	/* views readers were pointed at   */
static uint64_t	 view_n_gone;	/* refused: the queue let it go    */
static uint64_t	 view_n_forbid;	/* writes asked for under a view   */

uint64_t
view_xid(void)
{

	if (g_view != NULL)
		return (g_view->av_xid);
	return (g_apfs.ac_xid + (g_apfs.ac_dirty ? 1 : 0));
}

uint64_t
view_omap(void)
{

	return (g_view != NULL ? g_view->av_omap_tree :
	    g_apfs.ac_vol_omap_tree);
}

uint64_t
view_root(void)
{

	return (g_view != NULL ? g_view->av_root_bno : g_apfs.ac_root_tree_bno);
}

/*
 * A view is read-only by construction: the three places this file changes
 * the container (a block written, taken, given back) ask this first.  The
 * volume lock already keeps views and writers apart; this catches a reader
 * silently becoming a writer.
 */
static bool
view_forbids(const char *what)
{

	if (g_view == NULL)
		return (false);
	kprintf("apfs: %s while a view of xid %llu is entered -- refused\n",
	    what, (unsigned long long)g_view->av_xid);
	view_n_forbid++;
	return (true);
}
static uint64_t	 ip_n_alloc;	/* pool blocks taken    */
static uint64_t	 ip_n_free;	/* pool blocks returned */
static uint64_t	 cow_n_meta;	/* allocation metadata blocks moved */
static uint64_t	 cow_n_spine;	/* spine objects copied            */
static uint64_t	 cow_n_data;	/* file blocks moved by a write    */
uint64_t	 split_n;	/* nodes split in two              */
static uint64_t	 merge_n;	/* appends that lengthened a run   */
static uint64_t	 short_n;	/* records shortened by a truncate */
static uint64_t	 drop_n;	/* ...and records taken out of it  */
static uint64_t	 make_n;	/* files created                   */
static uint64_t	 kill_n;	/* ...and names taken back out     */
static uint64_t	 dmake_n;	/* directories made                */
static uint64_t	 dkill_n;	/* ...and directories removed      */
static uint64_t	 move_n;	/* names moved by a rename         */
static uint64_t	 clob_n;	/* ...of them onto a taken name    */
static uint64_t	 orph_n;	/* names taken from an open file   */
static uint64_t	 reap_n;	/* ...and those files let go       */
static uint64_t	 hole_n;	/* record ends reusing a deletion  */
uint64_t	 deep_n;	/* levels the tree has gained      */
uint64_t	 reidx_n;	/* index keys corrected after an edit */
uint64_t	 gone_n;	/* emptied nodes taken out of a tree */

size_t
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
	 * The modulus is 2^32-1, not 2^32: that is what makes this Fletcher
	 * rather than a plain running sum.
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
int
read_block_raw(uint64_t bno, void *buf)
{

	if (bio_read(0, bno * APFS_SECTORS_PER_BLOCK, APFS_SECTORS_PER_BLOCK,
	    buf) != 0)
		return (FS_APFS_E_IO);
	return (FS_APFS_E_OK);
}

static void	wlog_note(uint64_t bno, bool sealed);

/*
 * Write one APFS block with no checksum work: file data has no obj_phys
 * header and nowhere to record a sum.  Every write in this file funnels
 * through here, so the write log is noted here.
 */
static int
write_block_raw(uint64_t bno, const void *buf)
{

	if (view_forbids("a block write"))
		return (FS_APFS_E_IO);
	if (bio_write(0, bno * APFS_SECTORS_PER_BLOCK, APFS_SECTORS_PER_BLOCK,
	    buf) != 0)
		return (FS_APFS_E_IO);
	wlog_note(bno, false);
	return (FS_APFS_E_OK);
}

/*
 * Diagnostic: the last WLOG_N block writes, each sealed (metadata, checksum
 * recomputed) or raw (file bytes, bitmaps).  A block that fails only its
 * checksum has a header from one version and a body from another, which is
 * what a raw write over live metadata leaves: if the failing block shows
 * here as sealed and then as raw, the allocator gave it to two owners.
 */
#define	WLOG_N		512

struct wlog_ent {
	uint64_t	w_bno;
	uint64_t	w_seq;		/* 0 == this slot never used  */
	bool		w_sealed;
};

static struct wlog_ent	wlog[WLOG_N];
static unsigned		wlog_next;
static uint64_t		wlog_seq;

static void
wlog_note(uint64_t bno, bool sealed)
{

	wlog[wlog_next].w_bno    = bno;
	wlog[wlog_next].w_seq    = ++wlog_seq;
	wlog[wlog_next].w_sealed = sealed;
	wlog_next = (wlog_next + 1) % WLOG_N;
}

/*
 * write_block_raw made the note; the one caller that sealed the block marks
 * it afterwards.  Nothing runs in between: every writer holds fs_lock.
 */
static void
wlog_seal_last(void)
{
	unsigned	prev;

	prev = (wlog_next + WLOG_N - 1) % WLOG_N;
	wlog[prev].w_sealed = true;
}

static void
wlog_report(uint64_t bno)
{
	unsigned	i;
	unsigned	found;

	found = 0;
	for (i = 0; i < WLOG_N; i++) {
		if (wlog[i].w_seq == 0 || wlog[i].w_bno != bno)
			continue;
		kprintf("apfs-autopsy:   written at #%llu, %s\n",
		    (unsigned long long)wlog[i].w_seq,
		    wlog[i].w_sealed ? "sealed" : "RAW");
		found++;
	}
	if (found == 0)
		kprintf("apfs-autopsy:   not among the last %u writes "
		    "(now at #%llu)\n", (unsigned)WLOG_N,
		    (unsigned long long)wlog_seq);
}

/*
 * No read-back-and-compare after sealed writes: over a boot, 2779 of 2779
 * came back identical, and the check doubles a boot's device I/O.
 */

/*
 * Write one metadata block, sealing it first.  The Fletcher-64 covers the
 * block from offset 8 to the end, so it is computed first and stored after.
 */
int
fs_apfs_write_block(uint64_t bno, void *buf)
{
	struct apfs_obj_phys	*o;
	int			 rv;

	o = (struct apfs_obj_phys *)buf;
	o->o_cksum = fs_apfs_fletcher64((const uint8_t *)buf + 8,
	    APFS_BLOCK_SIZE - 8);
	rv = write_block_raw(bno, buf);
	if (rv == FS_APFS_E_OK)
		wlog_seal_last();
	return (rv);
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

/*
 * A block that fails its checksum gets an autopsy, because the return code
 * cannot tell apart three failures with different fixes: wrong on the
 * platter (a torn write), right on the platter but wrong in the cache, or
 * another block's bytes (a driver that answered the wrong question).  One
 * read through the cache and one straight at the device separate them.
 */
static void
block_autopsy(uint64_t bno, const void *got)
{
	const struct apfs_obj_phys	*o;
	const uint8_t			*g;
	uint8_t				*again;
	uint8_t				*raw;
	uint64_t			 sum;
	size_t				 i;
	size_t				 first_cache;
	size_t				 first_disk;
	size_t				 ndiff_disk;
	int				 rv_cache;
	int				 rv_disk;

	o   = (const struct apfs_obj_phys *)got;
	g   = (const uint8_t *)got;
	sum = fs_apfs_fletcher64(g + 8, APFS_BLOCK_SIZE - 8);
	kprintf("apfs-autopsy: block %llu -- stored cksum %llx, computed %llx; "
	    "the bytes claim oid %llu xid %llu type %x; %u channel overlap(s), "
	    "%u lost interrupt(s) so far\n",
	    (unsigned long long)bno, (unsigned long long)o->o_cksum,
	    (unsigned long long)sum, (unsigned long long)o->o_oid,
	    (unsigned long long)o->o_xid, (unsigned)o->o_type,
	    (unsigned)ata_overlaps(), (unsigned)ata_lost_intrs());
	wlog_report(bno);

	again = kmalloc(APFS_BLOCK_SIZE);
	raw   = kmalloc(APFS_BLOCK_SIZE);
	if (again == NULL || raw == NULL) {
		kprintf("apfs-autopsy: no memory to read it back\n");
		goto out;
	}

	rv_cache = bio_read(0, bno * APFS_SECTORS_PER_BLOCK,
	    APFS_SECTORS_PER_BLOCK, again);
	rv_disk  = ata_kread(0, bno * APFS_SECTORS_PER_BLOCK,
	    APFS_SECTORS_PER_BLOCK, raw);

	first_cache = APFS_BLOCK_SIZE;
	first_disk  = APFS_BLOCK_SIZE;
	ndiff_disk  = 0;
	for (i = 0; i < APFS_BLOCK_SIZE; i++) {
		if (rv_cache == 0 && first_cache == APFS_BLOCK_SIZE &&
		    again[i] != g[i])
			first_cache = i;
		if (rv_disk == 0 && raw[i] != g[i]) {
			if (first_disk == APFS_BLOCK_SIZE)
				first_disk = i;
			ndiff_disk++;
		}
	}

	if (rv_cache != 0)
		kprintf("apfs-autopsy: the cache would not answer (rv=%d)\n",
		    rv_cache);
	else
		kprintf("apfs-autopsy: read back through the cache -- %s, "
		    "cksum %s\n",
		    first_cache == APFS_BLOCK_SIZE ? "identical" : "DIFFERENT",
		    fs_apfs_fletcher64(again + 8, APFS_BLOCK_SIZE - 8) ==
		    ((const struct apfs_obj_phys *)again)->o_cksum ?
		    "good" : "bad");
	if (rv_cache == 0 && first_cache != APFS_BLOCK_SIZE)
		kprintf("apfs-autopsy:   first difference at byte %u\n",
		    (unsigned)first_cache);

	if (rv_disk != 0) {
		kprintf("apfs-autopsy: the device would not answer (rv=%d)\n",
		    rv_disk);
		goto out;
	}
	kprintf("apfs-autopsy: read straight off the device -- %s, "
	    "cksum %s\n",
	    first_disk == APFS_BLOCK_SIZE ? "identical" : "DIFFERENT",
	    fs_apfs_fletcher64(raw + 8, APFS_BLOCK_SIZE - 8) ==
	    ((const struct apfs_obj_phys *)raw)->o_cksum ? "good" : "bad");
	if (first_disk != APFS_BLOCK_SIZE)
		kprintf("apfs-autopsy:   %u byte(s) differ, first at %u "
		    "(sector %u of 8) -- device says oid %llu xid %llu "
		    "type %x\n",
		    (unsigned)ndiff_disk, (unsigned)first_disk,
		    (unsigned)(first_disk / 512),
		    (unsigned long long)
		    ((const struct apfs_obj_phys *)raw)->o_oid,
		    (unsigned long long)
		    ((const struct apfs_obj_phys *)raw)->o_xid,
		    (unsigned)((const struct apfs_obj_phys *)raw)->o_type);

out:
	if (again != NULL)
		kfree(again);
	if (raw != NULL)
		kfree(raw);
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
	    o->o_cksum) {
		block_autopsy(bno, buf);
		return (FS_APFS_E_CKSUM);
	}
	return (FS_APFS_E_OK);
}

/*
 * Is this block a container superblock we can believe?  Checksum first (it
 * is the only thing that makes the rest of the block meaningful), then the
 * object type, then the magic.
 */
bool
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
 * Scan the checkpoint descriptor ring for the newest superblock that
 * checksums, and adopt it.  Superblocks are interleaved with checkpoint-map
 * blocks, so most slots are not superblocks.  A torn superblock is not
 * fatal: an older checkpoint is still a consistent filesystem.
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
		 * This checkpoint's descriptor blocks and the space manager's
		 * ephemeral oid, from the superblock that won, not the anchor
		 * at block 0: the anchor is a copy of some earlier checkpoint
		 * and its indices point at that one's blocks.
		 */
		g_apfs.ac_xp_desc_index = nx->nx_xp_desc_index;
		g_apfs.ac_xp_desc_len   = nx->nx_xp_desc_len;
		g_apfs.ac_spaceman_oid  = nx->nx_spaceman_oid;

		/*
		 * What only a writer needs.  A checkpoint is built by copying
		 * the superblock that closed the last one, so ac_sb_bno must be
		 * this slot: the anchor would carry older ring indices forward.
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
 * Record every ephemeral object the adopted checkpoint's descriptor blocks
 * place (oid -> block only).  nx_xp_desc_index/len name this checkpoint's
 * run in the ring, which wraps; its last block is the closing superblock
 * and the rest are checkpoint maps.
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
 * Find and read the space manager, and keep it in memory (g_sm).  Blocks a
 * transaction gives up go into a free-queue tree keyed by its xid, not back
 * to the bitmap, until no reader can still be looking at the old state.
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
	 * Cross-checks against other blocks, because a layout remembered
	 * slightly wrong gives plausible numbers.  The strongest is the last:
	 * only a correct sm_fq offset names the free-queue oids the checkpoint
	 * map did.
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
	 * Keep it: an ephemeral object lives in memory, and the checkpoint
	 * writer puts each checkpoint's copy down.
	 */
	g_sm = kmalloc(APFS_BLOCK_SIZE);
	if (g_sm == NULL)
		return (FS_APFS_E_NOMEM);
	for (i = 0; i < APFS_BLOCK_SIZE; i++)
		g_sm[i] = ((const uint8_t *)scratch)[i];
	return (FS_APFS_E_OK);
}

/*
 * The internal pool.  The chunk bitmaps and chunk-info blocks cannot live in
 * the space they describe, so they live in a small reserved pool.  Which
 * pool blocks are in use is itself a bitmap, kept in a ring so that the
 * version of a checkpoint not yet superseded is never overwritten.  On a
 * real container the pool bitmap advances one ring slot per checkpoint and
 * the chunk-info block ping-pongs between two pool blocks.
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
 * Read the pool's geometry and its live bitmap, checked against what was
 * read elsewhere: the offsets must land inside the block, the ring slot must
 * be one of the ring's, and -- strongest -- the chunk bitmap and chunk-info
 * block the chunk walk found must be inside the pool and marked taken in it.
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
	 * The three tables are offsets into this block, and a wrong one reads
	 * the block's own bytes as a table: numbers, not a fault.  Bound them.
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
	 * One chunk-info block only, so the space manager's cib_addr[] is a
	 * single number to move.  More needs the walk, not a bigger constant.
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
	 * The chunk-info block is held too.  The bitmaps it names are admitted
	 * as wanted; metadata's is admitted here, so an allocator that cannot
	 * start says so at mount rather than at the first write.
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
 * Take a pool block, or 0 if the pool is full.  A block released but not yet
 * let go by the free queue is still marked in use here.
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
 * Give a pool block back: into the pool's free queue, not by clearing its
 * bit, so nothing hands it out until its transaction is far enough behind.
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
 * Blocks have become free again: move the chunk-info and space manager
 * counters to agree with the bits the caller has cleared.
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
 * The free queues.  A block released by a copy is not free: the older
 * checkpoints the ring keeps still point at it, and one is a filesystem only
 * while its blocks are not reused.  So a release goes into a queue keyed by
 * its transaction, and the block stays marked in use until that transaction
 * is far enough behind.  Two such B-trees, device and internal pool, both
 * named by the space manager and both ephemeral.
 *
 * Measured shape: fixed-size keys (xid, paddr) of 16 bytes and values of a
 * block count, 8 bytes -- or a value offset of 0xFFFF, meaning a count of
 * one with no value stored.  mkapfs leaves both forms.
 */
#define	APFS_FQ_GHOST		0xFFFFU		/* "no value; the count is 1" */

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
 * Give a removed key or value back to the node's free list.  A node's key
 * area is the keys the table of contents points at plus a chain of holes
 * (each holding the next hole's offset and its own length), with the total
 * in the header; a checker rebuilds both ("B-tree: wrong free space total
 * for key area").  A freed key (16 bytes) or value (8) has room for its
 * hole's header.  Value offsets count back from the end of the value area.
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
 * key order.  Space comes from a hole a release left if there is one, else
 * from the free span; every hole here is the right size, so taking one is
 * popping the head of the list.  Without the reuse the span only shrinks,
 * and a long-running container runs out of it with the queue nearly empty.
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
	 * Whether there is a hole is asked of the list's total, which does not
	 * depend on which head value means "none".
	 */
	keyhole = n->btn_key_free_list.nl_len >= sizeof(*k);
	valhole = n->btn_val_free_list.nl_len >= 8u;
	need    = (keyhole ? 0u : (uint32_t)sizeof(*k)) +
	    ((count > 1 && !valhole) ? 8u : 0u);
	if ((uint32_t)(fn.fn_nkeys + 1) * 4u > fn.fn_toc_len ||
	    n->btn_free_space.nl_len < need) {
		/*
		 * The queue is one node, so its history is bounded.  Rather
		 * than lose the block, let go of everything but the newest
		 * transaction early and try once more -- loudly, since the
		 * ring's promise is broken.
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
 * The floor of the view window: the oldest checkpoint whose every block the
 * free queue still holds, so the oldest a view may be opened on.  Recorded
 * rather than computed as ac_xid - APFS_FQ_KEEP: the release happens in
 * memory at the start of a checkpoint write, and a write that then fails has
 * let go of a slice no superblock accounts for.  At mount it is ac_xid - 1:
 * a fresh mount does not know who wrote the container, and any writer that
 * can fall back after a torn checkpoint keeps the previous one's blocks.
 */
static uint64_t	fq_floor;

/*
 * Let go of everything queued at `upto_xid` or earlier: clear the bits, move
 * the counters, drop the entries.  The only place a block becomes free
 * again.  An entry at or below `upto_xid` was queued by a transaction that
 * had stopped using its block, so after this nothing older than `upto_xid`
 * is whole, which is what fq_floor records.
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

	/*
	 * The floor moves first: a queue that cannot be reached now has still
	 * been told, and the next call that reaches it lets go on these terms.
	 */
	if (upto_xid > fq_floor)
		fq_floor = upto_xid;
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
		 * Keep an entry that is too young, or whose chunk is not
		 * resident: dropping that would leave its block marked in use
		 * with nothing owing it.  It gets another go next checkpoint.
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
	 * Released entries become holes, not span; an empty queue gets its
	 * whole node back as span, with the hole chains emptied too -- left,
	 * they point into free space ("B-tree node: free key is too small").
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

/* Is there an open transaction -- changes made and no checkpoint yet? */
int
fs_apfs_dirty(void)
{

	return (g_apfs.ac_mounted && g_apfs.ac_dirty);
}

/*
 * Does the open transaction need publishing now, whatever the caller's
 * policy?  Every block an edit releases is one entry in a single-node free
 * queue, held until its transaction is APFS_FQ_KEEP checkpoints behind, so
 * closing a transaction frees nothing and the cap goes on what a policy
 * controls: the open transaction's own entries, the tail of the node's
 * (xid, paddr) order.
 *
 * The budget caps those at a fifth of the node, so a closed slice stays near
 * a third, and retention holds APFS_FQ_KEEP (2) closed slices plus the open
 * one.  The belt fires at two thirds full, or with room for fewer than
 * APFS_CKPT_HEADROOM entries: then every mutation publishes, and each
 * publish releases a slice.  Followed, this never reaches fq_insert's early
 * release.  It caps a batch at a couple of edits; more needs a queue that can
 * grow a level, and a checkpoint writer that re-emits multi-block objects.
 */
#define	APFS_CKPT_HEADROOM	32u

int
fs_apfs_ckpt_due(void)
{
	const struct apfs_btree_node_phys	*n;
	struct fq_node				 fn;
	uint64_t				 open_xid;
	uint64_t				 xid;
	uint64_t				 paddr;
	uint32_t				 cap;
	uint32_t				 open_n;
	uint32_t				 q;

	if (!g_apfs.ac_mounted || !g_apfs.ac_dirty)
		return (0);
	open_xid = g_apfs.ac_xid + 1;
	for (q = 0; q < APFS_SFQ_COUNT; q++) {
		if (g_fq[q] == NULL)
			continue;
		n = (const struct apfs_btree_node_phys *)g_fq[q];
		fq_layout(g_fq[q], &fn);
		cap = fn.fn_toc_len / 4u;

		/* the budget: the open transaction's entries are the tail */
		open_n = 0;
		while (open_n < fn.fn_nkeys) {
			fq_key(&fn, fn.fn_nkeys - 1 - open_n, &xid, &paddr);
			if (xid != open_xid)
				break;
			open_n++;
		}
		if (open_n >= cap / 5u)
			return (1);

		/* the belt: two thirds full publishes regardless of whose */
		if (fn.fn_nkeys + cap / 3u > cap)
			return (1);
		if (n->btn_free_space.nl_len < APFS_CKPT_HEADROOM *
		    (uint32_t)(sizeof(struct apfs_spaceman_free_queue_key) + 8u))
			return (1);
	}
	return (0);
}

/*
 * Put the pool's bitmap down in a fresh ring slot and tell the space manager
 * where it went.  The replaced slot goes to the tail of the free list, which
 * makes the ring a queue: older checkpoints keep their bitmaps as long as
 * the ring is deep.  Nothing here is reachable until the superblock lands.
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
	 * spelled 0xFFFF, not merely unreferenced.  A checker walks the chain
	 * and requires exactly sm_ip_bm_size_in_blocks slots missing from it;
	 * a stale link here makes the live bitmap look free.
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

/* Blocks reported free by one chunk's bitmap: the clear bits in it. */
uint32_t
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
 * Walk the chunk-info blocks and count the bits of up to APFS_BM_SCAN_MAX
 * chunks (a terabyte has 65 chunk-info blocks but 8192 chunks), saying how
 * many were skipped.  Three numbers from three places -- the space
 * manager's free count, the chunk-info counts, the clear bits -- and two
 * agreeing while the third differs says where the reader is wrong.  The
 * first chunk with room is where metadata allocation starts (ac_alloc_*).
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
	 * Chunk-info address blocks add a level this code has never read, so
	 * it declines rather than guesses.
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
				 * No bitmap: the chunk is wholly free, and one
				 * claiming otherwise means this reader has the
				 * convention backwards.
				 */
				g_apfs.ac_bm_wholly_free++;
				g_apfs.ac_bm_free_counted += ci->ci_block_count;
				if (ci->ci_free_count != ci->ci_block_count)
					g_apfs.ac_bm_disagreed++;
				continue;
			}
			if (g_apfs.ac_bm_scanned >= APFS_BM_SCAN_MAX) {
				/* Budget spent: its count is taken on trust. */
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
			 * Raw: a bitmap block is bits and nothing else.  Its
			 * first eight bytes are the state of the chunk's first
			 * 64 blocks, not a Fletcher-64, and the checked reader
			 * would reject every bitmap in the container.
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

/* The three regions of a B-tree node, as apfs_priv.h lays them out. */
void
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
void
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
	 * Bounded descent: a cycle in the child pointers is what a corrupt
	 * image would produce, and it must not spin the kernel.
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

	/* The container omap oid is physical: it is already a block number. */
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
	 * The rest of the spine, which only a writer needs: moving a block
	 * means telling whatever points at it, all the way up to the container
	 * superblock.
	 */
	g_apfs.ac_ctr_omap_tree  = ctr_omap_tree;
	g_apfs.ac_vol_sb_bno     = apsb_bno;
	g_apfs.ac_vol_omap_bno   = sb->apfs_omap_oid;
	g_apfs.ac_root_tree_oid  = sb->apfs_root_tree_oid;
	/*
	 * The other tree that names a file's blocks.  Physical, so its oid is
	 * the block it lives in.
	 */
	g_apfs.ac_extref_bno     = sb->apfs_extentref_tree_oid;
	/*
	 * The blocks this volume owns, as distinct from the container's free
	 * count.  apfsck checks it against the extents it can reach ("Volume
	 * superblock: bad block count").
	 */
	g_apfs.ac_fs_alloc_count = sb->apfs_fs_alloc_count;
	return (FS_APFS_E_OK);
}

/* ---- the published past -------------------------------------------------- */

/*
 * The superblock of checkpoint `xid`, out of the ring, into `buf`.  A slot is
 * asked its magic and xid before it is checksummed, so a view does not cost
 * a ring's worth of Fletcher-64.  The match is checksummed: a torn slot
 * carrying the right xid is the crash that interrupted that checkpoint.
 */
static int
ring_find(uint64_t xid, void *buf)
{
	const struct apfs_nx_superblock	*nx;
	uint32_t			 i;

	for (i = 0; i < g_apfs.ac_xp_desc_blocks; i++) {
		if (read_block_raw(g_apfs.ac_xp_desc_base + i, buf) !=
		    FS_APFS_E_OK)
			return (FS_APFS_E_IO);
		nx = (const struct apfs_nx_superblock *)buf;
		if (nx->nx_magic != APFS_NX_MAGIC || nx->nx_o.o_xid != xid)
			continue;
		if (!block_is_nxsb(buf))
			continue;
		return (FS_APFS_E_OK);
	}
	return (FS_APFS_E_NOTFOUND);
}

/*
 * Is `xid` a checkpoint a view may be opened on?  Three answers, in the
 * order they are cheap: not published (the open transaction, or a number
 * from nowhere), let go of (below the floor), or yes.
 */
static int
view_admits(uint64_t xid)
{

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (xid == 0 || xid > g_apfs.ac_xid)
		return (FS_APFS_E_NOTFOUND);
	if (xid < fq_floor) {
		view_n_gone++;
		return (FS_APFS_E_GONE);
	}
	return (FS_APFS_E_OK);
}

/*
 * fs_apfs_view_open repeats mount_volume's walk for an older checkpoint,
 * reading each block through this: is it the checkpoint's own?  A block the
 * queue let go of and the allocator handed out again checksums perfectly;
 * only its header says it belongs to a newer transaction or another object.
 * The floor should make that impossible; this catches the floor being wrong.
 */
static int
view_read_own(uint64_t bno, uint64_t oid, uint64_t xid, void *buf)
{
	const struct apfs_obj_phys	*o;
	int				 rv;

	rv = fs_apfs_read_block(bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	o = (const struct apfs_obj_phys *)buf;
	if (o->o_xid > xid || (oid != 0 && o->o_oid != oid)) {
		kprintf("apfs: block %llu is oid %llu at xid %llu, but the "
		    "checkpoint at xid %llu names it as oid %llu -- reused "
		    "out from under the view\n", (unsigned long long)bno,
		    (unsigned long long)o->o_oid, (unsigned long long)o->o_xid,
		    (unsigned long long)xid, (unsigned long long)oid);
		return (FS_APFS_E_INVAL);
	}
	return (FS_APFS_E_OK);
}

int
fs_apfs_view_open(uint64_t xid, struct fs_apfs_view *out)
{
	const struct apfs_nx_superblock	*nx;
	const struct apfs_superblock	*sb;
	const struct apfs_omap_phys	*om;
	uint8_t				*buf;
	uint64_t			 ctr_omap_bno;
	uint64_t			 ctr_tree;
	uint64_t			 fs_oid;
	uint64_t			 vsb_bno;
	uint64_t			 vol_omap_bno;
	uint64_t			 vol_tree;
	uint64_t			 root_oid;
	uint64_t			 root_bno;
	int				 rv;
	bool				 hashed;

	if (out == NULL)
		return (FS_APFS_E_IO);
	rv = view_admits(xid);
	if (rv != FS_APFS_E_OK)
		return (rv);
	buf = kmalloc(APFS_BLOCK_SIZE);
	if (buf == NULL)
		return (FS_APFS_E_NOMEM);

	/* the checkpoint's superblock, which names its container object map */
	rv = ring_find(xid, buf);
	if (rv != FS_APFS_E_OK)
		goto out;
	nx = (const struct apfs_nx_superblock *)buf;
	ctr_omap_bno = nx->nx_omap_oid;
	fs_oid       = nx->nx_fs_oid[0];

	/* that map, to the volume superblock as of then */
	rv = view_read_own(ctr_omap_bno, ctr_omap_bno, xid, buf);
	if (rv != FS_APFS_E_OK)
		goto out;
	om = (const struct apfs_omap_phys *)buf;
	ctr_tree = om->om_tree_oid;
	rv = fs_apfs_omap_lookup(ctr_tree, fs_oid, xid, &vsb_bno);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = view_read_own(vsb_bno, fs_oid, xid, buf);
	if (rv != FS_APFS_E_OK)
		goto out;
	sb = (const struct apfs_superblock *)buf;
	if (sb->apfs_magic != APFS_APSB_MAGIC) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	/*
	 * The key shape is a property of the volume and not of a checkpoint,
	 * and the comparator was told it once, at mount.  A checkpoint that
	 * disagreed would be a different volume under the same name.
	 */
	hashed = (sb->apfs_incompat & (APFS_INCOMPAT_CASE_INSENSITIVE |
	    APFS_INCOMPAT_NORM_INSENSITIVE)) != 0;
	if (hashed != g_apfs.ac_drec_hashed) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	vol_omap_bno = sb->apfs_omap_oid;
	root_oid     = sb->apfs_root_tree_oid;

	/* the volume's map as of then, to the root as of then */
	rv = view_read_own(vol_omap_bno, vol_omap_bno, xid, buf);
	if (rv != FS_APFS_E_OK)
		goto out;
	om = (const struct apfs_omap_phys *)buf;
	vol_tree = om->om_tree_oid;
	rv = fs_apfs_omap_lookup(vol_tree, root_oid, xid, &root_bno);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = view_read_own(root_bno, root_oid, xid, buf);
	if (rv != FS_APFS_E_OK)
		goto out;

	out->av_xid       = xid;
	out->av_omap_tree = vol_tree;
	out->av_root_bno  = root_bno;
	view_n_open++;
out:
	kfree(buf);
	return (rv);
}

int
fs_apfs_view_enter(const struct fs_apfs_view *v)
{
	int	rv;

	if (v == NULL)
		return (FS_APFS_E_IO);
	if (g_view != NULL) {
		kprintf("apfs: a view of xid %llu is already entered -- xid "
		    "%llu refused; views do not nest\n",
		    (unsigned long long)g_view->av_xid,
		    (unsigned long long)v->av_xid);
		return (FS_APFS_E_INVAL);
	}
	rv = view_admits(v->av_xid);
	if (rv != FS_APFS_E_OK)
		return (rv);
	g_view = v;
	view_n_enter++;
	return (FS_APFS_E_OK);
}

void
fs_apfs_view_leave(void)
{

	if (g_view == NULL)
		kprintf("apfs: leaving a view when none is entered\n");
	g_view = NULL;
}

uint64_t
fs_apfs_view_xid(void)
{

	return (g_view != NULL ? g_view->av_xid : 0);
}

uint64_t
fs_apfs_xid(void)
{

	return (g_apfs.ac_mounted ? g_apfs.ac_xid : 0);
}

/* What the floor stands at; the self-test says it out loud. */
uint64_t
view_floor(void)
{

	return (fq_floor);
}

int
fs_apfs_view_list(uint64_t *xids, uint32_t cap, uint32_t *n_out)
{
	const struct apfs_nx_superblock	*nx;
	uint8_t				*buf;
	uint64_t			 xid;
	uint32_t			 n;
	uint32_t			 i;
	uint32_t			 j;
	uint32_t			 k;
	int				 rv;

	if (xids == NULL || n_out == NULL)
		return (FS_APFS_E_IO);
	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	buf = kmalloc(APFS_BLOCK_SIZE);
	if (buf == NULL)
		return (FS_APFS_E_NOMEM);

	/*
	 * One pass over the ring, keeping what the window admits, in order.
	 * The window is at most APFS_FQ_KEEP + 1 wide, so insertion is sort
	 * enough.  A slot is checksummed only once it is wanted, as in
	 * ring_find.
	 */
	n  = 0;
	rv = FS_APFS_E_OK;
	for (i = 0; i < g_apfs.ac_xp_desc_blocks; i++) {
		if (read_block_raw(g_apfs.ac_xp_desc_base + i, buf) !=
		    FS_APFS_E_OK) {
			rv = FS_APFS_E_IO;
			break;
		}
		nx = (const struct apfs_nx_superblock *)buf;
		if (nx->nx_magic != APFS_NX_MAGIC)
			continue;
		xid = nx->nx_o.o_xid;
		if (xid < fq_floor || xid > g_apfs.ac_xid)
			continue;
		if (!block_is_nxsb(buf))
			continue;
		for (j = 0; j < n && xids[j] < xid; j++)
			continue;
		if (j < n && xids[j] == xid)
			continue;		/* a ring names each once */
		if (n >= cap)
			continue;
		for (k = n; k > j; k--)
			xids[k] = xids[k - 1];
		xids[j] = xid;
		n++;
	}
	*n_out = n;
	kfree(buf);
	return (rv);
}

/* ---- file-system tree ----------------------------------------------------- */

/*
 * Order two file-system tree keys: negative, zero, positive.  Records sort
 * by object id, then type (not by the raw first word, whose top bits are
 * the type), then per type: file extents by offset, directory entries by
 * name.  Keys with no rule to separate them compare equal.
 */
int
jkey_cmp(const uint8_t *a, uint32_t alen, const uint8_t *b, uint32_t blen)
{
	uint64_t	ra, rb;
	uint64_t	ida, idb;
	uint64_t	la, lb;
	uint32_t	ta, tb;

	g_n_cmps++;
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
	 * Directory entries, since a separator can be one and a split must put
	 * each record in the right half.  Hashed volumes sort by the word
	 * holding the name's length and hash, then by the name; plain ones by
	 * the name alone.  Which this volume is was settled at mount.
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
 * Where a key goes in a node the caller is holding.  An interior node is
 * asked which child to go down: the last whose separator is not greater
 * than the key, a child being filed under its first key.  A leaf is asked
 * (node_lower) for the first record not less than the key.  Binary search,
 * counted in g_n_cmps.
 *
 * A separator equal to several children's first keys would make the first
 * answer skip records, and jkey_cmp returns equal for types it has no rule
 * for; the self-test seeks every record on the volume by its own key to
 * catch that.
 */
static uint32_t
node_child_for(const struct btree_layout *bl, const uint8_t *key, uint32_t klen)
{
	uint32_t	koff, klen2, voff, vlen;
	uint32_t	lo, hi, mid;

	lo = 0;
	hi = bl->bl_nkeys;
	while (lo < hi) {
		mid = lo + (hi - lo) / 2u;
		btree_entry_loc(bl, mid, &koff, &klen2, &voff, &vlen);
		if (jkey_cmp(bl->bl_keys + koff, klen2, key, klen) <= 0)
			lo = mid + 1u;
		else
			hi = mid;
	}
	return (lo == 0 ? 0 : lo - 1u);
}

static uint32_t
node_lower(const struct btree_layout *bl, const uint8_t *key, uint32_t klen)
{
	uint32_t	koff, klen2, voff, vlen;
	uint32_t	lo, hi, mid;

	lo = 0;
	hi = bl->bl_nkeys;
	while (lo < hi) {
		mid = lo + (hi - lo) / 2u;
		btree_entry_loc(bl, mid, &koff, &klen2, &voff, &vlen);
		if (jkey_cmp(bl->bl_keys + koff, klen2, key, klen) < 0)
			lo = mid + 1u;
		else
			hi = mid;
	}
	return (lo);
}

/*
 * Read the file-system tree in order from `key`, or from the start when
 * there is none (the whole-tree walk, the self-test's oracle).  Interior
 * nodes name children by virtual oid, so every step down costs an
 * object-map lookup.
 *
 * The scan is the walk entered part-way, one recursion and one callback, so
 * the two agree about order.  The key prunes only the leftmost path: past it
 * everything to the right is wanted whole.  A caller that wants a run (one
 * file's extents, one directory's entries) returns false at the first
 * record not its own.
 */
bool
btree_scan(uint64_t bno, const uint8_t *key, uint32_t klen, apfs_rec_fn fn,
    void *arg, int depth, bool *stopped)
{
	struct btree_layout	 bl;
	uint8_t			*node;
	uint64_t		 child_oid;
	uint64_t		 child_bno;
	uint32_t		 koff;
	uint32_t		 klen2;
	uint32_t		 voff;
	uint32_t		 vlen;
	uint32_t		 first;
	uint32_t		 i;
	bool			 leaf;
	bool			 ok;

	if (depth > 8)			/* corrupt tree must not spin us */
		return (false);
	if (depth == 0) {
		if (key == NULL)
			g_n_walks++;
		else
			g_n_seeks++;
	}
	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (false);
	if (fs_apfs_read_block(bno, node) != FS_APFS_E_OK) {
		kfree(node);
		return (false);
	}
	g_n_nodes++;
	btree_layout(node, &bl);
	leaf = (bl.bl_flags & APFS_BTNODE_LEAF) != 0;

	first = 0;
	if (key != NULL && bl.bl_nkeys != 0) {
		first = leaf ? node_lower(&bl, key, klen) :
		    node_child_for(&bl, key, klen);
	}

	ok = true;
	for (i = first; i < bl.bl_nkeys && !*stopped; i++) {
		btree_entry_loc(&bl, i, &koff, &klen2, &voff, &vlen);
		if (leaf) {
			const uint8_t	*k;
			uint64_t	 raw;

			g_n_recs++;
			k = bl.bl_keys + koff;
			raw = *(const uint64_t *)k;
			if (!fn(raw & APFS_J_OBJ_ID_MASK,
			    (uint32_t)(raw >> APFS_J_OBJ_TYPE_SHIFT),
			    k, klen2, bl.bl_vals - voff, vlen, bno, arg))
				*stopped = true;
			continue;
		}
		child_oid = *(const uint64_t *)(bl.bl_vals - voff);
		if (fs_apfs_omap_lookup(view_omap(), child_oid,
		    view_xid(), &child_bno) != FS_APFS_E_OK) {
			ok = false;
			break;
		}
		if (!btree_scan(child_bno, i == first ? key : NULL, klen, fn,
		    arg, depth + 1, stopped)) {
			ok = false;
			break;
		}
	}
	kfree(node);
	return (ok);
}

/* Every record, in order: the scan with nothing to skip to. */
bool
btree_walk(uint64_t bno, apfs_rec_fn fn, void *arg, int depth, bool *stopped)
{

	return (btree_scan(bno, NULL, 0, fn, arg, depth, stopped));
}

/*
 * Which leaf a key belongs in -- the question an insert asks, about a record
 * that may not exist: the same descent, stopped at the leaf.  The self-test
 * checks it over every key on the volume against leaf_find's walk.
 */
int
leaf_home(const uint8_t *key, uint32_t klen, uint64_t *bno_out)
{
	struct btree_layout	 bl;
	uint8_t			*node;
	uint64_t		 bno;
	uint64_t		 oid;
	uint32_t		 koff, klen2, voff, vlen;
	uint32_t		 depth;
	int			 rv;

	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (FS_APFS_E_NOMEM);
	g_n_seeks++;
	bno = view_root();
	rv = FS_APFS_E_OK;
	for (depth = 0; depth < APFS_TREE_MAX_DEPTH; depth++) {
		rv = fs_apfs_read_block(bno, node);
		if (rv != FS_APFS_E_OK)
			break;
		g_n_nodes++;
		btree_layout(node, &bl);
		if ((bl.bl_flags & APFS_BTNODE_LEAF) != 0) {
			*bno_out = bno;
			kfree(node);
			return (FS_APFS_E_OK);
		}
		if (bl.bl_nkeys == 0) {
			rv = FS_APFS_E_IO;
			break;
		}
		btree_entry_loc(&bl, node_child_for(&bl, key, klen), &koff,
		    &klen2, &voff, &vlen);
		oid = *(const uint64_t *)(bl.bl_vals - voff);
		rv = fs_apfs_omap_lookup(view_omap(), oid,
		    view_xid(), &bno);
		if (rv != FS_APFS_E_OK)
			break;
	}
	kfree(node);
	return (rv != FS_APFS_E_OK ? rv : FS_APFS_E_IO);
}

/*
 * Name inside a directory-record key: the 8-byte record header, a 4-byte
 * length-and-hash (hashed volumes) or 2-byte length, then the name with the
 * NUL the length counts.  APFS_DREC_KEY_MAX is the widest such key, the
 * bound for buffers holding one built by drec_key -- the reading limit, not
 * the writing one.
 */
#define	APFS_DREC_KEY_MAX	(12u + FS_APFS_NAME_MAX + 1u)

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

/*
 * The key every one of a directory's entries sorts after: its object id, the
 * entry type, and a name word of zero.  The name word holds the hash and
 * length, or the length alone, and the length counts a trailing NUL, so no
 * real entry records zero: a scan from here begins at the directory's first
 * name on either kind of volume.
 */
static void
drec_low_key(uint64_t dir, uint8_t *out, uint32_t *klen_out)
{

	*(uint64_t *)out = (dir & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_DIR_REC << APFS_J_OBJ_TYPE_SHIFT);
	if (g_apfs.ac_drec_hashed) {
		*(uint32_t *)(out + 8) = 0;
		*klen_out = 12u;
	} else {
		*(uint16_t *)(out + 8) = 0;
		*klen_out = 10u;
	}
}

struct dirent_search {
	const char	*ds_name;
	size_t		 ds_namelen;
	uint64_t	 ds_parent;
	uint64_t	 ds_found;
	bool		 ds_is_dir;
	bool		 ds_keyed;	/* the scan started at this name */
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
	bool				 hit;

	(void)bno;
	ds = arg;
	hit = false;
	if (type == APFS_TYPE_DIR_REC && oid == ds->ds_parent &&
	    vlen >= sizeof(*dv)) {
		name = drec_name(key, klen, &nlen);
		if (name != NULL && nlen == ds->ds_namelen) {
			hit = true;
			for (i = 0; hit && i < ds->ds_namelen; i++)
				hit = name[i] == ds->ds_name[i];
		}
	}
	/*
	 * Not this name.  A read that descended on it has its answer already:
	 * the first record handed over is the entry or the one after it.  A
	 * read from the start of the tree keeps going.
	 */
	if (!hit)
		return (!ds->ds_keyed);

	dv = (const struct apfs_drec_val *)val;
	ds->ds_found  = dv->dv_file_id;
	ds->ds_is_dir = (dv->dv_flags & 0x0F) == APFS_DT_DIR;
	return (false);				/* found: stop the walk */
}

int
fs_apfs_lookup(const char *path, uint64_t *oid_out, int *is_dir_out)
{
	struct dirent_search	ds;
	uint8_t			 dkey[APFS_DREC_KEY_MAX];
	const char		*p;
	const char		*comp;
	uint64_t		 oid;
	uint32_t		 dklen;
	bool			 is_dir;
	bool			 ok;
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
		/*
		 * An entry sorts under its parent's id and its name's hash, so
		 * a component costs one descent -- unless the name cannot be
		 * folded (non-ASCII on a hashing volume): then the whole walk.
		 */
		ds.ds_keyed = drec_key(oid, comp, (uint32_t)(p - comp), dkey,
		    &dklen, false) == FS_APFS_E_OK;
		if (ds.ds_keyed)
			ok = btree_scan(view_root(), dkey, dklen,
			    dirent_match, &ds, 0, &stopped);
		else
			ok = btree_walk(view_root(), dirent_match,
			    &ds, 0, &stopped);
		if (!ok)
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
 * What an inode record tells us.  File extents are keyed on ii_private_id,
 * not the inode's own object id.  The two are equal on a freshly written
 * volume and can differ once hard links exist, so keying on the wrong one
 * works until it silently does not.
 */
struct inode_info {
	uint64_t	ii_oid;
	uint64_t	ii_parent;	/* the directory it hangs under */
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
	/*
	 * No names left, which ii_nlink cannot say: it reports 1 for a file
	 * whose record claims none, since a file off the image may claim none
	 * and still be reachable.  Asked of the record, and only ever true of
	 * a file waiting in the private directory.
	 */
	bool		ii_orphan;
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
	/*
	 * Reached by a descent on this inode's own key, so the first record
	 * handed over is either it or proof that there is none -- either way
	 * there is nothing after it worth reading.
	 */
	if (type != APFS_TYPE_INODE || oid != ii->ii_oid)
		return (false);
	if (vlen < sizeof(*iv))
		return (false);

	iv = (const struct apfs_inode_val *)val;
	ii->ii_parent     = iv->ai_parent_id;
	ii->ii_private_id = iv->ai_private_id;
	ii->ii_mode       = iv->ai_mode;
	ii->ii_uid        = iv->ai_owner;
	ii->ii_gid        = iv->ai_group;
	ii->ii_mtime      = iv->ai_mod_time;
	ii->ii_atime      = iv->ai_access_time;
	ii->ii_ctime      = iv->ai_change_time;
	ii->ii_btime      = iv->ai_create_time;
	/*
	 * One field, two meanings, told apart by the mode: a directory's
	 * children, anything else's links.  POSIX would give a directory 2
	 * plus its subdirectories, but APFS has no "." or ".." entries to
	 * count, so a directory reports 1.
	 */
	ii->ii_nlink = (iv->ai_mode & APFS_S_IFMT) == APFS_S_IFDIR ? 1u :
	    (iv->ai_nchildren_or_nlink > 0 ?
	    (uint32_t)iv->ai_nchildren_or_nlink : 1u);
	ii->ii_orphan = (iv->ai_mode & APFS_S_IFMT) != APFS_S_IFDIR &&
	    iv->ai_nchildren_or_nlink == 0;
	ii->ii_found = true;

	/*
	 * No extended fields is normal: nothing beyond the fixed part.  The
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
	uint64_t	key;
	bool		stopped;

	ii->ii_oid        = oid;
	ii->ii_parent     = 0;
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
	ii->ii_orphan     = false;
	key = (oid & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_INODE << APFS_J_OBJ_TYPE_SHIFT);
	stopped = false;
	if (!btree_scan(view_root(), (const uint8_t *)&key,
	    (uint32_t)sizeof(key), inode_pick, ii, 0, &stopped))
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
 * The key one of a data stream's runs sorts under; with `logical` zero, the
 * key all of them sort at or after.  Every read of a file's bytes starts
 * there, not at the byte it wants: a run is keyed by where it begins, so a
 * descent to an offset would land past the run covering it.
 */
static void
extent_key(uint64_t id, uint64_t logical, uint64_t *out)
{

	out[0] = (id & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_FILE_EXTENT << APFS_J_OBJ_TYPE_SHIFT);
	out[1] = logical;
}

/*
 * Copying part of a file's extents into a buffer: the byte window
 * [er_lo, er_hi) of the file, er_buf holding er_lo; a whole-file read is
 * [0, size).  er_bounce holds a partial block at the window's edges,
 * allocated once by the caller.
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
	/* Past this stream's runs, which a scan that began at them has left. */
	if (type != APFS_TYPE_FILE_EXTENT || oid != er->er_id)
		return (false);
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
	 * Past the window.  Records sort by (oid, logical), so no later record
	 * can be wanted either: stop the walk.  A whole-file read never gets
	 * here.
	 */
	if (logical >= er->er_hi)
		return (false);
	if (phys == 0)
		return (true);		/* a hole: the buffer is already zero */

	for (off = 0; off < len; off += APFS_BLOCK_SIZE) {
		dst = logical + off;		/* file offset of this block */
		/*
		 * An extent is an allocated run and can reach past the end of
		 * the file; the tail of its last block is not ours to copy.
		 */
		if (dst >= er->er_size || dst >= er->er_hi)
			break;
		if (dst + APFS_BLOCK_SIZE <= er->er_lo)
			continue;		/* entirely before the window */

		/* The slice of this block that is real content and wanted. */
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
	uint64_t		 ekey[2];
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

	/* kmalloc(0) is not worth defining; an empty file gets a byte. */
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
	extent_key(er.er_id, 0, ekey);
	stopped = false;
	if (!btree_scan(view_root(), (const uint8_t *)ekey,
	    (uint32_t)sizeof(ekey), extent_copy, &er, 0, &stopped))
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
 * Resolve a path to what a ranged read needs: the dstream id its extents are
 * keyed on, and how many of its bytes are real.  The expensive half of
 * reading, paid once per open instead of once per 4 KiB the pager asks for.
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
 * The ranged read behind fs_pread: the slurp's extent walk pointed at a
 * window.  The caller's buffer holds file byte `off`, and only the blocks
 * overlapping [off, off+len) are read; the path was resolved once, by
 * fs_apfs_open.  The buffer is zeroed first, as in the slurp, because a hole
 * has no block to read.
 */
int
fs_apfs_pread(uint64_t id, uint64_t size, uint64_t off, uint8_t *buf,
    uint32_t len, uint32_t *out_got)
{
	struct extent_read	 er;
	uint8_t			*bounce;
	uint64_t		 ekey[2];
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
	extent_key(id, 0, ekey);
	stopped = false;
	if (!btree_scan(view_root(), (const uint8_t *)ekey,
	    (uint32_t)sizeof(ekey), extent_copy, &er, 0, &stopped))
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
 * Writing a file's bytes means moving them: overwritten in place, every
 * older checkpoint in the ring would lead to the new contents.  So a write
 * takes fresh blocks, copies into them, and moves the record.
 *
 * The whole extent moves, not the part written: relocating one block would
 * split the extent in three, while moving the run keeps every count the same
 * and no tree changes shape.  The cost is the extent's size (the self-test
 * moves 152 KiB to change twelve bytes).  Both trees naming the blocks are
 * told: the file-system tree and the extent reference tree (extref_move).
 */

/* How many extents one write may move before it is refused as unbounded. */
#define	APFS_WRITE_EXTENTS_MAX	8

/*
 * The extent record covering one file offset, and the leaf holding it.  A
 * locate pass, like inode_locate below: the scan frees its node buffer on
 * the way out, so the patch re-reads the block it was told about.
 */
struct extent_locate {
	uint64_t	el_id;		/* the dstream being written   */
	uint64_t	el_want;	/* the file offset to cover    */
	uint64_t	el_logical;	/* what was found: its start   */
	uint64_t	el_len;		/* ...its length, in bytes     */
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
	/* Past this stream's runs: the offset is in none of them. */
	if (type != APFS_TYPE_FILE_EXTENT || oid != el->el_id)
		return (false);
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
int
extent_at(uint64_t id, uint64_t off, uint64_t *phys_out)
{
	struct extent_locate	el;
	uint64_t		ekey[2];
	bool			stopped;

	el.el_id    = id;
	el.el_want  = off;
	el.el_found = false;
	extent_key(id, 0, ekey);
	stopped = false;
	if (!btree_scan(view_root(), (const uint8_t *)ekey,
	    (uint32_t)sizeof(ekey), extent_locate, &el, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!el.el_found)
		return (FS_APFS_E_NOTFOUND);
	*phys_out = el.el_phys;
	return (FS_APFS_E_OK);
}

/*
 * Move the run this extent describes, putting the caller's bytes in as the
 * copy goes past them, and leave *pos at the first file byte beyond it.
 * Every block of the old run is read whole and written whole, and the
 * window only decides which bytes are replaced on the way, so partial blocks
 * at either end need no read-modify-write case.
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
	 * Near the run it replaces: the old run's chunk is resident for the
	 * release below anyway, and the file's bytes stay together.
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
	 * The record, found again in our copy of the leaf with the reader's
	 * layout code.  Only the block number in its value changes, so it
	 * stays where it is in the node.
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
	 * moved, so a failure leaves a transaction that must not be written,
	 * and both remaining steps say so.
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
	uint64_t		 ekey[2];
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
	 * Growth needs a record inserted, not just moved: refuse the whole
	 * write rather than do the prefix that fits.  A short write reporting
	 * success leaves a file half-updated with nobody told.
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
		extent_key(id, 0, ekey);
		stopped = false;
		if (!btree_scan(view_root(), (const uint8_t *)ekey,
		    (uint32_t)sizeof(ekey), extent_locate, &el, 0, &stopped)) {
			rv = FS_APFS_E_IO;
			goto out;
		}
		/*
		 * Coverage, checked: a range no extent record describes is not
		 * an error the walk reports, so an unbacked file would come
		 * back as a flawless write of nothing.
		 */
		if (!el.el_found) {
			rv = FS_APFS_E_NOALLOC;
			goto out;
		}
		/*
		 * A hole overlapping the write.  Writing one means finding it a
		 * run and giving the file a record it does not have, an insert
		 * this path does not do.
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
 * the patch re-reads it.  That keeps the scan read-only: it frees its node
 * buffer on the way out, so anything written into it would be lost.
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
		return (false);		/* the descent landed: there is none */
	il->il_bno   = bno;
	il->il_found = true;
	return (false);
}

/*
 * The leaf an inode record lives in.  An inode's key is only its object id
 * and the record type, so this is the plainest descent in the file.
 */
int
inode_where(uint64_t oid, uint64_t *bno_out)
{
	struct inode_locate	il;
	uint64_t		key;
	bool			stopped;

	key = (oid & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_INODE << APFS_J_OBJ_TYPE_SHIFT);
	il.il_oid   = oid;
	il.il_bno   = 0;
	il.il_found = false;
	stopped = false;
	if (!btree_scan(view_root(), (const uint8_t *)&key,
	    (uint32_t)sizeof(key), inode_locate, &il, 0, &stopped))
		return (FS_APFS_E_IO);
	if (!il.il_found)
		return (FS_APFS_E_NOTFOUND);
	*bno_out = il.il_bno;
	return (FS_APFS_E_OK);
}

/*
 * Amend an inode's fixed part: a touch moves the times, a chmod the
 * permission bits and the change time.  The mode is amended in its low bits
 * only: apfsck checks an inode's type against the directory entry naming it
 * ("file mode doesn't match dentry type"), and chmod(2) leaves the type.
 */
#define	INODE_AMEND_TIME	0x1u	/* modification and change times   */
#define	INODE_AMEND_MODE	0x2u	/* permission bits, and change time */

static int
inode_amend(uint64_t oid, uint32_t what, uint64_t ns, uint16_t perm)
{
	struct btree_layout	 bl;
	struct apfs_inode_val	*iv;
	struct apfs_obj_phys	*o;
	uint8_t			*node;
	const uint8_t		*k;
	uint64_t		 ino_leaf;
	uint64_t		 leaf_oid;
	uint64_t		 new_bno;
	uint64_t		 raw;
	uint64_t		 xid;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 i;
	int			 rv;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	rv = inode_where(oid, &ino_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);

	node = kmalloc(APFS_BLOCK_SIZE);
	if (node == NULL)
		return (FS_APFS_E_NOMEM);
	rv = fs_apfs_read_block(ino_leaf, node);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * Find the record again inside our own copy with the reader's layout
	 * code.  Re-deriving the offset, rather than carrying one out of the
	 * walk, keeps writer and reader from disagreeing about where a value
	 * begins -- in a root node, 40 bytes of btree_info shift it.
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
		if ((what & INODE_AMEND_TIME) != 0)
			iv->ai_mod_time = ns;
		if ((what & INODE_AMEND_MODE) != 0)
			iv->ai_mode = (uint16_t)((iv->ai_mode & APFS_S_IFMT) |
			    (perm & 07777u));
		iv->ai_change_time = ns;
		rv = FS_APFS_E_OK;
		break;
	}
	if (rv != FS_APFS_E_OK)
		goto out;			/* nothing has been written */

	/*
	 * Copy-on-write.  The node is virtual: it keeps its oid, the name the
	 * object map answers, and only its address changes (spine_update).
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
	rv = free_blocks(ino_leaf, 1);
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
	 * If what moved was the tree root, the reader's shortcut to it is now
	 * stale.  A root that is an index node never moves here, but a
	 * one-node fs tree takes this branch on its first write.
	 */
	if (leaf_oid == g_apfs.ac_root_tree_oid)
		g_apfs.ac_root_tree_bno = new_bno;
	rv = FS_APFS_E_OK;
out:
	kfree(node);
	return (rv);
}

int
fs_apfs_touch(uint64_t oid, uint64_t mtime_ns)
{

	return (inode_amend(oid, INODE_AMEND_TIME, mtime_ns, 0));
}

/*
 * chmod: the permission bits, and the change time that goes with them.  The
 * record neither moves nor changes length, so this cannot fail for want of
 * room.  It still copies the leaf and updates the spine: a block written in
 * place would be one the live checkpoint still names.
 */
int
fs_apfs_chmod(uint64_t oid, uint16_t perm, uint64_t now_ns)
{

	return (inode_amend(oid, INODE_AMEND_MODE, now_ns, perm));
}

/* ---- growing -------------------------------------------------------------- */

/*
 * A file gets longer.  This adds a record, so a tree changes shape, and
 * three records in two trees must agree afterwards:
 *
 *	the file extent    (object, offset in the file) -> run
 *	the physical extent          (first block)      -> length, owner, count
 *	the inode's dstream                             -> length, allocated
 *
 * An insert that does not fit is refused, and the caller splits the node and
 * asks again.  (The table of contents cannot grow: the key area begins where
 * it ends, so growing it would move every key.)
 */

/*
 * A hole is room: if inserts took only from the free span, a node whose
 * records come and go would lose a record's worth of room per cycle.  These
 * holes differ in size, unlike the free queue's, so the chain is searched.
 * `step` is +1 for the key area and -1 for the value area, whose offsets
 * count back from the end of the node.
 *
 * A hole is usable only if it fits exactly or leaves room for a link: a
 * smaller remainder could not stay on the chain and would be bytes belonging
 * to nothing, which apfsck counts.
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
 * Take `need` bytes out of the hole at `at` (hole_find's choice) and return
 * where they are.  They come off the far end, since a hole's link names its
 * first byte; only a hole consumed exactly is unlinked.
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
 * Put a record into a variable-KV node at the position the caller worked
 * out, or refuse if there is no room.  Keys grow up from the key area and
 * values down from the end of the node, both into the free span; either may
 * instead come out of a hole, independently of the other.
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
		    "in its table of contents -- whoever asked has to make "
		    "room and ask again\n", (unsigned)bl.bl_level,
		    (unsigned)bl.bl_nkeys);
		return (FS_APFS_E_NOALLOC);
	}

	/*
	 * Both ends are placed before either is written, so a refusal changes
	 * nothing and a key never has to be put back.
	 */
	khole = hole_find(&n->btn_key_free_list, bl.bl_keys, 1, klen);
	vhole = hole_find(&n->btn_val_free_list, bl.bl_vals, -1, vlen);
	need  = (khole == APFS_BTOFF_INVALID ? klen : 0) +
	    (vhole == APFS_BTOFF_INVALID ? vlen : 0);
	if (need > n->btn_free_space.nl_len) {
		kprintf("apfs: the node at level %u has %u bytes free and the "
		    "record needs %u -- whoever asked has to make room and ask "
		    "again\n", (unsigned)bl.bl_level,
		    (unsigned)n->btn_free_space.nl_len, (unsigned)need);
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
		 * The value's offset is measured back from the end of the node,
		 * so it is whatever is left of the free span after this value
		 * is taken off the bottom of it.
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
 * Take a record out of a variable-KV node.  Its bytes become holes on the
 * key and value chains, not free span, which is one stretch in the middle:
 * each hole holds an nloc naming the next and its own length, and the header
 * holds the head and the total, which apfsck checks.  hole_find reuses them.
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
	 * this kernel deletes is that small (shortest key eight bytes, value
	 * four), but a hole that cannot hold its own link would corrupt the
	 * chain.
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
 * How many records a tree holds is written once, in the btree_info at the end
 * of its root node, not in the leaf the record went into.  So an insert into
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
 * And the longest key and value it has ever held, in the same footer, raised
 * to cover a record longer than any before it.  Never lowered: a footer that
 * claims more than any record needs is accepted and one that claims less is
 * refused, so a delete has nothing to do here.
 */
static void
tree_longest_raise(uint8_t *root, uint32_t klen, uint32_t vlen)
{
	uint32_t	*lkey;
	uint32_t	*lval;

	lkey = (uint32_t *)(root + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE +
	    APFS_BTREE_INFO_LONGKEY);
	lval = (uint32_t *)(root + APFS_BLOCK_SIZE - APFS_BTREE_INFO_SIZE +
	    APFS_BTREE_INFO_LONGVAL);
	if (klen > *lkey)
		*lkey = klen;
	if (vlen > *lval)
		*lval = vlen;
}

/*
 * The longest key and value in one node, raising what the caller has.  A
 * fixed-KV node has none: the footer states its sizes and leaves the
 * longest-* fields zero.
 */
static void
node_longest(const uint8_t *node, uint32_t *klen, uint32_t *vlen)
{
	struct btree_layout	bl;
	uint32_t		koff, kl, voff, vl;
	uint32_t		i;

	btree_layout(node, &bl);
	if (bl.bl_fixed)
		return;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &kl, &voff, &vl);
		if (kl > *klen)
			*klen = kl;
		if (vl > *vlen)
			*vlen = vl;
	}
}

/*
 * The extent reference tree, one or two levels deep: the catalog's root
 * division without the virtual half.  A physical tree names children by
 * block, so there is no object map to tell and no oid to mint, and the
 * volume superblock names the root's address.  Every writer below reads the
 * root, follows one child when there is an index level, edits the leaf, and
 * writes both back through cow_physical, leaf first.  The root's copy of a
 * leaf's first key is refreshed on every settle: keys here are eight bytes,
 * so that is one store.
 */
struct extref_walk {
	uint64_t	ew_leaf_bno;	/* where the records are          */
	uint32_t	ew_slot;	/* the root entry naming the leaf */
	bool		ew_two;		/* the tree has an index level    */
};

static uint64_t	 extgrow_n;	/* index levels the extref tree gained */
static uint64_t	 extsplit_n;	/* extref leaves split in two          */
static uint64_t	 extdrop_n;	/* ...and emptied ones taken out       */

/*
 * Read the root and, when the tree has an index level, the leaf covering
 * `start`.  With one level the root is the leaf and the caller works in the
 * root buffer; `leaf` is not touched.
 */
static int
extref_descend(uint64_t start, uint8_t *root, uint8_t *leaf,
    struct extref_walk *ew)
{
	struct btree_layout	 bl;
	uint64_t		 raw;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 i;
	int			 rv;

	if (g_apfs.ac_extref_bno == 0)
		return (FS_APFS_E_NOTFOUND);
	rv = fs_apfs_read_block(g_apfs.ac_extref_bno, root);
	if (rv != FS_APFS_E_OK)
		return (rv);
	btree_layout(root, &bl);
	if (bl.bl_fixed)
		return (FS_APFS_E_INVAL);
	ew->ew_two      = bl.bl_level != 0;
	ew->ew_leaf_bno = g_apfs.ac_extref_bno;
	ew->ew_slot     = 0;
	if (!ew->ew_two)
		return (FS_APFS_E_OK);
	if (bl.bl_level != 1 || bl.bl_nkeys == 0) {
		kprintf("apfs: the extent reference tree at %llu is %u levels "
		    "deep with %u children -- this writer walks two\n",
		    (unsigned long long)g_apfs.ac_extref_bno,
		    (unsigned)(bl.bl_level + 1), (unsigned)bl.bl_nkeys);
		return (FS_APFS_E_INVAL);
	}
	/*
	 * The last child whose separator is no greater than the key; a key
	 * below every separator belongs to the first child, which is the same
	 * rule the catalog's descent applies.
	 */
	for (i = 1; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		if (klen < 8)
			return (FS_APFS_E_INVAL);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw & APFS_J_OBJ_ID_MASK) > start)
			break;
		ew->ew_slot = i;
	}
	btree_entry_loc(&bl, ew->ew_slot, &koff, &klen, &voff, &vlen);
	if (vlen < sizeof(uint64_t))
		return (FS_APFS_E_INVAL);
	ew->ew_leaf_bno = *(const uint64_t *)(bl.bl_vals - voff);
	return (fs_apfs_read_block(ew->ew_leaf_bno, leaf));
}

/*
 * Write the edited leaf back through cow_physical, point the root's entry
 * (value and first-key copy) at it, and copy the root, leaving its new
 * address in ac_extref_bno for the spine.  With one level, just the root.
 * apfsck accepts a stale, lower separator here, unlike in the catalog, but
 * Apple's trees keep the key exact, and so does this.
 */
static int
extref_settle(struct extref_walk *ew, uint64_t xid, uint8_t *root,
    uint8_t *leaf)
{
	struct btree_layout	 bl;
	struct btree_layout	 lb;
	uint64_t		 new_bno;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 lkoff, lklen, lvoff, lvlen;
	int			 rv;

	if (ew->ew_two) {
		rv = cow_physical(ew->ew_leaf_bno, xid, leaf, &new_bno);
		if (rv != FS_APFS_E_OK)
			return (rv);
		ew->ew_leaf_bno = new_bno;
		btree_layout(root, &bl);
		btree_entry_loc(&bl, ew->ew_slot, &koff, &klen, &voff, &vlen);
		if (klen < 8 || vlen < sizeof(uint64_t))
			return (FS_APFS_E_INVAL);
		*(uint64_t *)((uint8_t *)bl.bl_vals - voff) = new_bno;
		btree_layout(leaf, &lb);
		if (lb.bl_nkeys != 0) {
			btree_entry_loc(&lb, 0, &lkoff, &lklen, &lvoff,
			    &lvlen);
			if (lklen >= 8)
				*(uint64_t *)(bl.bl_keys + koff) =
				    *(const uint64_t *)(lb.bl_keys + lkoff);
		}
	}
	rv = cow_physical(g_apfs.ac_extref_bno, xid, root, &new_bno);
	if (rv != FS_APFS_E_OK)
		return (rv);
	g_apfs.ac_extref_bno = new_bno;
	return (FS_APFS_E_OK);
}

/*
 * The root is full and still a leaf: divide it down, as the catalog's root
 * divides.  Its records go to two new nodes at its level and it rebuilds one
 * level up, holding a separator for each.  The volume superblock names this
 * tree's root, so the root stays the root; only its address moves, as on
 * every checkpoint.
 */
static int
extref_grow_level(uint64_t xid)
{
	struct apfs_btree_node_phys	*n;
	struct btree_layout		 bl;
	uint8_t				*root;
	uint8_t				*lo;
	uint8_t				*hi;
	uint8_t				*nr;
	uint64_t			 paddrs[2];
	uint64_t			 sep;
	uint64_t			 new_bno;
	uint32_t			 half;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;

	root = kmalloc(APFS_BLOCK_SIZE);
	lo   = kmalloc(APFS_BLOCK_SIZE);
	hi   = kmalloc(APFS_BLOCK_SIZE);
	nr   = kmalloc(APFS_BLOCK_SIZE);
	if (root == NULL || lo == NULL || hi == NULL || nr == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}
	rv = fs_apfs_read_block(g_apfs.ac_extref_bno, root);
	if (rv != FS_APFS_E_OK)
		goto out;
	btree_layout(root, &bl);
	if (bl.bl_level != 0 || bl.bl_fixed || bl.bl_nkeys < 2) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	half = bl.bl_nkeys / 2;

	rv = node_rebuild_as(root, 0, half, lo,
	    (uint16_t)(bl.bl_flags & ~APFS_BTNODE_ROOT), APFS_OBJ_BTREE_NODE);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = node_rebuild_as(root, half, bl.bl_nkeys, hi,
	    (uint16_t)(bl.bl_flags & ~APFS_BTNODE_ROOT), APFS_OBJ_BTREE_NODE);
	if (rv != FS_APFS_E_OK)
		goto out;
	/*
	 * The new root is not a leaf, and the flag must say so: a level-one
	 * node flagged LEAF is "nonleaf node flagged as leaf".  A catalog root
	 * is an index already and never meets this.
	 */
	rv = node_rebuild_as(root, 0, 0, nr,
	    (uint16_t)(bl.bl_flags & ~APFS_BTNODE_LEAF), APFS_OBJ_BTREE_ROOT);
	if (rv != FS_APFS_E_OK)
		goto out;
	n = (struct apfs_btree_node_phys *)nr;
	n->btn_level = 1;

	rv = alloc_blocks(1, g_apfs.ac_extref_bno, &paddrs[0]);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = alloc_blocks(1, g_apfs.ac_extref_bno, &paddrs[1]);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(paddrs[0], 1);
		goto out;
	}
	/*
	 * Physical: each half's object id is its block number, so nothing is
	 * minted and no map is told.
	 */
	for (i = 0; i < 2; i++) {
		n = (struct apfs_btree_node_phys *)(i == 0 ? lo : hi);
		n->btn_o.o_oid = paddrs[i];
		n->btn_o.o_xid = xid;
		rv = fs_apfs_write_block(paddrs[i], i == 0 ? lo : hi);
		if (rv != FS_APFS_E_OK) {
			(void)free_blocks(paddrs[0], 1);
			(void)free_blocks(paddrs[1], 1);
			goto out;
		}
	}
	for (i = 0; i < 2; i++) {
		btree_entry_loc(&bl, i == 0 ? 0 : half, &koff, &klen, &voff,
		    &vlen);
		sep = *(const uint64_t *)(bl.bl_keys + koff);
		rv = leaf_insert(nr, i, &sep, (uint32_t)sizeof(sep),
		    &paddrs[i], (uint32_t)sizeof(paddrs[i]));
		if (rv != FS_APFS_E_OK) {
			(void)free_blocks(paddrs[0], 1);
			(void)free_blocks(paddrs[1], 1);
			goto out;
		}
	}
	tree_nodes_add(nr, 2);

	rv = cow_physical(g_apfs.ac_extref_bno, xid, nr, &new_bno);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(paddrs[0], 1);
		(void)free_blocks(paddrs[1], 1);
		goto out;
	}
	g_apfs.ac_extref_bno = new_bno;
	g_apfs.ac_fs_alloc_count += 2;
	extgrow_n++;
	kprintf("apfs: the extent reference tree grew an index level -- %u "
	    "runs split %u and %u under the root at %llu\n",
	    (unsigned)bl.bl_nkeys, (unsigned)half,
	    (unsigned)(bl.bl_nkeys - half), (unsigned long long)new_bno);
	rv = FS_APFS_E_OK;
out:
	kfree(root);
	kfree(lo);
	kfree(hi);
	kfree(nr);
	return (rv);
}

/*
 * A leaf of the two-level tree is full: split it where it stands.  The upper
 * half is a new node, the lower keeps the leaf's block (and so its place in
 * the root), and the root gains one separator.  When the root has no room
 * for it, the refusal is out loud and nothing has moved: this writer walks
 * two levels, about two hundred and fifty runs.
 */
static int
extref_split_leaf(uint64_t start, uint64_t xid)
{
	struct apfs_btree_node_phys	*n;
	struct extref_walk		 ew;
	struct btree_layout		 lb;
	uint8_t				*root;
	uint8_t				*leaf;
	uint8_t				*half;
	uint64_t			 paddr;
	uint64_t			 sep;
	uint32_t			 mid;
	uint32_t			 koff, klen, voff, vlen;
	int				 rv;

	root = kmalloc(APFS_BLOCK_SIZE);
	leaf = kmalloc(APFS_BLOCK_SIZE);
	half = kmalloc(APFS_BLOCK_SIZE);
	if (root == NULL || leaf == NULL || half == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}
	rv = extref_descend(start, root, leaf, &ew);
	if (rv != FS_APFS_E_OK)
		goto out;
	if (!ew.ew_two) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	btree_layout(leaf, &lb);
	if (lb.bl_nkeys < 2) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}
	mid = lb.bl_nkeys / 2;

	rv = node_rebuild(leaf, mid, lb.bl_nkeys, half);
	if (rv != FS_APFS_E_OK)
		goto out;
	rv = alloc_blocks(1, ew.ew_leaf_bno, &paddr);
	if (rv != FS_APFS_E_OK)
		goto out;
	n = (struct apfs_btree_node_phys *)half;
	n->btn_o.o_oid = paddr;
	n->btn_o.o_xid = xid;
	rv = fs_apfs_write_block(paddr, half);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(paddr, 1);
		goto out;
	}

	/* The separator first: the root can refuse, the rebuild cannot. */
	btree_entry_loc(&lb, mid, &koff, &klen, &voff, &vlen);
	sep = *(const uint64_t *)(lb.bl_keys + koff);
	rv = leaf_insert(root, ew.ew_slot + 1, &sep, (uint32_t)sizeof(sep),
	    &paddr, (uint32_t)sizeof(paddr));
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(paddr, 1);
		kprintf("apfs: the extent reference tree's root holds %u "
		    "children and has no room for another -- a third level "
		    "is a different rung\n",
		    (unsigned)((struct apfs_btree_node_phys *)root)->
		    btn_nkeys);
		goto out;
	}
	rv = node_rebuild(leaf, 0, mid, half);
	if (rv != FS_APFS_E_OK) {
		(void)free_blocks(paddr, 1);
		goto out;
	}
	mem_copy(leaf, half, APFS_BLOCK_SIZE);
	tree_nodes_add(root, 1);

	rv = extref_settle(&ew, xid, root, leaf);
	if (rv != FS_APFS_E_OK)
		goto out;
	g_apfs.ac_fs_alloc_count += 1;
	extsplit_n++;
	kprintf("apfs: an extent reference leaf split -- %u runs stay at "
	    "%llu, %u went to %llu\n", (unsigned)mid,
	    (unsigned long long)ew.ew_leaf_bno,
	    (unsigned)(lb.bl_nkeys - mid), (unsigned long long)paddr);
	rv = FS_APFS_E_OK;
out:
	kfree(root);
	kfree(leaf);
	kfree(half);
	return (rv);
}

/*
 * Record a newly allocated run's owner in the extent reference tree; the
 * record count is in the root's footer.  A full node divides (a root that is
 * its own leaf) or splits (a leaf under an index) and the insert is asked
 * again: a division, a split, then an insert that fits.
 */
static int
extref_insert_pv(uint64_t start, const struct apfs_phys_ext_val *pv,
    uint64_t xid)
{
	struct extref_walk	 ew;
	struct btree_layout	 bl;
	uint8_t			*root;
	uint8_t			*leaf;
	uint8_t			*node;
	uint64_t		 key;
	uint64_t		 raw;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 pos;
	uint32_t		 tries;
	int			 rv;

	root = kmalloc(APFS_BLOCK_SIZE);
	leaf = kmalloc(APFS_BLOCK_SIZE);
	if (root == NULL || leaf == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}
	key = start | ((uint64_t)APFS_TYPE_EXTENT << APFS_J_OBJ_TYPE_SHIFT);
	for (tries = 0; ; tries++) {
		rv = extref_descend(start, root, leaf, &ew);
		if (rv != FS_APFS_E_OK)
			goto out;
		node = ew.ew_two ? leaf : root;
		btree_layout(node, &bl);
		for (pos = 0; pos < bl.bl_nkeys; pos++) {
			btree_entry_loc(&bl, pos, &koff, &klen, &voff, &vlen);
			raw = *(const uint64_t *)(bl.bl_keys + koff);
			if ((raw & APFS_J_OBJ_ID_MASK) > start)
				break;
		}
		rv = leaf_insert(node, pos, &key, (uint32_t)sizeof(key), pv,
		    (uint32_t)sizeof(*pv));
		if (rv == FS_APFS_E_OK)
			break;
		if (rv != FS_APFS_E_NOALLOC || tries >= 2)
			goto out;
		rv = ew.ew_two ? extref_split_leaf(start, xid) :
		    extref_grow_level(xid);
		if (rv != FS_APFS_E_OK)
			goto out;
	}
	tree_count_add(root, 1);
	rv = extref_settle(&ew, xid, root, leaf);
out:
	kfree(root);
	kfree(leaf);
	return (rv);
}

static int
extref_insert(uint64_t start, uint64_t blocks, uint64_t owner, uint64_t xid,
    void *buf)
{
	struct apfs_phys_ext_val	 pv;

	(void)buf;
	pv.pe_len_and_kind = blocks |
	    ((uint64_t)APFS_PEXT_KIND_NEW << APFS_PEXT_KIND_SHIFT);
	pv.pe_owning_obj_id = owner;
	pv.pe_refcnt        = 1;
	return (extref_insert_pv(start, &pv, xid));
}

/*
 * A run has grown at its end: lengthen its record rather than add one for
 * touching blocks, which keeps appending from being quadratic in records.
 * The key does not change, so no separator above can go wrong.
 */
static int
extref_extend(uint64_t start, uint64_t extra, uint64_t xid, void *buf)
{
	struct apfs_phys_ext_val	*pv;
	struct extref_walk		 ew;
	struct btree_layout		 bl;
	uint8_t				*root;
	uint8_t				*node;
	uint64_t			 raw;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;

	root = kmalloc(APFS_BLOCK_SIZE);
	if (root == NULL)
		return (FS_APFS_E_NOMEM);
	rv = extref_descend(start, root, buf, &ew);
	if (rv != FS_APFS_E_OK)
		goto out;
	node = ew.ew_two ? (uint8_t *)buf : root;
	btree_layout(node, &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_EXTENT)
			continue;
		if ((raw & APFS_J_OBJ_ID_MASK) != start)
			continue;
		if (vlen < sizeof(*pv)) {
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		pv = (struct apfs_phys_ext_val *)(bl.bl_vals - voff);
		pv->pe_len_and_kind += extra;
		rv = extref_settle(&ew, xid, root, buf);
		goto out;
	}
out:
	kfree(root);
	return (rv);
}

/*
 * A run has lost blocks off its end, or all of them: say so in the extent
 * reference tree, from which apfsck recomputes what the volume owns.  The key
 * is the run's start, so this is a length in place.  A run somebody else
 * also names (a clone; none are made here) is refused, not de-referenced.
 */
static int
extref_shrink(uint64_t start, uint64_t keep, uint64_t xid, void *buf,
    bool *dropped)
{
	struct apfs_phys_ext_val	*pv;
	struct extref_walk		 ew;
	struct btree_layout		 bl;
	uint8_t				*root;
	uint8_t				*node;
	uint64_t			 raw;
	uint64_t			 new_bno;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	int				 rv;
	bool				 gone;

	*dropped = false;
	root = kmalloc(APFS_BLOCK_SIZE);
	if (root == NULL)
		return (FS_APFS_E_NOMEM);
	rv = extref_descend(start, root, buf, &ew);
	if (rv != FS_APFS_E_OK)
		goto out;
	node = ew.ew_two ? (uint8_t *)buf : root;
	btree_layout(node, &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_EXTENT)
			continue;
		if ((raw & APFS_J_OBJ_ID_MASK) != start)
			continue;
		if (vlen < sizeof(*pv)) {
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		pv = (struct apfs_phys_ext_val *)(bl.bl_vals - voff);
		if (pv->pe_refcnt != 1) {
			kprintf("apfs: the run at %llu is named %d times -- "
			    "shortening a shared run is a different rung\n",
			    (unsigned long long)start, (int)pv->pe_refcnt);
			rv = FS_APFS_E_NOALLOC;
			goto out;
		}
		gone = false;
		if (keep == 0) {
			rv = leaf_delete(node, i);
			if (rv != FS_APFS_E_OK)
				goto out;
			tree_count_add(root, -1);
			*dropped = true;
			btree_layout(node, &bl);
			gone = bl.bl_nkeys == 0;
		} else
			pv->pe_len_and_kind =
			    (pv->pe_len_and_kind & ~APFS_PEXT_LEN_MASK) | keep;

		/*
		 * A leaf with nothing in it leaves the tree: settling it would
		 * refresh the root's separator from a first record it no
		 * longer has ("index key absent from child node").  So the
		 * root's entry goes, and the block goes back.  A root left
		 * holding one empty child folds back to being its own leaf.
		 */
		if (gone && ew.ew_two) {
			struct btree_layout	 rb;

			btree_layout(root, &rb);
			if (rb.bl_nkeys > 1) {
				rv = leaf_delete(root, ew.ew_slot);
				if (rv != FS_APFS_E_OK)
					goto out;
			} else {
				struct apfs_btree_node_phys	*n;

				rv = node_rebuild_as(root, 0, 0, (uint8_t *)
				    buf, (uint16_t)(rb.bl_flags |
				    APFS_BTNODE_LEAF), APFS_OBJ_BTREE_ROOT);
				if (rv != FS_APFS_E_OK)
					goto out;
				n = (struct apfs_btree_node_phys *)buf;
				n->btn_level = 0;
				mem_copy(root, buf, APFS_BLOCK_SIZE);
				kprintf("apfs: the extent reference tree "
				    "emptied out and is its own leaf "
				    "again\n");
			}
			rv = free_blocks(ew.ew_leaf_bno, 1);
			if (rv != FS_APFS_E_OK)
				goto out;
			tree_nodes_add(root, -1);
			rv = cow_physical(g_apfs.ac_extref_bno, xid, root,
			    &new_bno);
			if (rv != FS_APFS_E_OK)
				goto out;
			g_apfs.ac_extref_bno = new_bno;
			g_apfs.ac_fs_alloc_count -= 1;
			extdrop_n++;
			goto out;
		}
		rv = extref_settle(&ew, xid, root, buf);
		goto out;
	}
out:
	kfree(root);
	return (rv);
}

/*
 * Which leaf a key belongs in, by a walk in tree order: the last one holding
 * a key no greater than it.  leaf_home answers the same question by descent;
 * the self-test asks both about every key on the volume and requires them to
 * agree.
 */
bool
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
 * A node runs out of room.  A split makes a new object, and of the three
 * counters involved two are traps:
 *
 *	nx_next_oid       the container's: the source of a new virtual oid
 *	apfs_next_obj_id  the volume's, numbering inodes -- another namespace
 *	bt_node_count     in the root's footer, one more after a split;
 *	                  bt_key_count does not move
 *
 * The new half is virtual, so writing it does not make it reachable: the
 * volume's object map gains an entry and the parent a separator.
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
	 * Asked the way leaf_insert answers it, holes and all -- except for a
	 * fixed-KV node, which leaf_insert_fixed fills from the span only
	 * (omap_slot_drop leaves no holes behind).
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
 * The same insert, for a node whose keys and values are all one size (the
 * object map, which a split needs an entry in).  The arithmetic is
 * leaf_insert's; only the table entry differs, four bytes against eight.
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
 * Build a node holding records [from, to) of another, laid out afresh and
 * saying what kind of node it is.  Re-inserting each record gives a layout
 * correct by construction; half a node copied verbatim would carry a hole
 * chain describing space no longer in it.  The kind is an argument because
 * a root's halves are not roots: the ROOT flag (which also decides whether
 * the last 40 bytes are a btree_info), the object type and the level must
 * all say so ("B-tree node: wrong object type for root" / "for nonroot").
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
	 * No holes, and the chains must say so.  0xFFFF is the format's "no
	 * such offset"; a zero would name the first byte of the key area as a
	 * hole, and a checker would read a live record as a hole header
	 * ("B-tree node: free key is too small").
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
 * How many nodes a tree has, which lives beside the record count in the
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
 * Checked: the last 40 bytes are a btree_info only in a node carrying the
 * ROOT flag, and records in any other.
 */
bool
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
 * Who is above a node, found by following child pointers until the block
 * turns up rather than by descending on its key: the two agree only while
 * the tree is in order, which the caller is in the middle of maintaining.
 * A handful of reads per split.  The path is in apfs_priv.h because the
 * split test reads one.
 */
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
			if (fs_apfs_omap_lookup(view_omap(), oid,
			    view_xid(), &child) != FS_APFS_E_OK)
				break;
			found = path_walk(child, want, tp, depth + 1);
		}
	}
	kfree(node);
	return (found);
}

bool
path_to(uint64_t want, struct tree_path *tp)
{

	tp->tp_n = 0;
	return (path_walk(view_root(), want, tp, 0));
}

/*
 * More than one node at a time.  A name's records are never neighbours (an
 * entry sorts under the parent's id, the inode under the child's), and a
 * split can fall between a file's inode and its extents.  So every writer
 * here names the leaves, reads them all, edits them in memory, and commits:
 * a refusal costs nothing.  One copy per node per transaction: the leaves
 * are de-duplicated as they are named.
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
 * there is no room for another, which the caller must check.
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
 * Empty, and safe to free.  Called before the first leaf is named, because
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
 * Has any of it reached the disk yet?  A commit refuses before its first
 * copy, so a caller whose commit was refused must put back the volume
 * counters it adjusted; once a copy has landed, the checkpoint must not be
 * written at all.
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
 * A node that has lost its last record leaves the tree: with no first key it
 * cannot be filed above.  Five things, each with the complaint its omission
 * draws:
 *
 *	the parent stops naming it	B-tree: keys are out of order
 *	the object map forgets its oid	Omap record: oid-xid combination is
 *					never used
 *	its block goes back		Space manager: bad allocation bitmap
 *	the tree counts one node fewer	Catalog: wrong node count in info footer
 *	the volume owns one block fewer	Volume superblock: bad block count
 *
 * This does the first; edit_commit does the rest.  The node is not copied,
 * only marked gone.  A level is never given back (a root with one child is
 * accepted), but the cascade happens: the parent joins the edit, and if it
 * is now empty the pass reaches it.
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
	 * The root cannot go, since the volume superblock names it, so a tree
	 * whose last leaf has emptied stops here.  Nothing reaches this while
	 * the volume holds anything: the root directory's own records are in
	 * it.
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
	gone_n++;
	kprintf("apfs: the node at %llu lost its last record and left the tree "
	    "-- oid %llu names nothing now, and the node at %llu holds %u\n",
	    (unsigned long long)ne->le_bno[i],
	    (unsigned long long)ne->le_oid[i],
	    (unsigned long long)ne->le_bno[pslot], (unsigned)(bl.bl_nkeys - 1));
	return (FS_APFS_E_OK);
}

/*
 * A node's first key is also its parent's business: an index node files a
 * child under the child's first key ("B-tree: index key absent from child
 * node"), so deleting or inserting at the front of a leaf leaves the parent
 * stale.  Each node in the edit is compared against its parent's entry and
 * the parent corrected, which can cascade up to the root, a corrected parent
 * joining the edit.  Keys differ in size, so the key is replaced by a delete
 * and an insert in the same slot, which can fail for room: this runs before
 * anything is copied.
 */
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
	 * Repeat until nothing changes: correcting a parent adds it to the
	 * list (so the bound is re-read), and a parent already passed can be
	 * emptied by its last child going.  A pass that moves neither le_n nor
	 * le_dropped is done.
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
	uint32_t		klen;
	uint32_t		vlen;
	uint32_t		i;
	int			rv;

	/*
	 * First, in memory: the reindex is the one thing here that can still
	 * refuse, and it can add nodes to the list about to be copied.  It is
	 * also where an emptied node leaves the tree, taking it out of that
	 * list, so the two counts below are not both le_n.
	 */
	rv = edit_reindex(ne, scratch);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/*
	 * The longest record in this edit, measured after the reindex: an
	 * index key is a copy of a leaf's first key, so a long key at the
	 * start of a leaf puts one in the node above too, which no caller
	 * could report about itself.
	 */
	klen = 0;
	vlen = 0;
	for (i = 0; i < ne->le_n; i++)
		if (!ne->le_gone[i])
			node_longest(ne->le_node[i], &klen, &vlen);

	if (ne->le_root < ne->le_n) {
		tree_count_add(ne->le_node[ne->le_root], records);
		tree_nodes_add(ne->le_node[ne->le_root],
		    -(int64_t)ne->le_dropped);
		tree_longest_raise(ne->le_node[ne->le_root], klen, vlen);
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
		tree_longest_raise(scratch, klen, vlen);
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
	 * so before the spine runs: the volume superblock carrying that count
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
 * The tree gains a level.  Nothing can go above the root, which the volume
 * superblock names by oid, so it splits downward: its records go into two
 * new nodes, and it is rebuilt one level higher naming them, keeping its
 * oid.  What apfsck says to each obligation left out:
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
 * nx_next_oid, like apfs_next_obj_id, asserts that everything at or above it
 * is unused, so an object numbered from the free range fails first.
 */
int
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
	 * Rebuilding an empty range is how it keeps its flags, its object type
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
 * Split the node at `bno` in two and tell everything that has to know: the
 * lower half (keeping its oid), the upper half (a new object), the parent
 * gaining a separator, the root counting the nodes, the volume's object map
 * gaining an entry, and the spine.  Nothing between them moves: interior
 * nodes name children by oid, and the object map absorbs the difference.
 *
 * Room above is asked for first, before a block is allocated: a split that
 * found the parent full half way would have nowhere to put its new half.  A
 * full parent splits in turn, and a full root grows the tree (tree_grow).
 *
 * `at` is where to cut, zero meaning the middle; only the index self-test
 * cares, placing a chosen record first in the upper half so that deleting it
 * makes the parent's key wrong.
 */
int
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
		 * because its length is what the parent needs room for.
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
		 * The parent is full, so it makes room first -- it splits, or a
		 * full root grows the tree -- and since that can move the
		 * parent, the loop starts again from this node.
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
	o->o_oid = lo_oid;	/* virtual: the oid is a name, kept */
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
	 * One more node in the tree (the record count does not move).  The
	 * count is in the root, which is the parent only in a two-level tree.
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
	 * One more block for the volume: two written, one freed.  apfsck counts
	 * all of a volume's blocks ("Volume superblock: bad block count").
	 * Raised before the spine, which copies the superblock carrying it.
	 */
	g_apfs.ac_fs_alloc_count += 1;

	/*
	 * The object map learns where all of them are: the lower half and the
	 * nodes above by replacement, the new half by insertion, all inside
	 * one copy of its node.
	 */
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
 * Make a file longer.  A partly used last block grows with only the length
 * changing; only when the allocation is exhausted does this take a run.  The
 * new run is zeroed: a block fresh from the allocator holds whatever its
 * last owner left, and handing that out as a file's tail is a disclosure.
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
	 * A run, if the file has run out.  Taken next to the bytes it extends,
	 * so a growing file does not scatter across the container.
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
			extent_key(id, 0, key);
			stopped = false;
			if (!btree_scan(view_root(),
			    (const uint8_t *)key, (uint32_t)sizeof(key),
			    extent_locate, &last, 0, &stopped)) {
				rv = FS_APFS_E_IO;
				goto out;
			}
			/*
			 * The block just past the run, not its start: this
			 * wants to continue it, and alloc_blocks takes the
			 * hint literally when that block is free.
			 */
			if (last.el_found && last.el_phys != 0)
				near = last.el_phys +
				    last.el_len / APFS_BLOCK_SIZE;
		}
		rv = alloc_blocks((uint32_t)blocks, near, &first);
		if (rv != FS_APFS_E_OK)
			goto out;

		/*
		 * Two runs that touch are one run: if the allocator handed back
		 * the blocks right after the file's last extent, lengthen that
		 * extent rather than add a record to each of two trees.
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
	 * The inode's length is in an extended field: find the dstream by
	 * walking the field table, as the reader does.
	 */
	rv = inode_where(ino, &ino_leaf);
	if (rv != FS_APFS_E_OK)
		goto give_back;

	/*
	 * Which leaf the extent belongs in, which need not be the inode's; both
	 * are edited as one edit, and edit_leaf copies a shared leaf once.
	 */
	extent_key(id, alloced, key);
	ext_leaf = ino_leaf;
	if (blocks != 0 && merge) {
		ext_leaf = last.el_bno;		/* the record being lengthened */
	} else if (blocks != 0) {
		rv = leaf_home((const uint8_t *)key, (uint32_t)sizeof(key),
		    &ext_leaf);
		if (rv != FS_APFS_E_OK)
			goto give_back;
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
	 * Room, asked before anything is changed.  Naming the full leaf lets
	 * the caller split it and come back; after a split the record may
	 * belong in either half, so everything here is worked out again.
	 */
	if (blocks != 0 && !merge && !leaf_has_room(ne.le_node[ext_slot],
	    (uint32_t)sizeof(key), (uint32_t)sizeof(fe))) {
		*full_leaf = ext_leaf;
		rv = FS_APFS_E_NOALLOC;
		goto give_back;
	}

	if (blocks != 0 && merge) {
		/*
		 * Lengthen the record already there.  It is keyed on where the
		 * run starts in the file, which is unchanged, so this is an
		 * edit in place.
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

	/*
	 * The extent reference tree can still refuse (a run extend cannot
	 * find, a tree deeper than the walk, no block for a split).  The
	 * catalog edit above is still in memory, so the blocks this grow took
	 * are the one thing changed; kept, the next checkpoint would write them
	 * as used with nothing naming them.  So they go back.
	 */
	if (blocks != 0 && merge) {
		merge_n++;
		rv = extref_extend(last.el_phys, blocks, xid, node);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the run at %llu cannot grow in the "
			    "extent reference tree (%d) -- the %llu block(s) "
			    "taken for it go back and nothing is written\n",
			    (unsigned long long)last.el_phys, rv,
			    (unsigned long long)blocks);
			goto give_back;
		}
	} else if (blocks != 0) {
		rv = extref_insert(first, blocks, ino, xid, node);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: the extent reference tree will not take "
			    "an owner for %llu (%d) -- the %llu block(s) go "
			    "back and nothing is written\n",
			    (unsigned long long)first, rv,
			    (unsigned long long)blocks);
			goto give_back;
		}
	}

	/*
	 * The volume owns more blocks, a count apfsck recomputes from the
	 * extents it can reach.  Set before the spine copies the superblock
	 * carrying it: that copy is the only chance to write it.
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
 * And the same, with one retry behind a split.  Split first and grow after,
 * not halfway through: a split moves the leaf, the root and the object map.
 * Once, not in a loop: a single record cannot need two splits, so a second
 * refusal is not a full node.
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
 * The records a truncation touches: every run reaching past the new end.
 * Bounded, since the collecting is inside a tree walk whose callback cannot
 * allocate; a file with more runs than this is refused, not half-shortened.
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
	/* Past this stream's runs: there is nothing further of it to cut. */
	if (type != APFS_TYPE_FILE_EXTENT || oid != ec->ec_id)
		return (false);
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
 * Make a file shorter, the mirror of fs_apfs_grow.  Each obligation, and
 * what apfsck says while it is the one missing:
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
 * Shortening only the file's own record reads back correctly and passes three
 * of the checker's five questions.  The blocks go to the free queue, not the
 * bitmap: older checkpoints still name them.  Cutting inside a block costs
 * only a length (12235 bytes cut to 12000 keeps all three blocks).
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
	struct leaf_edit		 ne;
	uint8_t				*node;
	uint8_t				*leaf;
	const uint8_t			*k;
	uint64_t			 ekey[2];
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
	extent_key(id, 0, ekey);
	stopped    = false;
	if (!btree_scan(view_root(), (const uint8_t *)ekey,
	    (uint32_t)sizeof(ekey), extent_cut_pick, &ec, 0, &stopped))
		return (FS_APFS_E_IO);
	if (ec.ec_over) {
		kprintf("apfs: inode %llu has more than %u runs past %llu -- "
		    "cutting that many at once is a different rung\n",
		    (unsigned long long)ino, (unsigned)APFS_TRUNC_MAX,
		    (unsigned long long)new_size);
		return (FS_APFS_E_NOALLOC);
	}

	rv = inode_where(ino, &ino_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/*
	 * Which leaves this touches, worked out before anything is changed: a
	 * split can put a file's extents and its inode in different leaves.
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
	 * kernel can reach, asked now rather than when the giving back happens:
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
	 * by its key, not by a slot number remembered from the walk: a delete
	 * slides every entry after it down one.
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
	 * The volume owns fewer blocks.  Set before the spine copies the
	 * superblock carrying it, as in growing.
	 */
	g_apfs.ac_fs_alloc_count -= freed;

	/*
	 * And every leaf that changed, with the root whose record count a
	 * delete moved.  Last: until here nothing about the file's records has
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
 * A directory entry must sort where an implementation that hashes names
 * would put it, or only this kernel can read the volume; lookups descend on
 * the same key.  The hash, recovered from the container: CRC-32C over the
 * case-folded name's code points, each as four little-endian bytes, started
 * at all ones, no final complement, low 22 bits kept.  All twenty-six names
 * in the test volume match; without folding twenty-five do (only "Cellar"
 * tells them apart).  Folding is ASCII and anything else is refused: Apple
 * folds through NFD and the full Unicode tables, which this kernel lacks.
 */
uint32_t
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
 * Build the key a directory entry sorts under: the parent's object id with
 * the record type on top, the hash-and-length word or a bare length, then
 * the name and its NUL; `out` holds APFS_DREC_KEY_MAX bytes.  `complain`: a
 * write says why it refuses a name; a lookup falls back to the walk quietly.
 */
static int
drec_key(uint64_t parent, const char *name, uint32_t nlen, uint8_t *out,
    uint32_t *klen_out, bool complain)
{
	uint8_t		wide[4];
	uint32_t	crc;
	uint32_t	i;
	uint8_t		c;

	if (nlen > FS_APFS_NAME_MAX)
		return (FS_APFS_E_INVAL);
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
			if (complain)
				kprintf("apfs: \"%s\" is not ASCII -- folding "
				    "a name the way this volume hashes them "
				    "needs Unicode tables this kernel does "
				    "not carry\n", name);
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
 * Copy a node of the file-system tree into the checkpoint being built.  Not
 * cow_physical, which is for objects whose oid is their block: a tree node
 * is virtual, its oid a name the object map resolves, and the copy keeps it.
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
 * Find a record by object id and type in a node the caller is holding, and
 * say where it is.  By key, not by a slot remembered from a walk: an insert
 * or a delete slides everything after it.
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
 * Making and unmaking a name.  What apfsck answers when one obligation is
 * left out of an otherwise complete edit:
 *
 *	CREATE, leaving out		apfsck answers
 *	  apfs_next_obj_id		"Inode record: free inode number in use"
 *	  the directory entry		"Inode record: wrong directory child
 *					 count"
 *	  the inode record		"Inode record: wrong link count"
 *	  the dstream id record		"Data stream: missing reference count"
 *	  the parent's child count	"Inode record: wrong directory child
 *					 count"
 *	  the tree's key count		"Catalog: wrong key count in info
 *					 footer"
 *	  apfs_num_files		nothing at all
 *
 *	UNLINK, leaving out
 *	  the entry, or the parent's count	as above
 *	  the inode and dstream records	"Inode record: wrong link count"
 *	  the tree's key count		as above
 *	  the extent reference record	"Physical extent record: bad
 *					 reference count"
 *	  apfs_fs_alloc_count		"Volume superblock: bad block count"
 *	  the blocks, given back	"Space manager: bad allocation bitmap"
 *	  the deleted bytes, threaded	"B-tree: wrong free space total for
 *					 key area"
 *	  apfs_num_files		nothing at all
 *
 *	MKDIR and RMDIR differ in
 *	  the inode record (mkdir)	"Inode record: no name for primary
 *					 link"
 *	  the inode record (rmdir)	"Inode record: directory has hard
 *					 links"
 *	  apfs_num_directories		"Volume superblock: bad directory count"
 *
 * The free inode number masks every other complaint, so omissions are
 * tried one at a time.  apfs_num_files is never checked; it is kept for
 * other readers.  Unlink truncates first, which handles the blocks and the
 * records naming them.
 *
 * A directory also has no data stream ("Inode record: has dstream but isn't
 * a regular file"), its inode's mode must match the entry's type ("file
 * mode doesn't match dentry type"), and it may not go while it holds a name
 * ("Dentry record: parent inode missing").  apfs_num_directories counts
 * neither the root nor the private directory.  One function with a question
 * in it per direction keeps the few lines that differ from drifting apart.
 */

/* Longest name this kernel will make, as against the 255 it will read. */
#define	APFS_MAKE_NAME_MAX	64

/*
 * Put a name into a directory, and under it an empty file -- or, when `isdir`,
 * an empty directory.
 *
 * A file has a dstream from the moment it exists, holding no bytes: every
 * path that makes a file longer looks for a length to move, so a file made
 * without one could be opened and read but never written.  A directory must
 * not have one at all.
 *
 * A leaf with no room refuses and names the leaf (*full_leaf): a split moves
 * that leaf, its parent, the root and the object map, so nothing worked out
 * here survives one.  make_at splits it and asks again from the beginning.
 */
static int
make_once(uint64_t dir, const char *name, uint64_t now, bool isdir,
    uint16_t perm, uint64_t *ino_out, uint64_t *full_leaf)
{
	struct apfs_inode_val	*iv;
	struct apfs_xf_blob	*blob;
	struct apfs_x_field	*xf;
	struct apfs_drec_val	 dv;
	struct dirent_search	 ds;
	struct inode_info	 ii;
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
	uint32_t		 nexts;
	uint32_t		 dslen;
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

	/*
	 * The key first: the name needs one before anything else is worth
	 * doing, and the next question is asked by it.
	 */
	rv = drec_key(dir, name, nlen, dkey, &dklen, true);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/* And the name must be free. */
	ds.ds_name    = name;
	ds.ds_namelen = nlen;
	ds.ds_parent  = dir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	ds.ds_keyed   = true;
	stopped = false;
	if (!btree_scan(view_root(), dkey, dklen, dirent_match, &ds,
	    0, &stopped))
		return (FS_APFS_E_IO);
	if (ds.ds_found != 0)
		return (FS_APFS_E_EXIST);

	ino = g_apfs.ac_next_ino;
	ikey = (ino & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_INODE << APFS_J_OBJ_TYPE_SHIFT);
	skey = (ino & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_DSTREAM_ID << APFS_J_OBJ_TYPE_SHIFT);

	dv.dv_file_id    = ino;
	dv.dv_date_added = now;
	dv.dv_flags      = isdir ? APFS_DT_DIR : APFS_DT_REG;
	refs             = 1;

	/*
	 * The inode record: a fixed part, then the extended fields in ascending
	 * order of type, then their data, each padded up to a multiple of
	 * eight -- order and padding as read off the inodes on this volume.
	 *
	 * A file has two fields and a directory one.  The count, the used-data
	 * total, the record's length and whether a dstream id record follows
	 * all state that one fact, so they are written from one variable.
	 */
	mem_zero(rec, (uint32_t)sizeof(rec));
	pad   = (nlen + 1u + 7u) & ~7u;
	nexts = isdir ? 1u : 2u;
	dslen = isdir ? 0u : (uint32_t)sizeof(struct apfs_dstream);
	iv    = (struct apfs_inode_val *)rec;
	iv->ai_parent_id          = dir;
	iv->ai_private_id         = ino;
	iv->ai_create_time        = now;
	iv->ai_mod_time           = now;
	iv->ai_change_time        = now;
	iv->ai_access_time        = now;
	iv->ai_internal_flags     = APFS_INODE_NO_RSRC_FORK;
	/* A directory counts children here, and has none yet. */
	iv->ai_nchildren_or_nlink = isdir ? 0 : 1;
	/*
	 * The type is the writer's and the permission bits the caller's,
	 * already reduced by the umask and written as given.
	 */
	iv->ai_mode               = (uint16_t)((isdir ? APFS_S_IFDIR :
	    APFS_S_IFREG) | (perm & 07777u));
	blob = (struct apfs_xf_blob *)(rec + sizeof(*iv));
	blob->xb_num_exts  = (uint16_t)nexts;
	blob->xb_used_data = (uint16_t)(pad + dslen);
	xf = (struct apfs_x_field *)(rec + sizeof(*iv) + sizeof(*blob));
	xf[0].xf_type  = APFS_INO_EXT_TYPE_NAME;
	xf[0].xf_flags = APFS_XF_DO_NOT_COPY;
	xf[0].xf_size  = (uint16_t)(nlen + 1u);
	if (!isdir) {
		xf[1].xf_type  = APFS_INO_EXT_TYPE_DSTREAM;
		xf[1].xf_flags = APFS_XF_SYSTEM_FIELD;
		xf[1].xf_size  = (uint16_t)sizeof(struct apfs_dstream);
	}
	data = (uint32_t)(sizeof(*iv) + sizeof(*blob)) + nexts *
	    (uint32_t)sizeof(*xf);
	mem_copy(rec + data, (const uint8_t *)name, nlen);
	/* The NUL, the padding and the empty dstream are already zero. */
	vlen = data + pad + dslen;

	/* Where each of them belongs. */
	rv = inode_where(dir, &par_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = leaf_home(dkey, dklen, &drec_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = leaf_home((const uint8_t *)&ikey, (uint32_t)sizeof(ikey),
	    &ino_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);

	edit_init(&ne);
	(void)edit_leaf(&ne, par_leaf);
	(void)edit_leaf(&ne, drec_leaf);
	(void)edit_leaf(&ne, ino_leaf);
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * Every edit, in memory.  A file's inode and dstream id records go in
	 * together: their keys are adjacent and, the object id being new,
	 * nothing lies between them (once both exist a split can, which is
	 * why unmaking asks for the second one's leaf).  *full_leaf names each
	 * leaf as it is asked, so a refusal names the full one.
	 */
	slot = edit_leaf(&ne, drec_leaf);
	*full_leaf = drec_leaf;
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], dkey, dklen), dkey, dklen, &dv,
	    (uint32_t)sizeof(dv));
	if (rv != FS_APFS_E_OK)
		goto out;

	slot = edit_leaf(&ne, ino_leaf);
	*full_leaf = ino_leaf;
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], (const uint8_t *)&ikey,
	    (uint32_t)sizeof(ikey)), &ikey, (uint32_t)sizeof(ikey), rec, vlen);
	if (rv != FS_APFS_E_OK)
		goto out;
	if (!isdir) {
		rv = leaf_insert(ne.le_node[slot],
		    node_place(ne.le_node[slot], (const uint8_t *)&skey,
		    (uint32_t)sizeof(skey)), &skey, (uint32_t)sizeof(skey),
		    &refs, (uint32_t)sizeof(refs));
		if (rv != FS_APFS_E_OK)
			goto out;
	}
	*full_leaf = 0;

	slot = edit_leaf(&ne, par_leaf);
	rv = dir_children_add(ne.le_node[slot], dir, 1, now);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The volume's own claims, set before the spine copies the superblock
	 * that carries them, as with the block count above.
	 */
	g_apfs.ac_next_ino = ino + 1;
	if (isdir)
		g_apfs.ac_num_dirs += 1;
	else
		g_apfs.ac_num_files += 1;

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto undo;
	}
	rv = edit_commit(&ne, isdir ? 2 : 3, g_apfs.ac_xid + 1, scratch);
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

	if (isdir)
		dmake_n++;
	else
		make_n++;
	if (ino_out != NULL)
		*ino_out = ino;
	kprintf("apfs: %s\"%s\" made in inode %llu as inode %llu -- %u leaves "
	    "moved\n", isdir ? "directory " : "", name,
	    (unsigned long long)dir, (unsigned long long)ino,
	    (unsigned)ne.le_n);
	edit_free(&ne);
	return (FS_APFS_E_OK);

undo:
	g_apfs.ac_next_ino = ino;
	if (isdir)
		g_apfs.ac_num_dirs -= 1;
	else
		g_apfs.ac_num_files -= 1;
out:
	edit_free(&ne);
	return (rv);
}

/*
 * And the same, with room made underneath it.
 *
 * Two leaves can be the full one: a name's entry sorts under the parent's
 * object id and the inode under the child's, so either can lack room, and
 * splitting the first can be answered by the second.  Hence a loop, not
 * fs_apfs_grow's single retry.
 *
 * It stops after two splits.  Each leaf needs at most one (a half-empty node
 * has room for three such records by a wide margin), so a third refusal is
 * not a full node, and splitting on would fill the volume with half-empty
 * nodes.  Each split happens before the attempt it makes room for, never
 * half way through one: nothing make_once computed survives it.
 */
static int
make_at(uint64_t dir, const char *name, uint64_t now, bool isdir, uint16_t perm,
    uint64_t *ino_out)
{
	uint8_t		*scratch;
	uint64_t	 full;
	uint32_t	 tries;
	int		 rv;

	for (tries = 0; ; tries++) {
		/*
		 * Cleared before each attempt: refusals unrelated to a full
		 * node (an allocator with nowhere to write) answer
		 * FS_APFS_E_NOALLOC too, and splitting on one would split a
		 * leaf at random.
		 */
		full = 0;
		rv = make_once(dir, name, now, isdir, perm, ino_out, &full);
		if (rv != FS_APFS_E_NOALLOC || full == 0)
			return (rv);
		if (tries >= 2) {
			kprintf("apfs: the leaf at %llu still has no room for "
			    "\"%s\" after %u split(s) -- whatever is refusing, "
			    "it is not a full node\n", (unsigned long long)full,
			    name, (unsigned)tries);
			return (rv);
		}

		scratch = kmalloc(APFS_BLOCK_SIZE);
		if (scratch == NULL)
			return (FS_APFS_E_NOMEM);
		rv = node_split_at(full, 0, g_apfs.ac_xid + 1, scratch);
		kfree(scratch);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}
}

int
fs_apfs_create(uint64_t dir, const char *name, uint64_t now, uint16_t perm,
    uint64_t *ino_out)
{

	return (make_at(dir, name, now, false, perm, ino_out));
}

int
fs_apfs_mkdir(uint64_t dir, const char *name, uint64_t now, uint16_t perm,
    uint64_t *ino_out)
{

	return (make_at(dir, name, now, true, perm, ino_out));
}

struct dir_probe {
	uint64_t	dp_dir;
	bool		dp_any;
};

static bool
dirent_any(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct dir_probe	*dp;

	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	(void)bno;
	dp = arg;
	/*
	 * The scan began at this directory's first possible entry, so the very
	 * first record handed over is either one of its names or proof that it
	 * has none.  Either way there is nothing behind it worth reading.
	 */
	if (type != APFS_TYPE_DIR_REC || oid != dp->dp_dir)
		return (false);
	dp->dp_any = true;
	return (false);
}

/*
 * Does this directory hold a name?  Asked of the tree, not of the child count
 * in the directory's inode: the count is only a claim about the records, and
 * the case this guards against is the one where the claim is wrong.  One
 * descent.
 */
static int
dir_empty(uint64_t dir, bool *empty_out)
{
	struct dir_probe	dp;
	uint8_t			dkey[APFS_DREC_KEY_MAX];
	uint32_t		dklen;
	bool			stopped;

	dp.dp_dir = dir;
	dp.dp_any = false;
	drec_low_key(dir, dkey, &dklen);
	stopped = false;
	if (!btree_scan(view_root(), dkey, dklen, dirent_any, &dp,
	    0, &stopped))
		return (FS_APFS_E_IO);
	*empty_out = !dp.dp_any;
	return (FS_APFS_E_OK);
}

/*
 * And take a name back out, with what was under it.
 *
 * A file is first cut to nothing by the truncate, which gives back the runs,
 * the records in both trees that name them and the volume's block count;
 * what is left is the three records the create made and the two counters it
 * moved.  A directory has no blocks and no dstream; instead it must be
 * empty, asked before anything is touched.
 */
static int
unmake_at(uint64_t dir, const char *name, uint64_t now, bool isdir)
{
	struct dirent_search	 ds;
	struct inode_info	 ii;
	struct leaf_edit	 ne;
	uint8_t			 dkey[12 + APFS_MAKE_NAME_MAX + 1];
	uint8_t			*scratch;
	uint64_t		 child;
	uint64_t		 skey;
	uint64_t		 par_leaf;
	uint64_t		 drec_leaf;
	uint64_t		 ino_leaf;
	uint64_t		 ds_leaf;
	uint32_t		 dklen;
	uint32_t		 nlen;
	uint32_t		 pos, voff, vlen;
	uint32_t		 slot;
	uint32_t		 gone;
	int			 rv;
	bool			 stopped;
	bool			 empty;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	nlen = (uint32_t)str_len(name);
	if (nlen == 0 || nlen > APFS_MAKE_NAME_MAX)
		return (FS_APFS_E_INVAL);
	rv = drec_key(dir, name, nlen, dkey, &dklen, true);
	if (rv != FS_APFS_E_OK)
		return (rv);

	ds.ds_name    = name;
	ds.ds_namelen = nlen;
	ds.ds_parent  = dir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	ds.ds_keyed   = true;
	stopped = false;
	if (!btree_scan(view_root(), dkey, dklen, dirent_match, &ds,
	    0, &stopped))
		return (FS_APFS_E_IO);
	if (ds.ds_found == 0)
		return (FS_APFS_E_NOTFOUND);
	if (ds.ds_is_dir != isdir) {
		kprintf("apfs: \"%s\" is %sa directory, and %s\n", name,
		    ds.ds_is_dir ? "" : "not ",
		    isdir ? "rmdir takes out nothing else" :
		    "unlink does not take those out");
		return (isdir ? FS_APFS_E_NOTDIR : FS_APFS_E_ISDIR);
	}
	child = ds.ds_found;
	if (inode_info(child, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	/*
	 * An orphan is exempt, explicitly: it has no names at all, which
	 * ii_nlink rounds up to one, so the guard would pass it by accident.
	 * It comes through here only as a file being let go from the private
	 * directory.
	 */
	if (!isdir && !ii.ii_orphan && ii.ii_nlink != 1) {
		kprintf("apfs: inode %llu has %u links and this kernel makes "
		    "none -- unlinking one of several is a different rung\n",
		    (unsigned long long)child, (unsigned)ii.ii_nlink);
		return (FS_APFS_E_NOALLOC);
	}
	if (isdir) {
		rv = dir_empty(child, &empty);
		if (rv != FS_APFS_E_OK)
			return (rv);
		if (!empty) {
			kprintf("apfs: \"%s\" still holds a name -- a directory "
			    "taken out from over its children leaves entries "
			    "whose parent is gone\n", name);
			return (FS_APFS_E_NOTEMPTY);
		}
	}

	/*
	 * The bytes first, through the truncate.  It moves leaves, so
	 * everything below is located afterwards.
	 */
	if (!isdir && (ii.ii_size != 0 || ii.ii_alloced != 0)) {
		rv = fs_apfs_truncate(child, ii.ii_private_id, 0);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}

	rv = inode_where(dir, &par_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = inode_where(child, &ino_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = leaf_home(dkey, dklen, &drec_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	/*
	 * The data stream record is asked for separately, though it sorts
	 * right after the inode's and was written into the same node: both
	 * exist by now, so a split can have landed between them.  Left behind
	 * (the branch below takes "none here" for a file off the image), it is
	 * what apfsck calls "Data stream: has no references".
	 */
	ds_leaf = ino_leaf;
	if (!isdir) {
		skey = (child & APFS_J_OBJ_ID_MASK) |
		    ((uint64_t)APFS_TYPE_DSTREAM_ID << APFS_J_OBJ_TYPE_SHIFT);
		rv = leaf_home((const uint8_t *)&skey, (uint32_t)sizeof(skey),
		    &ds_leaf);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}

	edit_init(&ne);
	(void)edit_leaf(&ne, par_leaf);
	(void)edit_leaf(&ne, drec_leaf);
	(void)edit_leaf(&ne, ino_leaf);
	(void)edit_leaf(&ne, ds_leaf);
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The entry, found by its exact key rather than by object and type:
	 * a directory has one record per name and they differ only past the
	 * eighth byte, so "the DIR_REC of this parent" names all of them.
	 */
	gone = 0;
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
		gone++;
	}

	/*
	 * How many records actually left, counted, because the tree's key
	 * count is told this number at the end: a file this kernel made has a
	 * dstream id record and a directory has none.
	 */
	slot = edit_leaf(&ne, ds_leaf);
	if (node_slot(ne.le_node[slot], child, APFS_TYPE_DSTREAM_ID, &pos,
	    &voff, &vlen)) {
		rv = leaf_delete(ne.le_node[slot], pos);
		if (rv != FS_APFS_E_OK)
			goto out;
		gone++;
	} else if (!isdir) {
		/*
		 * A file this kernel made always has one.  One that came off
		 * the image need not, and removing a record that is not there
		 * would take the record after it.
		 */
		kprintf("apfs: inode %llu has no dstream id record -- one "
		    "fewer to take out\n", (unsigned long long)child);
	}
	/* Its own leaf again, which is the stream's only when they share. */
	slot = edit_leaf(&ne, ino_leaf);
	if (!node_slot(ne.le_node[slot], child, APFS_TYPE_INODE, &pos, &voff,
	    &vlen)) {
		rv = FS_APFS_E_NOTFOUND;
		goto out;
	}
	rv = leaf_delete(ne.le_node[slot], pos);
	if (rv != FS_APFS_E_OK)
		goto out;
	gone++;

	slot = edit_leaf(&ne, par_leaf);
	rv = dir_children_add(ne.le_node[slot], dir, -1, now);
	if (rv != FS_APFS_E_OK)
		goto out;

	if (isdir)
		g_apfs.ac_num_dirs -= 1;
	else
		g_apfs.ac_num_files -= 1;

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto undo;
	}
	rv = edit_commit(&ne, -(int64_t)gone, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		if (!edit_moved(&ne)) {
			kprintf("apfs: unmaking \"%s\" was refused before "
			    "anything moved (%d) -- the volume is as it was\n",
			    name, rv);
			goto undo;
		}
		kprintf("apfs: unmaking \"%s\" failed after a leaf had moved "
		    "(%d) -- this checkpoint must not be written\n", name, rv);
		goto out;
	}

	if (isdir)
		dkill_n++;
	else
		kill_n++;
	kprintf("apfs: %s\"%s\" taken out of inode %llu -- inode %llu is gone "
	    "with %u record(s), %u leaves moved\n", isdir ? "directory " : "",
	    name, (unsigned long long)dir, (unsigned long long)child,
	    (unsigned)gone, (unsigned)ne.le_n);
	edit_free(&ne);
	return (FS_APFS_E_OK);

undo:
	if (isdir)
		g_apfs.ac_num_dirs += 1;
	else
		g_apfs.ac_num_files += 1;
out:
	edit_free(&ne);
	return (rv);
}

int
fs_apfs_unlink(uint64_t dir, const char *name, uint64_t now)
{

	return (unmake_at(dir, name, now, false));
}

int
fs_apfs_rmdir(uint64_t dir, const char *name, uint64_t now)
{

	return (unmake_at(dir, name, now, true));
}

/*
 * Moving a name.
 *
 * A rename is the two writers above run together.  What is easy to leave
 * out is the inode record, which carries a name and a parent of its own that
 * must agree with the entry naming it.  Each left out in turn, on an
 * otherwise complete move:
 *
 *	RENAME, leaving out		apfsck answers
 *	  the name in the record	"Inode record: wrong name for only link"
 *	  ai_parent_id			"Inode record: bad parent for only link"
 *	  either parent's child count	"Inode record: wrong directory child
 *					count"
 *
 * "for only link": a volume with hard links keeps the other names in
 * SIBLING_LINK records; nothing here makes a second link, and unmake_at
 * refuses an inode that has one.
 *
 * The record changes length.  A name lives in an extended field, each datum
 * padded to eight bytes, so renaming "a" to "bbbbbbbbb" makes the record
 * eight bytes longer.  So the inode record is not amended in place, as a
 * chmod is: a new one is built beside it, the old one deleted, the new one
 * inserted.  Everything but the name and the parent is carried across; the
 * dstream field holds the file's length and blocks, and a record rebuilt the
 * way a create writes one would empty every file it moved.
 */

/*
 * The old record with a different name in it, a different parent, and a change
 * time.  Returns the new length, which is the old one plus or minus a multiple
 * of eight.
 */
static int
inode_renamed(const uint8_t *old, uint32_t olen, uint64_t ndir,
    const char *name, uint32_t nlen, uint64_t now, uint8_t *out, uint32_t cap,
    uint32_t *len_out)
{
	const struct apfs_inode_val	*oiv;
	const struct apfs_xf_blob	*oblob;
	const struct apfs_x_field	*oxf;
	struct apfs_inode_val		*niv;
	struct apfs_xf_blob		*nblob;
	struct apfs_x_field		*nxf;
	uint32_t			 nexts;
	uint32_t			 ent;
	uint32_t			 odata;
	uint32_t			 ndata;
	uint32_t			 used;
	uint32_t			 size;
	uint32_t			 pad;
	uint32_t			 i;
	bool				 named;

	if (olen < sizeof(*oiv) + sizeof(*oblob))
		return (FS_APFS_E_INVAL);
	oiv   = (const struct apfs_inode_val *)old;
	oblob = (const struct apfs_xf_blob *)(old + sizeof(*oiv));
	nexts = oblob->xb_num_exts;
	ent   = (uint32_t)(sizeof(*oiv) + sizeof(*oblob));
	odata = ent + nexts * (uint32_t)sizeof(*oxf);
	if (odata > olen || odata > cap)
		return (FS_APFS_E_INVAL);

	/*
	 * The fixed part, the blob and the whole field table come across as
	 * they are; only one field's size and one field's data change, and the
	 * table is walked below to fix the one.
	 */
	mem_copy(out, old, odata);
	niv = (struct apfs_inode_val *)out;
	niv->ai_parent_id   = ndir;
	niv->ai_change_time = now;
	nblob = (struct apfs_xf_blob *)(out + sizeof(*niv));
	nxf   = (struct apfs_x_field *)(out + ent);

	ndata = odata;
	used  = 0;
	named = false;
	for (i = 0; i < nexts; i++) {
		oxf  = (const struct apfs_x_field *)(old + ent +
		    i * sizeof(*oxf));
		size = oxf->xf_size;
		if (size > olen - odata)
			return (FS_APFS_E_INVAL);
		if (oxf->xf_type == APFS_INO_EXT_TYPE_NAME) {
			named = true;
			nxf[i].xf_size = (uint16_t)(nlen + 1u);
			pad = (nlen + 1u + 7u) & ~7u;
			if (pad > cap - ndata)
				return (FS_APFS_E_NOALLOC);
			mem_zero(out + ndata, pad);
			mem_copy(out + ndata, (const uint8_t *)name, nlen);
		} else {
			pad = (size + 7u) & ~7u;
			if (pad > cap - ndata)
				return (FS_APFS_E_NOALLOC);
			mem_zero(out + ndata, pad);
			mem_copy(out + ndata, old + odata, size);
		}
		/*
		 * Only a malformed record pads a datum past its end, but
		 * `olen - odata` is unsigned, so one would wrap the next
		 * round's bounds check and read past the node.  It comes off
		 * the disk: bounded, not trusted.
		 */
		odata += (size + 7u) & ~7u;
		if (odata > olen)
			return (FS_APFS_E_INVAL);
		ndata += pad;
		used  += pad;
	}
	/*
	 * An inode with no name field cannot be moved: the checker requires
	 * the record to name the link, and there is nowhere to write the new
	 * name without inventing a field.
	 */
	if (!named) {
		kprintf("apfs: the inode record has no name field -- there is "
		    "nothing in it to rename\n");
		return (FS_APFS_E_INVAL);
	}
	nblob->xb_used_data = (uint16_t)used;
	*len_out = ndata;
	return (FS_APFS_E_OK);
}

/*
 * Is `dir` the directory `top`, or somewhere underneath it?
 *
 * Asked before a directory moves: moving one into itself detaches the
 * subtree, whose records stay in the tree with nothing naming them, and
 * walking up from inside it goes round for ever.  The walk goes up the
 * parent chain to the root, whose parent is an id that is not an object.
 */
static int
dir_under(uint64_t dir, uint64_t top, bool *under)
{
	struct inode_info	ii;
	uint64_t		up;
	uint32_t		hops;
	int			rv;

	*under = false;
	up = dir;
	for (hops = 0; up != APFS_ROOT_DIR_INO; hops++) {
		if (up == top) {
			*under = true;
			return (FS_APFS_E_OK);
		}
		/*
		 * A chain longer than any tree here could be has a loop in it,
		 * the very thing this keeps out: refuse rather than walk it.
		 */
		if (hops > 64) {
			kprintf("apfs: the parents of inode %llu do not reach "
			    "the root in %u hops -- something above it is "
			    "already a loop\n", (unsigned long long)dir,
			    (unsigned)hops);
			return (FS_APFS_E_INVAL);
		}
		rv = inode_info(up, &ii);
		if (rv != FS_APFS_E_OK)
			return (rv);
		up = ii.ii_parent;
	}
	return (FS_APFS_E_OK);
}

/*
 * The name a file waits under once it has none.
 *
 * Derived from the object id and never stored, which is what makes the private
 * directory usable without a table on the side: a caller holding the id can
 * always say what the entry is called, and two orphans cannot collide because
 * two live files cannot share an id.
 */
static void
orphan_name(uint64_t ino, char *out)
{
	static const char	hex[] = "0123456789abcdef";
	uint64_t		v;
	uint32_t		n;
	uint32_t		i;

	out[0] = '0';
	out[1] = 'x';
	n = 2;
	/* No leading zeroes: apfsck wants the "%llx" of it, not a width. */
	for (i = 16; i > 0; i--) {
		v = (ino >> ((i - 1) * 4)) & 0xfu;
		if (v != 0 || n > 2 || i == 1)
			out[n++] = hex[v];
	}
	out[n++] = '-';
	out[n++] = 'd';
	out[n++] = 'e';
	out[n++] = 'a';
	out[n++] = 'd';
	out[n]   = '\0';
}

/*
 * One attempt at moving a name, naming the leaf that had no room, as
 * make_once does: a split moves the leaf, its parent, the root and the
 * object map, so nothing worked out here survives one.
 *
 * Orphaning is the same move.  A file whose last name goes while something
 * still holds it open moves into the private directory, and there it must
 * say what the format wants of an orphan: a parent that is not the private
 * directory (the root, which cannot be removed out from under it), and a
 * link count of zero.  The rest is the same problem with the same answer.
 *
 * A taken name is taken over in this same edit: POSIX asks a rename to be
 * atomic, the checkpoint is this volume's only atom, and "unlink then move"
 * could publish a state without the name at all.  Only the occupant's name
 * has to go, so a file standing where the move lands is orphaned here: its
 * record rebuilt under the derived name, its entry rewritten to the
 * newcomer, no extent touched.  The caller learns the inode through
 * *victim_out and reaps it or leaves it to the last close.  A directory
 * standing there must be empty, and an empty directory is two records and
 * two counters, taken out here.
 */
static int
move_once(uint64_t odir, const char *oname, uint64_t ndir, const char *nname,
    uint64_t now, bool orphan, uint64_t *full_leaf, uint64_t *victim_out)
{
	struct apfs_drec_val	 dv;
	struct apfs_drec_val	 vdv;
	struct btree_layout	 bl;
	struct dirent_search	 ds;
	struct inode_info	 ii;
	struct leaf_edit	 ne;
	uint8_t			 okey[12 + APFS_MAKE_NAME_MAX + 1];
	uint8_t			 nkey[12 + APFS_MAKE_NAME_MAX + 1];
	uint8_t			 vkey[12 + APFS_MAKE_NAME_MAX + 1];
	char			 dead[APFS_ORPHAN_NAME_MAX];
	uint8_t			*rec;
	uint8_t			*scratch;
	const uint8_t		*old;
	uint64_t		 child;
	uint64_t		 ikey;
	uint64_t		 opar_leaf;
	uint64_t		 npar_leaf;
	uint64_t		 odrec_leaf;
	uint64_t		 ndrec_leaf;
	uint64_t		 ino_leaf;
	uint64_t		 victim;
	uint64_t		 vic_leaf;
	uint64_t		 priv_leaf;
	uint64_t		 dead_leaf;
	uint64_t		 vsize;
	int64_t			 keydelta;
	uint32_t		 oklen, nklen, vklen;
	uint32_t		 onlen, nnlen;
	uint32_t		 koff, klen, voff, vlen;
	uint32_t		 rlen;
	uint32_t		 pos;
	uint32_t		 slot;
	uint32_t		 i;
	int			 rv;
	bool			 stopped;
	bool			 isdir;
	bool			 victim_isdir;
	bool			 under;
	bool			 empty;

	if (victim_out != NULL)
		*victim_out = 0;
	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	if (!g_apfs.ac_ip_valid || g_apfs.ac_ctr_omap_tree == 0)
		return (FS_APFS_E_NOALLOC);

	onlen = (uint32_t)str_len(oname);
	nnlen = (uint32_t)str_len(nname);
	if (onlen == 0 || onlen > APFS_MAKE_NAME_MAX ||
	    nnlen == 0 || nnlen > APFS_MAKE_NAME_MAX)
		return (FS_APFS_E_INVAL);
	rv = drec_key(odir, oname, onlen, okey, &oklen, true);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = drec_key(ndir, nname, nnlen, nkey, &nklen, true);
	if (rv != FS_APFS_E_OK)
		return (rv);

	/*
	 * The same name in the same directory must succeed and change nothing
	 * (POSIX).  Compared on the keys, which the tree is ordered on: names
	 * differing only in case are two keys, so renaming "a" to "A" does the
	 * whole move and keeps the case asked for.
	 */
	if (oklen == nklen) {
		for (i = 0; i < oklen && okey[i] == nkey[i]; i++)
			;
		if (i == oklen)
			return (FS_APFS_E_OK);
	}

	ds.ds_name    = oname;
	ds.ds_namelen = onlen;
	ds.ds_parent  = odir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	ds.ds_keyed   = true;
	stopped = false;
	if (!btree_scan(view_root(), okey, oklen, dirent_match, &ds,
	    0, &stopped))
		return (FS_APFS_E_IO);
	if (ds.ds_found == 0)
		return (FS_APFS_E_NOTFOUND);
	child = ds.ds_found;
	isdir = ds.ds_is_dir;

	/*
	 * Whatever already stands under the destination is the occupant, and
	 * the move happens over it (POSIX).  Its kind decides which of two
	 * endings it gets, below, after the checks every move needs.
	 */
	ds.ds_name    = nname;
	ds.ds_namelen = nnlen;
	ds.ds_parent  = ndir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	ds.ds_keyed   = true;
	stopped = false;
	if (!btree_scan(view_root(), nkey, nklen, dirent_match, &ds,
	    0, &stopped))
		return (FS_APFS_E_IO);
	victim       = ds.ds_found;
	victim_isdir = ds.ds_is_dir;
	/*
	 * One file cannot stand at both names: a link count of one is enforced
	 * on everything this kernel makes, so the same object id under two
	 * entries is not a state a healthy volume can be in.  Refused rather
	 * than asserted, because the volume is an input here, not an invariant.
	 */
	if (victim != 0 && victim == child) {
		kprintf("apfs: \"%s\" and \"%s\" both name inode %llu -- two "
		    "names on one link is not a state this kernel makes\n",
		    oname, nname, (unsigned long long)child);
		return (FS_APFS_E_INVAL);
	}

	/*
	 * The destination has to be a directory, asked of its record: finding
	 * the old name proved it of the source, but "the name is free" is as
	 * true of a regular file, where dir_children_add would add a child to
	 * a field that counts links.
	 */
	if (inode_info(ndir, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	if ((ii.ii_mode & APFS_S_IFMT) != APFS_S_IFDIR) {
		kprintf("apfs: inode %llu is not a directory -- nothing can be "
		    "moved into it\n", (unsigned long long)ndir);
		return (FS_APFS_E_NOTDIR);
	}

	if (inode_info(child, &ii) != FS_APFS_E_OK)
		return (FS_APFS_E_NOTFOUND);
	if (!isdir && ii.ii_nlink != 1) {
		kprintf("apfs: inode %llu has %u links -- moving one name of "
		    "several is a different rung\n",
		    (unsigned long long)child, (unsigned)ii.ii_nlink);
		return (FS_APFS_E_NOALLOC);
	}
	/*
	 * A directory cannot wait in there: an orphaned directory would still
	 * hold entries whose parent no path reaches, every name under it
	 * unreachable yet valid -- damage a checker cannot tell from a healthy
	 * volume.
	 */
	if (orphan && isdir) {
		kprintf("apfs: \"%s\" is a directory -- one held open while "
		    "its name goes away is a rung of its own\n", oname);
		return (FS_APFS_E_ISDIR);
	}
	if (child == APFS_ROOT_DIR_INO)
		return (FS_APFS_E_INVAL);
	/*
	 * The destination must not be inside what is moving.  Only asked of a
	 * directory that is changing parents: a file contains nothing, and a
	 * directory renamed where it stands cannot be its own new ancestor.
	 */
	if (isdir && ndir != odir) {
		rv = dir_under(ndir, child, &under);
		if (rv != FS_APFS_E_OK)
			return (rv);
		if (under) {
			kprintf("apfs: inode %llu is inside \"%s\" -- a "
			    "directory moved under itself takes every name in "
			    "it out of the volume\n",
			    (unsigned long long)ndir, oname);
			return (FS_APFS_E_INVAL);
		}
	}

	/*
	 * What the occupant must be, before anything is touched.  The kinds
	 * must agree (POSIX gives each mismatch its own answer).  A directory
	 * being replaced must be empty, asked of the tree as rmdir asks, for
	 * the reason above.  A file being replaced goes to the private
	 * directory, where several names cannot follow one inode, so more
	 * than one link is refused as it is for the source.
	 */
	vsize = 0;
	vklen = 0;
	if (victim != 0) {
		if (victim_isdir && !isdir) {
			kprintf("apfs: \"%s\" is a directory and \"%s\" is "
			    "not -- a file cannot take a directory's name\n",
			    nname, oname);
			return (FS_APFS_E_ISDIR);
		}
		if (!victim_isdir && isdir) {
			kprintf("apfs: \"%s\" is not a directory and \"%s\" "
			    "is -- a directory cannot take a file's name\n",
			    nname, oname);
			return (FS_APFS_E_NOTDIR);
		}
		if (victim_isdir) {
			rv = dir_empty(victim, &empty);
			if (rv != FS_APFS_E_OK)
				return (rv);
			if (!empty) {
				kprintf("apfs: \"%s\" still holds a name -- "
				    "a directory replaced over its children "
				    "leaves entries whose parent is gone\n",
				    nname);
				return (FS_APFS_E_NOTEMPTY);
			}
		} else {
			if (inode_info(victim, &ii) != FS_APFS_E_OK)
				return (FS_APFS_E_NOTFOUND);
			if (ii.ii_nlink != 1) {
				kprintf("apfs: inode %llu has %u links -- "
				    "taking one name of several is a "
				    "different rung\n",
				    (unsigned long long)victim,
				    (unsigned)ii.ii_nlink);
				return (FS_APFS_E_NOALLOC);
			}
			vsize = ii.ii_size;
		}
		orphan_name(victim, dead);
		rv = drec_key(APFS_PRIV_DIR_INO, dead,
		    (uint32_t)str_len(dead), vkey, &vklen, true);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}

	/*
	 * Eight leaves at most -- two parents, two entries and the inode, and
	 * with an occupant, its inode, the private directory's and its new
	 * entry's -- against the nine an edit holds, so the slots below are
	 * not checked for overflow.
	 */
	rv = inode_where(odir, &opar_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = inode_where(ndir, &npar_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = inode_where(child, &ino_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = leaf_home(okey, oklen, &odrec_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	rv = leaf_home(nkey, nklen, &ndrec_leaf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	vic_leaf  = 0;
	priv_leaf = 0;
	dead_leaf = 0;
	if (victim != 0) {
		rv = inode_where(victim, &vic_leaf);
		if (rv != FS_APFS_E_OK)
			return (rv);
		if (!victim_isdir) {
			rv = inode_where(APFS_PRIV_DIR_INO, &priv_leaf);
			if (rv != FS_APFS_E_OK)
				return (rv);
			rv = leaf_home(vkey, vklen, &dead_leaf);
			if (rv != FS_APFS_E_OK)
				return (rv);
		}
	}

	rec = kmalloc(APFS_BLOCK_SIZE);
	if (rec == NULL)
		return (FS_APFS_E_NOMEM);
	edit_init(&ne);
	(void)edit_leaf(&ne, opar_leaf);
	(void)edit_leaf(&ne, npar_leaf);
	(void)edit_leaf(&ne, odrec_leaf);
	(void)edit_leaf(&ne, ndrec_leaf);
	(void)edit_leaf(&ne, ino_leaf);
	if (victim != 0) {
		(void)edit_leaf(&ne, vic_leaf);
		if (!victim_isdir) {
			(void)edit_leaf(&ne, priv_leaf);
			(void)edit_leaf(&ne, dead_leaf);
		}
	}
	rv = edit_read(&ne);
	if (rv != FS_APFS_E_OK)
		goto out;

	/*
	 * The occupant steps down first, while everything about it is still a
	 * record.  A file is orphaned as above -- rebuilt under the derived
	 * name with the root for a parent and no links, its record out and back
	 * in at its own key, its entry moved to the private directory with its
	 * value whole -- and no extent is touched: the bytes are the reap's,
	 * later.  An empty directory just loses its two records.
	 */
	if (victim != 0) {
		slot = edit_leaf(&ne, vic_leaf);
		if (!node_slot(ne.le_node[slot], victim, APFS_TYPE_INODE, &pos,
		    &voff, &vlen)) {
			rv = FS_APFS_E_NOTFOUND;
			goto out;
		}
		if (!victim_isdir) {
			btree_layout(ne.le_node[slot], &bl);
			old = (const uint8_t *)bl.bl_vals - voff;
			rv  = inode_renamed(old, vlen, APFS_ROOT_DIR_INO, dead,
			    (uint32_t)str_len(dead), now, rec, APFS_BLOCK_SIZE,
			    &rlen);
			if (rv != FS_APFS_E_OK)
				goto out;
			((struct apfs_inode_val *)rec)->ai_nchildren_or_nlink =
			    0;
		}
		rv = leaf_delete(ne.le_node[slot], pos);
		if (rv != FS_APFS_E_OK)
			goto out;
		if (!victim_isdir) {
			ikey = (victim & APFS_J_OBJ_ID_MASK) |
			    ((uint64_t)APFS_TYPE_INODE <<
			    APFS_J_OBJ_TYPE_SHIFT);
			*full_leaf = vic_leaf;
			rv = leaf_insert(ne.le_node[slot],
			    node_place(ne.le_node[slot],
			    (const uint8_t *)&ikey, (uint32_t)sizeof(ikey)),
			    &ikey, (uint32_t)sizeof(ikey), rec, rlen);
			if (rv != FS_APFS_E_OK)
				goto out;
			*full_leaf = 0;
		}

		/*
		 * Its entry out, by the exact key the newcomer's goes in by:
		 * seen from the directory, the same key with a different file
		 * behind it.
		 */
		slot = edit_leaf(&ne, ndrec_leaf);
		btree_layout(ne.le_node[slot], &bl);
		rv = FS_APFS_E_NOTFOUND;
		for (i = 0; i < bl.bl_nkeys; i++) {
			btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
			if (klen != nklen)
				continue;
			if (jkey_cmp(bl.bl_keys + koff, klen, nkey, nklen) !=
			    0)
				continue;
			if (vlen < sizeof(vdv)) {
				rv = FS_APFS_E_INVAL;
				break;
			}
			vdv = *(const struct apfs_drec_val *)
			    ((const uint8_t *)bl.bl_vals - voff);
			rv = leaf_delete(ne.le_node[slot], i);
			break;
		}
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: \"%s\" was in inode %llu a moment ago "
			    "and is not in leaf %llu now\n", nname,
			    (unsigned long long)ndir,
			    (unsigned long long)ndrec_leaf);
			goto out;
		}
		if (!victim_isdir) {
			slot = edit_leaf(&ne, dead_leaf);
			*full_leaf = dead_leaf;
			rv = leaf_insert(ne.le_node[slot],
			    node_place(ne.le_node[slot], vkey, vklen), vkey,
			    vklen, &vdv, (uint32_t)sizeof(vdv));
			if (rv != FS_APFS_E_OK)
				goto out;
			*full_leaf = 0;
		}
	}

	/*
	 * The new record is built first, while the old one is still a record:
	 * a delete threads the value's bytes onto the node's free list by
	 * writing a link into the first of them.
	 */
	slot = edit_leaf(&ne, ino_leaf);
	if (!node_slot(ne.le_node[slot], child, APFS_TYPE_INODE, &pos, &voff,
	    &vlen)) {
		rv = FS_APFS_E_NOTFOUND;
		goto out;
	}
	btree_layout(ne.le_node[slot], &bl);
	old = (const uint8_t *)bl.bl_vals - voff;
	rv  = inode_renamed(old, vlen, orphan ? APFS_ROOT_DIR_INO : ndir, nname,
	    nnlen, now, rec, APFS_BLOCK_SIZE, &rlen);
	if (rv != FS_APFS_E_OK)
		goto out;
	/*
	 * The parent went in above as an argument; the link count goes on
	 * after, since inode_renamed carries the fixed part across verbatim.
	 * Zero states that no name reaches this file any more.
	 */
	if (orphan)
		((struct apfs_inode_val *)rec)->ai_nchildren_or_nlink = 0;

	/*
	 * The old entry out, by its exact key as in unmake_at: a directory has
	 * one record per name and they differ only past the eighth byte, so
	 * "the DIR_REC of this parent" names all of them at once.
	 */
	slot = edit_leaf(&ne, odrec_leaf);
	btree_layout(ne.le_node[slot], &bl);
	rv = FS_APFS_E_NOTFOUND;
	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		if (klen != oklen)
			continue;
		if (jkey_cmp(bl.bl_keys + koff, klen, okey, oklen) != 0)
			continue;
		/*
		 * Carried across whole rather than rebuilt, so that whatever an
		 * entry says beyond the object id and the type keeps saying it.
		 * A shorter one than this kernel knows how to read is refused
		 * before the delete, since there would be nothing to put back.
		 */
		if (vlen < sizeof(dv)) {
			rv = FS_APFS_E_INVAL;
			break;
		}
		dv = *(const struct apfs_drec_val *)
		    ((const uint8_t *)bl.bl_vals - voff);
		rv = leaf_delete(ne.le_node[slot], i);
		break;
	}
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs: \"%s\" was in inode %llu a moment ago and is "
		    "not in leaf %llu now\n", oname, (unsigned long long)odir,
		    (unsigned long long)odrec_leaf);
		goto out;
	}

	/*
	 * And the new entry in, carrying the old one's value: the object id it
	 * names, its type, and the date it was added, which is a fact about the
	 * name and not about this move.  The delete came first so that a rename
	 * within one directory has the old entry's room to put the new one in.
	 */
	slot = edit_leaf(&ne, ndrec_leaf);
	*full_leaf = ndrec_leaf;
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], nkey, nklen), nkey, nklen, &dv,
	    (uint32_t)sizeof(dv));
	if (rv != FS_APFS_E_OK)
		goto out;

	/* Then the record itself, out and back in at its own unchanged key. */
	slot = edit_leaf(&ne, ino_leaf);
	if (!node_slot(ne.le_node[slot], child, APFS_TYPE_INODE, &pos, &voff,
	    &vlen)) {
		rv = FS_APFS_E_NOTFOUND;
		goto out;
	}
	rv = leaf_delete(ne.le_node[slot], pos);
	if (rv != FS_APFS_E_OK)
		goto out;
	ikey = (child & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_INODE << APFS_J_OBJ_TYPE_SHIFT);
	*full_leaf = ino_leaf;
	rv = leaf_insert(ne.le_node[slot],
	    node_place(ne.le_node[slot], (const uint8_t *)&ikey,
	    (uint32_t)sizeof(ikey)), &ikey, (uint32_t)sizeof(ikey), rec, rlen);
	if (rv != FS_APFS_E_OK)
		goto out;
	*full_leaf = 0;

	/*
	 * The counts.  A name that stays in its directory changes neither, but
	 * the times still move, through one call with nothing to add.  An
	 * occupant's directory loses it, cancelling the newcomer's arrival
	 * there, and the private directory gains a file it takes in.
	 */
	slot = edit_leaf(&ne, opar_leaf);
	if (odir == ndir)
		rv = dir_children_add(ne.le_node[slot], odir,
		    victim != 0 ? -1 : 0, now);
	else {
		rv = dir_children_add(ne.le_node[slot], odir, -1, now);
		if (rv == FS_APFS_E_OK) {
			slot = edit_leaf(&ne, npar_leaf);
			rv = dir_children_add(ne.le_node[slot], ndir,
			    victim != 0 ? 0 : 1, now);
		}
	}
	if (rv == FS_APFS_E_OK && victim != 0 && !victim_isdir) {
		slot = edit_leaf(&ne, priv_leaf);
		rv = dir_children_add(ne.le_node[slot], APFS_PRIV_DIR_INO, 1,
		    now);
	}
	if (rv != FS_APFS_E_OK)
		goto out;

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		rv = FS_APFS_E_NOMEM;
		goto out;
	}
	/*
	 * Over a free name or a file no total moves: records went out and came
	 * back, and the file still exists.  Over a directory the tree is two
	 * records short and the volume one directory fewer, put back if the
	 * commit refuses before anything moved.
	 */
	keydelta = (victim != 0 && victim_isdir) ? -2 : 0;
	if (victim != 0 && victim_isdir)
		g_apfs.ac_num_dirs -= 1;
	rv = edit_commit(&ne, keydelta, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		if (victim != 0 && victim_isdir && !edit_moved(&ne))
			g_apfs.ac_num_dirs += 1;
		kprintf("apfs: moving \"%s\" failed (%d) -- %s\n", oname, rv,
		    edit_moved(&ne) ? "a leaf had moved, and this checkpoint "
		    "must not be written" : "before anything moved, so the "
		    "volume is as it was");
		goto out;
	}

	if (orphan) {
		orph_n++;
		kprintf("apfs: \"%s\" is gone from inode %llu -- inode %llu "
		    "waits in the private directory as \"%s\", %llu byte(s) "
		    "still there, %u leaves moved\n", oname,
		    (unsigned long long)odir, (unsigned long long)child, nname,
		    (unsigned long long)ii.ii_size, (unsigned)ne.le_n);
	} else if (victim != 0 && !victim_isdir) {
		move_n++;
		clob_n++;
		orph_n++;
		kprintf("apfs: \"%s\" in inode %llu is now \"%s\" in inode "
		    "%llu -- inode %llu took the name, inode %llu waits in "
		    "the private directory with %llu byte(s), %u leaves "
		    "moved\n", oname, (unsigned long long)odir, nname,
		    (unsigned long long)ndir, (unsigned long long)child,
		    (unsigned long long)victim, (unsigned long long)vsize,
		    (unsigned)ne.le_n);
	} else if (victim != 0) {
		move_n++;
		clob_n++;
		kprintf("apfs: \"%s\" in inode %llu is now \"%s\" in inode "
		    "%llu -- inode %llu took the name, directory %llu is "
		    "gone, %u leaves moved\n", oname,
		    (unsigned long long)odir, nname, (unsigned long long)ndir,
		    (unsigned long long)child, (unsigned long long)victim,
		    (unsigned)ne.le_n);
	} else {
		move_n++;
		kprintf("apfs: \"%s\" in inode %llu is now \"%s\" in inode "
		    "%llu -- inode %llu, %u leaves moved\n", oname,
		    (unsigned long long)odir, nname, (unsigned long long)ndir,
		    (unsigned long long)child, (unsigned)ne.le_n);
	}
	if (victim_out != NULL && victim != 0 && !victim_isdir)
		*victim_out = victim;
	rv = FS_APFS_E_OK;
out:
	edit_free(&ne);
	kfree(rec);
	return (rv);
}

/*
 * And the same, with room made underneath it: make_at's loop with a wider
 * ceiling.  Four inserts can be refused -- the new entry (under the
 * destination's id), the inode record (under the child's, longer when the
 * name is), and with an occupant, its inode under the derived name and its
 * new entry in the private directory.  Splitting for one can be answered by
 * another, and four is the ceiling: each leaf needs at most one split.
 */
static int
move_at(uint64_t odir, const char *oname, uint64_t ndir, const char *nname,
    uint64_t now, bool orphan, uint64_t *victim_out)
{
	uint8_t		*scratch;
	uint64_t	 full;
	uint32_t	 tries;
	int		 rv;

	for (tries = 0; ; tries++) {
		full = 0;
		rv = move_once(odir, oname, ndir, nname, now, orphan, &full,
		    victim_out);
		if (rv != FS_APFS_E_NOALLOC || full == 0)
			return (rv);
		if (tries >= 4) {
			kprintf("apfs: the leaf at %llu still has no room for "
			    "\"%s\" after %u split(s) -- whatever is refusing, "
			    "it is not a full node\n", (unsigned long long)full,
			    nname, (unsigned)tries);
			return (rv);
		}

		scratch = kmalloc(APFS_BLOCK_SIZE);
		if (scratch == NULL)
			return (FS_APFS_E_NOMEM);
		rv = node_split_at(full, 0, g_apfs.ac_xid + 1, scratch);
		kfree(scratch);
		if (rv != FS_APFS_E_OK)
			return (rv);
	}
}

int
fs_apfs_rename(uint64_t odir, const char *oname, uint64_t ndir,
    const char *nname, uint64_t now, uint64_t *victim_out)
{

	return (move_at(odir, oname, ndir, nname, now, false, victim_out));
}

/*
 * Which object a name in a directory stands for.  The orphan calls need it
 * first: the name a file waits under is derived from its object id.
 */
static int
dirent_find(uint64_t dir, const char *name, uint64_t *child, bool *isdir)
{
	struct dirent_search	ds;
	uint8_t			dkey[12 + APFS_MAKE_NAME_MAX + 1];
	uint32_t		dklen;
	uint32_t		nlen;
	int			rv;
	bool			stopped;

	nlen = (uint32_t)str_len(name);
	if (nlen == 0 || nlen > APFS_MAKE_NAME_MAX)
		return (FS_APFS_E_INVAL);
	rv = drec_key(dir, name, nlen, dkey, &dklen, true);
	if (rv != FS_APFS_E_OK)
		return (rv);

	ds.ds_name    = name;
	ds.ds_namelen = nlen;
	ds.ds_parent  = dir;
	ds.ds_found   = 0;
	ds.ds_is_dir  = false;
	ds.ds_keyed   = true;
	stopped = false;
	if (!btree_scan(view_root(), dkey, dklen, dirent_match, &ds,
	    0, &stopped))
		return (FS_APFS_E_IO);
	if (ds.ds_found == 0)
		return (FS_APFS_E_NOTFOUND);
	*child = ds.ds_found;
	*isdir = ds.ds_is_dir;
	return (FS_APFS_E_OK);
}

int
fs_apfs_orphan(uint64_t dir, const char *name, uint64_t now, uint64_t *ino_out)
{
	char		dead[APFS_ORPHAN_NAME_MAX];
	uint64_t	child;
	int		rv;
	bool		isdir;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);
	rv = dirent_find(dir, name, &child, &isdir);
	if (rv != FS_APFS_E_OK)
		return (rv);
	if (isdir)
		return (FS_APFS_E_ISDIR);

	orphan_name(child, dead);
	rv = move_at(dir, name, APFS_PRIV_DIR_INO, dead, now, true, NULL);
	if (rv != FS_APFS_E_OK)
		return (rv);
	if (ino_out != NULL)
		*ino_out = child;
	return (FS_APFS_E_OK);
}

int
fs_apfs_reap(uint64_t ino, uint64_t now)
{
	struct inode_info	ii;
	char			dead[APFS_ORPHAN_NAME_MAX];
	int			rv;

	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);

	/*
	 * Asked for proof first, because this destroys and the caller's word
	 * is a counter kept elsewhere.  Taking the bytes of an inode that
	 * still has a name would leave a live entry pointing at nothing, the
	 * shape a wrong reference count would produce.
	 */
	rv = inode_info(ino, &ii);
	if (rv != FS_APFS_E_OK)
		return (rv);
	if (!ii.ii_orphan) {
		kprintf("apfs: inode %llu still has a name -- it is not "
		    "waiting to be let go\n", (unsigned long long)ino);
		return (FS_APFS_E_INVAL);
	}

	orphan_name(ino, dead);
	rv = unmake_at(APFS_PRIV_DIR_INO, dead, now, false);
	if (rv != FS_APFS_E_OK)
		return (rv);
	reap_n++;
	return (FS_APFS_E_OK);
}

struct orphan_probe {
	uint64_t	op_child;
	bool		op_any;
};

static bool
orphan_first(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct orphan_probe	*op;

	(void)key;
	(void)klen;
	(void)bno;
	op = arg;
	/*
	 * The scan began at the private directory's first possible entry, so
	 * the first record handed over is either one of its names or proof
	 * that it has none.  Nothing behind it is worth reading either way.
	 */
	if (type != APFS_TYPE_DIR_REC || oid != APFS_PRIV_DIR_INO)
		return (false);
	if (vlen >= sizeof(struct apfs_drec_val)) {
		op->op_child = ((const struct apfs_drec_val *)val)->dv_file_id;
		op->op_any   = true;
	}
	return (false);
}

int
fs_apfs_reap_all(uint64_t now, uint32_t *n_out)
{
	struct orphan_probe	op;
	uint8_t			dkey[APFS_DREC_KEY_MAX];
	uint32_t		dklen;
	uint32_t		n;
	int			rv;
	bool			stopped;

	*n_out = 0;
	if (!g_apfs.ac_mounted)
		return (FS_APFS_E_NOMOUNT);

	/*
	 * One at a time, asking the tree again after each: a reap rewrites
	 * leaves, so a list gathered beforehand would go stale.  Bounded, so a
	 * reap that succeeds without removing its entry cannot spin the mount.
	 */
	for (n = 0; n < 4096; n++) {
		op.op_child = 0;
		op.op_any   = false;
		drec_low_key(APFS_PRIV_DIR_INO, dkey, &dklen);
		stopped = false;
		if (!btree_scan(view_root(), dkey, dklen,
		    orphan_first, &op, 0, &stopped))
			return (FS_APFS_E_IO);
		if (!op.op_any)
			break;
		rv = fs_apfs_reap(op.op_child, now);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs: inode %llu was left in the private "
			    "directory and cannot be let go (%d)\n",
			    (unsigned long long)op.op_child, rv);
			return (rv);
		}
	}
	if (n >= 4096) {
		kprintf("apfs: the private directory is still not empty after "
		    "%u reaps -- giving up rather than looping\n", n);
		return (FS_APFS_E_INVAL);
	}

	*n_out = n;
	if (n != 0)
		kprintf("apfs: %u file(s) were left waiting in the private "
		    "directory by an earlier boot and have been let go\n", n);
	return (FS_APFS_E_OK);
}

uint64_t
fs_apfs_moves(void)
{

	return (move_n);
}

uint64_t
fs_apfs_clobbers(void)
{

	return (clob_n);
}

uint64_t
fs_apfs_extref_grows(void)
{

	return (extgrow_n);
}

uint64_t
fs_apfs_extref_splits(void)
{

	return (extsplit_n);
}

uint64_t
fs_apfs_extref_drops(void)
{

	return (extdrop_n);
}

uint64_t
fs_apfs_orphans(void)
{

	return (orph_n);
}

uint64_t
fs_apfs_reaps(void)
{

	return (reap_n);
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

uint64_t
fs_apfs_dirmakes(void)
{

	return (dmake_n);
}

uint64_t
fs_apfs_dirkills(void)
{

	return (dkill_n);
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
	/*
	 * Past this directory's entries: the scan began at the first of them,
	 * so any other record ends the directory, which is also how an index
	 * beyond the last name reports that there is no such entry.
	 */
	if (type != APFS_TYPE_DIR_REC || oid != rs->rs_dir)
		return (false);
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
	uint8_t			dkey[APFS_DREC_KEY_MAX];
	uint64_t		oid;
	uint32_t		dklen;
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
	drec_low_key(oid, dkey, &dklen);
	stopped = false;
	if (!btree_scan(view_root(), dkey, dklen, readdir_pick, &rs,
	    0, &stopped))
		return (FS_APFS_E_IO);
	if (!rs.rs_hit)
		return (0);

	/*
	 * A directory entry carries a name and an object id but no length;
	 * that is in the inode, one more descent.  Not fatal if missing: a
	 * name with an unknown size beats failing the whole enumeration.
	 */
	if (!out->ade_is_dir && inode_info(out->ade_ino, &ii) == FS_APFS_E_OK)
		out->ade_size = ii.ii_size;
	return (1);
}

/*
 * Mount-time listing, so the banner shows the tree actually resolved by
 * name.  It also remembers the first small regular file it saw, which
 * fs_apfs_init then reads: a size out of an inode proves the metadata path,
 * and only bytes off the disk prove the extent path.
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
 * Read one file at mount and report a byte sum: trivially reproducible on
 * the host that wrote the image, which is the point.
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
	 * The floor of the view window, as fq_floor explains: the checkpoint
	 * before this one is all a fresh mount can vouch for.
	 */
	fq_floor = g_apfs.ac_xid > 1 ? g_apfs.ac_xid - 1 : 1;

	/*
	 * The ephemeral layer.  Not required to mount -- nothing a file read
	 * touches lives there -- so a container whose checkpoint maps cannot be
	 * read still mounts, and only the accounting goes unanswered.
	 */
	if (read_checkpoint_maps(scratch) == FS_APFS_E_OK) {
		if (read_spaceman(scratch) == FS_APFS_E_OK) {
			/*
			 * The chunk walk needs three blocks live at once, so
			 * it borrows two more for the walk only.
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
			 * After the walk: ip_load checks that the two blocks
			 * it found are inside the pool and marked taken.
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
	 * metadata lives in, not whichever chunk the bitmap walk met first, so
	 * that a copy lands near what it replaces.
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
struct alloc_chunk *
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
 * Bring the chunk covering `bno` into memory, or say why not.  A wholly free
 * chunk has no bitmap block, and giving it one is a different operation
 * that nothing here needs yet.
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
struct alloc_chunk *
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
 * All in memory; alloc_flush writes the bitmap and the chunk-info block when
 * the checkpoint closes.  The three edits must agree -- a bitmap that says a
 * block is taken while the chunk-info counts it free is a container apfsck
 * rejects -- but nothing here is visible until a checkpoint publishes all of
 * it, so no other transaction is needed.
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
 * use, so this cannot pick one up.  Near keeps a copy's release in a chunk
 * already held and a file's extents together; failing that, metadata's own
 * chunk is tried.
 */
int
alloc_blocks(uint32_t count, uint64_t near, uint64_t *first_out)
{
	struct alloc_chunk	*ch;
	int			 rv;

	if (!g_apfs.ac_alloc_have || count == 0)
		return (FS_APFS_E_INVAL);
	if (view_forbids("an allocation"))
		return (FS_APFS_E_INVAL);

	ch = (near != 0) ? chunk_for(near) : NULL;
	if (ch != NULL) {
		/*
		 * Exactly there first, when it is free: a caller naming a block
		 * usually wants its run continued, and first-fit managed that
		 * only four times in six, the transaction's metadata copies
		 * taking the block in between.  Landing on it is lengthening a
		 * record rather than adding one.
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
 * Give a run back: into the device's free queue, keyed by the releasing
 * transaction.  The bits stay set until fq_release, the only thing that
 * makes a block free again.  The chunk is admitted here, so a run this
 * kernel cannot reach is refused by a caller that still has a choice.
 */
int
free_blocks(uint64_t first, uint32_t count)
{
	struct alloc_chunk	*ch;

	if (!g_apfs.ac_alloc_have)
		return (FS_APFS_E_INVAL);
	if (view_forbids("a release"))
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
 * Put the dirty chunk bitmaps and the chunk-info block down, once per
 * checkpoint: each goes to a fresh internal-pool block and the space manager
 * is pointed at the new chunk-info block, so until the checkpoint commits the
 * old blocks still hold.  Not once per allocation: a spine update allocates
 * half a dozen times, each costing two pool blocks, and the pool is fifteen.
 * A bitmap is written raw (no header); the chunk-info block is physical, its
 * oid its block number.
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
	 * Every bitmap that changed moves, then the chunk-info block that
	 * names them; a clean chunk's bitmap is left where it is.
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
 * The spine.  A virtual object -- a tree node, a volume superblock -- is
 * found through an object map; a copy is unreachable until the map says so,
 * the map is physical and must itself be copied, and so on up to the
 * container superblock.  Seven objects for one inode timestamp:
 *
 *	leaf -> volume omap tree -> volume omap -> volume superblock
 *	     -> container omap tree -> container omap -> nx_superblock
 *
 * Children are named by oid, so within the fs tree the path copy is one node
 * long.  cow_physical copies a physical object, whose oid is its block.
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
	o->o_oid = bno;		/* physical: the oid is the block number */
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
 * Take an entry out of a node whose records are all one size.  No hole
 * chain: leaf_insert_fixed takes only from the free span, so an object map
 * gaining and losing an entry per transaction would leak room.  The hole is
 * filled instead: the last record placed in the key area (not last in key
 * order) moves into the freed bytes and its table entry is repointed; the
 * same on the value side.
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
 * Apply one struct omap_edit to an object map node -- repoint the moved
 * oids, drop the gone ones, insert the new ones -- and copy the node.  A
 * moved oid's entry is replaced, not given a second version: with no
 * snapshots nothing needs the old one, and the node does not grow.
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
	 * Several at once: one insert moves both the leaf it lands in and the
	 * root whose key count it changes, and a pass for each would copy this
	 * node, and six spine objects with it, once per pass.
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
	 * And new objects: the upper half of a split, or both halves of a root
	 * that splits (it keeps its own oid, which the volume superblock
	 * names).  Writing their blocks did not make them reachable; these
	 * entries do.
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
 * A run of blocks has moved: tell the extent reference tree, which says who
 * owns a run and how many references it has; apfsck checks it against the
 * file extents.  The record's key is the run's first block, so it changes
 * and the record re-sorts.  Within one leaf only its table entry moves;
 * across leaves it is carried out through the shrink and in through the
 * insert, and a failure between the two leaves this checkpoint
 * unpublishable, said out loud.
 */
static int
extref_move(uint64_t old_start, uint64_t new_start, uint64_t blocks,
    uint64_t xid, void *buf)
{
	struct apfs_btree_node_phys	*n;
	struct apfs_phys_ext_val	 keepv;
	struct apfs_phys_ext_val	*pv;
	struct extref_walk		 ew;
	struct extref_walk		 tw;
	struct btree_layout		 bl;
	struct apfs_kvloc		*kv;
	struct apfs_kvloc		 save;
	uint8_t				*root;
	uint8_t				*node;
	uint64_t			 raw;
	uint32_t			 koff, klen, voff, vlen;
	uint32_t			 i;
	uint32_t			 pos;
	int				 rv;
	bool				 dropped;

	root = kmalloc(APFS_BLOCK_SIZE);
	if (root == NULL)
		return (FS_APFS_E_NOMEM);
	rv = extref_descend(old_start, root, buf, &ew);
	if (rv != FS_APFS_E_OK)
		goto out;
	node = ew.ew_two ? (uint8_t *)buf : root;
	n    = (struct apfs_btree_node_phys *)node;
	btree_layout(node, &bl);
	if (bl.bl_nkeys == 0) {
		rv = FS_APFS_E_INVAL;
		goto out;
	}

	for (i = 0; i < bl.bl_nkeys; i++) {
		btree_entry_loc(&bl, i, &koff, &klen, &voff, &vlen);
		raw = *(const uint64_t *)(bl.bl_keys + koff);
		if ((raw >> APFS_J_OBJ_TYPE_SHIFT) != APFS_TYPE_EXTENT)
			continue;
		if ((raw & APFS_J_OBJ_ID_MASK) != old_start)
			continue;
		if (vlen < sizeof(*pv)) {
			rv = FS_APFS_E_INVAL;
			goto out;
		}
		break;
	}
	if (i == bl.bl_nkeys) {
		kprintf("apfs: no physical extent record starts at %llu -- the "
		    "run being moved is not the whole of one\n",
		    (unsigned long long)old_start);
		rv = FS_APFS_E_NOTFOUND;
		goto out;
	}

	/*
	 * Length in blocks here, in bytes in the file extent.  Checked: moving
	 * a record for a different span would leave the two trees each
	 * consistent and disagreeing.
	 */
	pv = (struct apfs_phys_ext_val *)(bl.bl_vals - voff);
	if ((pv->pe_len_and_kind & APFS_PEXT_LEN_MASK) != blocks) {
		kprintf("apfs: the extent at %llu is %llu blocks here and %llu "
		    "in the file -- not moving it\n",
		    (unsigned long long)old_start,
		    (unsigned long long)(pv->pe_len_and_kind &
		    APFS_PEXT_LEN_MASK), (unsigned long long)blocks);
		rv = FS_APFS_E_INVAL;
		goto out;
	}

	/*
	 * Where the new key would land.  The same descent the old one took;
	 * only when both answers name one node is the cheap dance below safe.
	 */
	if (ew.ew_two) {
		struct btree_layout	 rb;
		uint32_t		 rkoff, rklen, rvoff, rvlen;
		uint32_t		 j;

		tw.ew_slot = 0;
		btree_layout(root, &rb);
		for (j = 1; j < rb.bl_nkeys; j++) {
			btree_entry_loc(&rb, j, &rkoff, &rklen, &rvoff,
			    &rvlen);
			raw = *(const uint64_t *)(rb.bl_keys + rkoff);
			if ((raw & APFS_J_OBJ_ID_MASK) > new_start)
				break;
			tw.ew_slot = j;
		}
		if (tw.ew_slot != ew.ew_slot) {
			keepv = *pv;
			rv = extref_shrink(old_start, 0, xid, buf, &dropped);
			if (rv != FS_APFS_E_OK)
				goto out;
			rv = extref_insert_pv(new_start, &keepv, xid);
			if (rv != FS_APFS_E_OK)
				kprintf("apfs: the run moved to %llu came out "
				    "of one leaf and would not go into "
				    "another (%d) -- this checkpoint must "
				    "not be written\n",
				    (unsigned long long)new_start, rv);
			goto out;
		}
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

	/*
	 * Physical, so nothing resolves an oid to find it: the volume
	 * superblock names the root's block outright, and spine_update writes
	 * ac_extref_bno in when it copies that superblock.
	 */
	rv = extref_settle(&ew, xid, root, buf);
out:
	kfree(root);
	return (rv);
}

/*
 * Virtual objects have moved, been made or gone (`oe`): make that the answer
 * everything up to the container superblock gives.  Every step is a copy;
 * the last leaves the new container object map in ac_omap_oid for the
 * checkpoint writer, and until then none of it is reachable.
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
	 * virtual: its oid is a name the container's map resolves, so the
	 * copy keeps the oid it had and only its address changes.
	 */
	rv = fs_apfs_read_block(g_apfs.ac_vol_sb_bno, buf);
	if (rv != FS_APFS_E_OK)
		return (rv);
	vsb = (struct apfs_superblock *)buf;
	vsb->apfs_omap_oid = new_vol_omap;
	/*
	 * The extent reference tree is named from here too, and moves for its
	 * own reasons.  Written unconditionally so the two need not know about
	 * each other: whoever moved it left the new address in ac_extref_bno,
	 * and otherwise this writes back what is already there.
	 */
	vsb->apfs_extentref_tree_oid = g_apfs.ac_extref_bno;
	vsb->apfs_fs_alloc_count     = g_apfs.ac_fs_alloc_count;
	/*
	 * And what the volume says about its own contents, on the same terms.
	 * apfs_next_obj_id is checked (a created inode whose number the volume
	 * still calls free stops apfsck first); apfs_num_files is not checked
	 * at all, and is written as the volume's own claim for other readers.
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
	 * leaves blocks allocated to a chain nothing points at, which the next
	 * checkpoint would publish as a leak.
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
 * Writing a checkpoint.
 *
 * Everything above builds the next state out of copies; a checkpoint is the
 * instant it becomes the container's.  It too is written entirely into
 * blocks nobody is reading:
 *
 *	0. once the free queues have let go of what is old enough, the
 *	   allocation metadata and the pool bitmap (alloc_flush, ip_rotate);
 *	1. the ephemeral objects, copied into the next free slots of the data
 *	   ring, each carrying the new xid;
 *	2. a checkpoint map naming where they landed, into the next free slot
 *	   of the descriptor ring;
 *	3. a superblock after it, naming the new container object map, whose
 *	   landing is the commit -- before that write the container is the old
 *	   checkpoint entire, after it the new one entire, and there is no
 *	   third state;
 *	4. block zero, a copy of that superblock.
 *
 * Step 4 is not bookkeeping: a container whose block zero names an older
 * checkpoint than the ring holds is one apfsck calls "not unmounted
 * cleanly".  A crash between 3 and 4 leaves that state on purpose:
 * consistent, mountable at the new xid, and marked as interrupted.
 *
 * Ordering holds because bio_write reaches the device before the cache
 * (fs/bio.c) and ata_kwrite ends every write with FLUSH CACHE
 * (dev/ata_drv.c): the order below is the order the disk sees.  Hence not
 * fs_txn, whose transactions are unordered sets of blocks.
 */

static uint64_t	ckpt_n_written;		/* checkpoints committed */
static uint64_t	ckpt_n_refused;		/* asked for and declined */

/*
 * Is slot `s` in the run of `len` slots from `start` in a ring of `blocks`?
 * Refuses a checkpoint written over the one being read, which would
 * checksum perfectly and which nothing later would catch.
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
	 * A checkpoint that does not re-emit the ephemeral objects leaves its
	 * space manager in the previous one's slots, which are the next to be
	 * reused.  So a table that is empty, or known to be missing entries,
	 * refuses rather than writing three quarters of a checkpoint.
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
	 * The xid to write.  The superblock's nx_next_xid should agree; the two
	 * are derived differently, so a disagreement is worth saying even
	 * though the answer taken is the same.
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
	 * The superblock is read first: it is the one block this depends on,
	 * and finding it changed or unreadable must stop the checkpoint while
	 * the disk is untouched.
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
	 * 0. the allocation metadata, then the pool's own bitmap: putting the
	 * chunk bitmaps down takes pool blocks and returns as many, so a pool
	 * bitmap written first would be stale.  Both go before the space
	 * manager of step 1, which names where they landed.
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
		 * The space manager and the free-queue trees come from memory,
		 * where they live; any other ephemeral object is only ever
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
	 * Where the next virtual object id comes from: a split needs one for
	 * its new half, and the counter is the container's, not the volume's
	 * apfs_next_obj_id, which numbers inodes.
	 */
	nx->nx_next_oid      = g_apfs.ac_next_oid;
	/*
	 * The one pointer every copy-on-write chain ends at; this write makes
	 * the whole chain reachable.
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
	 * 4. block zero.  Past this point the checkpoint has happened, so a
	 * failure is reported, not propagated: it only decides whether the
	 * next fsck calls the container cleanly unmounted.
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
	/* And the pool bitmap that was written is now the live one. */
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

/*
 * Checkpoints written since boot.  A checkpoint is when the free queue lets
 * go of blocks, which a first-fit scan may then prefer, so a test's claim
 * about where allocations land holds only over a window no checkpoint
 * interrupted; this is how the test knows whether one did.
 */
uint64_t
fs_apfs_ckpts(void)
{

	return (ckpt_n_written);
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
	kprintf("apfs: %llu tree reads -- %llu descended on a key, %llu read "
	    "every record; %llu nodes, %llu records, %llu keys compared "
	    "(%llu nodes, %llu records each)\n",
	    (unsigned long long)(g_n_walks + g_n_seeks),
	    (unsigned long long)g_n_seeks,
	    (unsigned long long)g_n_walks,
	    (unsigned long long)g_n_nodes,
	    (unsigned long long)g_n_recs,
	    (unsigned long long)g_n_cmps,
	    (unsigned long long)((g_n_walks + g_n_seeks) ?
	    g_n_nodes / (g_n_walks + g_n_seeks) : 0),
	    (unsigned long long)((g_n_walks + g_n_seeks) ?
	    g_n_recs / (g_n_walks + g_n_seeks) : 0));

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
	 * Blocks neither in use nor available: given up by some transaction and
	 * waiting on its age.  A bitmap alone cannot express that, which is why
	 * freeing here is a B-tree insert rather than clearing a bit.
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
	 * How much of the disk the check looked at, printed with its result:
	 * chunks not bit-counted are taken on trust.
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
	 * The three free counts must be read at one instant: the walk's
	 * totals are from mount, so they are brought up to date with what each
	 * resident chunk has changed since it was admitted.
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
	 * Written and refused: written alone cannot tell a boot where nothing
	 * asked for a checkpoint from one that refused them.
	 */
	if (ckpt_n_written != 0 || ckpt_n_refused != 0)
		kprintf("apfs: %llu checkpoint(s) written, %llu refused -- now "
		    "at xid %llu, superblock in block %llu\n",
		    (unsigned long long)ckpt_n_written,
		    (unsigned long long)ckpt_n_refused,
		    (unsigned long long)g_apfs.ac_xid,
		    (unsigned long long)g_apfs.ac_sb_bno);

	/*
	 * The published past: how far back it reaches now, and how often it
	 * was asked for, refusals included -- they show the window's edge was
	 * tested.
	 */
	if (view_n_open != 0 || view_n_gone != 0 || view_n_forbid != 0)
		kprintf("apfs: views -- checkpoints %llu..%llu readable, %llu "
		    "opened, %llu entered, %llu refused as let go, %llu "
		    "write(s) refused under one\n",
		    (unsigned long long)fq_floor,
		    (unsigned long long)g_apfs.ac_xid,
		    (unsigned long long)view_n_open,
		    (unsigned long long)view_n_enter,
		    (unsigned long long)view_n_gone,
		    (unsigned long long)view_n_forbid);

	/* The pool and the spine, and what copy-on-write has cost them. */
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
	 * And what the tree has had done to its shape.  The last is the quiet
	 * one: an index key corrected because an edit moved the first record
	 * of the node under it.
	 */
	if (split_n != 0 || deep_n != 0 || reidx_n != 0 || gone_n != 0)
		kprintf("apfs: tree shape -- %llu node(s) split, %llu dropped, "
		    "%llu level(s) gained, %llu index key(s) corrected\n",
		    (unsigned long long)split_n, (unsigned long long)gone_n,
		    (unsigned long long)deep_n, (unsigned long long)reidx_n);

	/*
	 * And the queues: what is still waiting rises while a boot works and
	 * falls as checkpoints age out.  Read from the queues, not as queued
	 * minus let go: a container arrives with entries queued already.
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
