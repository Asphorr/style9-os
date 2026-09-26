/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "apfs.h"
#include "apfs_priv.h"
#include "bio.h"
#include "kmem.h"
#include "kprintf.h"

/*
 * Boot-time self-tests of the APFS writer.  Each arranges its case rather
 * than waiting for it, then asks the disk rather than the kernel's idea of
 * the disk.  apfsck (apfsprogs) is the outside oracle for the format; what
 * it cannot check is what this kernel believes about key order, hence
 * apfs-seek, whose oracle is the whole-tree walk.  The internals used here
 * come from apfs_priv.h.
 */

/*
 * The three free counts, read off the disk from blocks the caller names:
 * with copy-on-write, the live checkpoint's bitmap and the one being built
 * are different blocks, and following the current pointers only finds the
 * second.
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
 * Is every block of this run marked taken on the disk right now?  1 yes,
 * 0 no, negative if the bitmap would not read.  Asked of the bits, not the
 * free counts, which every checkpoint moves as the queues release.
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
 * Allocate, look, put back.
 *
 * A block marked in use that nothing references is invalid ("Space manager:
 * bad allocation bitmap" from apfsck): an allocation is only half of an
 * operation whose other half is a reference.  So this proves everything up
 * to that half -- take a run, see the disk agree, give it back -- and the
 * state apfsck rejects does not outlive the call.
 *
 * It also asks whether, after the allocation and before the checkpoint, the
 * live checkpoint still reads exactly as it did.  A writer that got
 * copy-on-write subtly wrong (the bitmap copied but not the chunk-info, the
 * pointer moved before the block) passes every count and fails that.
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
	 * First: the allocation is complete as far as this kernel knows, and
	 * the blocks the live checkpoint names still read as before.
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
	 * Second, the free queue's reason to exist: giving the run back does
	 * not make it free, since the checkpoints behind this one still use
	 * those blocks.  After the release is published the bits are still set.
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
	 * Third: once the releasing transaction is APFS_FQ_KEEP checkpoints
	 * behind, the queue lets go and the blocks are free again.
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
	 * Fourth, about the queue's own node: a release puts the space of the
	 * entry it removed on the node's free lists, and an insert must reuse
	 * it.  A queue that empties has its node reset outright, but a busy one
	 * never empties, and a node that only eats into its span loses an
	 * entry's room per cycle until it refuses a release -- a leaked block.
	 *
	 * So the queue is kept busy (one release per checkpoint stops it
	 * emptying) and the node measured over two stretches at the same
	 * depth; the second must cost it nothing.  Two cycles with a drain
	 * between them would pass on a node that reuses nothing, because the
	 * drain resets it.
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
	 * Fifth: a file's bytes need not be in the chunk its metadata is
	 * allocated from, so relocating them takes a run in one chunk and gives
	 * one back in another within one transaction -- two bitmaps, dirtied
	 * and written apart.  Asked of a chunk other than g_home, so failing to
	 * reach it fails here rather than at the first write.
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
 * A write moves the bytes, and the checkpoint behind it keeps its own: after
 * a write, the block the previous checkpoint names holds exactly what it
 * held.  Asked of that block, read raw, since nothing current points at it;
 * following the current records would only show the new copy.
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
	 * First: the bytes are somewhere else now.  A write in place passes
	 * every read-back check and fails only this one.
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

	/* Second: the new block received the write. */
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
	 * Third: the old block, which every earlier checkpoint names, reads
	 * byte for byte as before the write.
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
		kprintf("apfs-data: FAIL byte %u of the block the previous "
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
	    "the write, and the run the previous checkpoint names is "
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
 * A node splits and nothing is lost.  Asked for directly: appends merge
 * touching extents, so a sequential writer never fills a leaf.
 *
 * Counting records through the walk proves none was lost or duplicated, but
 * a walk cannot see a wrong separator -- it visits every child whatever the
 * keys say, and the tree is quietly out of order ("B-tree: keys are out of
 * order" from apfsck).  So the index is checked too, by the invariant a
 * split must keep: the key a parent stores for a child is the child's own
 * first key.
 */

/*
 * Every separator in the tree against the child it names, at every level.
 * Recursive, because a wrong separator written into an interior node by a
 * split of another interior node is invisible from the root.
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
	 * The depth before.  This test adds a node per boot and never gives
	 * one back, so the root eventually fills and the split grows the tree
	 * instead; the root's level and deep_n tell the two apart.
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
	 * At least one, more when the leaf's parent was full and had to split
	 * before it could take a separator.
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

	/* The half the walk is blind to: every separator against its child. */
	if (!index_check(g_apfs.ac_root_tree_bno, 0))
		return;

	/* And a file, because a count can be right while a lookup is not. */
	if (fs_apfs_stat("/var/db/big.txt", &st) != FS_APFS_E_OK) {
		kprintf("apfs-split: FAIL /var/db/big.txt cannot be found "
		    "through the split tree\n");
		return;
	}

	/*
	 * A growth is not a split.  Both leave a tree that walks and counts
	 * right; only a growth changes the depth, and a level gained without
	 * deep_n moving (or the reverse) means the two disagree.
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
 * Where an inode's own record is: its leaf, its slot there, and the leaf's
 * record count.  Asked afresh each time, since a split moves the record.
 */
static bool
inode_slot_of(uint64_t oid, uint8_t *scratch, uint64_t *bno_out,
    uint32_t *at_out, uint32_t *nkeys_out)
{
	struct btree_layout	bl;
	uint64_t		bno;
	uint32_t		koff, klen, voff, vlen;
	uint32_t		pos;

	if (inode_where(oid, &bno) != FS_APFS_E_OK)
		return (false);
	if (fs_apfs_read_block(bno, scratch) != FS_APFS_E_OK)
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

	*bno_out   = bno;
	*at_out    = pos;
	*nkeys_out = bl.bl_nkeys;
	return (true);
}

/*
 * A node stops starting where its parent says it does.
 *
 * That needs a delete to take the first record out of a leaf, which depends
 * on where splits fell, so it is arranged out of two files.  The leaf holding
 * the first one's inode record is split at that record, making it the key
 * the index files the upper half under; then the file is unlinked and the
 * key must be corrected.  The second file, made just after, sorts into the
 * same half and keeps it from emptying (that case is the next test).  Both
 * are removed again, so the volume ends the boot as it began.
 *
 * The invariant is apfsck's "B-tree: index key absent from child node",
 * checked from inside at every level as the split test does -- plus reidx_n,
 * since an index right because nothing needed correcting proves nothing.
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

	rv = fs_apfs_create(parent, "idxa.txt", now, 0644, &ino_a);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(parent, "idxb.txt", now, 0644, &ino_b);
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

	/* Split so the record starts, and keys, the upper half. */
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
 * And the node itself goes.
 *
 * A new file has the highest key on the volume, so splitting its leaf at
 * its inode record leaves an upper half holding that file's records alone;
 * unlinking it must take the half out of the tree.  Checked three ways --
 * gone_n moved, the tree's node count fell, every node still starts where
 * its parent says -- since the counter alone would pass a writer that
 * unhooked the node without saying so ("wrong node count in info footer").
 *
 * The cascade is arranged too when the tree is deep enough: a parent left
 * with nothing must go the same way, which no ordinary delete reaches, since
 * a split leaves its parent at least two children.  So the node above is
 * split at the entry naming the file's node, leaving that node an only
 * child, and the unlink must take two nodes out.
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
	rv = fs_apfs_create(parent, "drop.txt", now, 0644, &ino);
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
	 * The file must be alone in the upper half, or the half would not
	 * empty and the test would pass without trying anything.
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
	 * Split the node above at the entry naming this one, so the delete
	 * takes two nodes out.  Where the tree does not allow it (the parent
	 * is the root, or this child is not its last), the simpler case runs.
	 */
	expect = 1;
	oid    = ((const struct apfs_obj_phys *)scratch)->o_oid;

	/*
	 * The root may not go, so a node hanging straight off it has no
	 * cascade; a tree that shallow is grown a level first (tree_grow, as
	 * for a full root) to put a node between the leaf and the root.
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

	dropped = gone_n;
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
	if (gone_n - dropped != expect) {
		kprintf("apfs-drop: FAIL %u node(s) should have left the tree "
		    "and %llu did\n", (unsigned)expect,
		    (unsigned long long)(gone_n - dropped));
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

/* Is there still a data stream record under this object id? */
struct stream_probe {
	uint64_t	sp_id;
	bool		sp_found;
};

static bool
stream_see(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct stream_probe	*sp;

	(void)key;
	(void)klen;
	(void)val;
	(void)vlen;
	(void)bno;
	sp = arg;
	/* The descent starts at the record's key; a larger oid is past it. */
	if (oid != sp->sp_id)
		return (false);
	if (type == APFS_TYPE_DSTREAM_ID)
		sp->sp_found = true;
	return (true);
}

/*
 * An inode and its data stream, in two different nodes.
 *
 * A file this kernel makes has two adjacent records under its object id: the
 * inode, and the reference count of its data stream.  Adjacent is the same
 * node only until a split falls between them, and an unlink that looks for
 * the second in the first's leaf leaves the stream behind ("Data stream: has
 * no references" from apfsck) -- while reporting success, since a file off
 * the image may genuinely lack the record.
 *
 * So: make a file, split its leaf at the stream record so the inode ends the
 * lower half and the stream starts the upper, and unlink it.  The claim is
 * not that the unlink succeeded but that no record of the stream is left.
 */
void
fs_apfs_stream_selftest(uint64_t now)
{
	struct stream_probe	 sp;
	uint8_t			*scratch;
	uint64_t		 parent;
	uint64_t		 ino;
	uint64_t		 skey;
	uint64_t		 bno;
	uint64_t		 ino_leaf;
	uint64_t		 ds_leaf;
	uint32_t		 nkeys;
	uint32_t		 at;
	int			 is_dir;
	int			 rv;
	bool			 stopped;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-strm: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup("/etc", &parent, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-strm: /etc is not there -- skipped\n");
		return;
	}

	/* Whatever an interrupted run left, so this starts from nothing. */
	(void)fs_apfs_unlink(parent, "strm.txt", now);
	(void)fs_apfs_checkpoint();

	rv = fs_apfs_create(parent, "strm.txt", now, 0644, &ino);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-strm: FAIL cannot make the file this needs "
		    "(%d)\n", rv);
		return;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-strm: FAIL the checkpoint after making it was "
		    "refused\n");
		return;
	}
	skey = (ino & APFS_J_OBJ_ID_MASK) |
	    ((uint64_t)APFS_TYPE_DSTREAM_ID << APFS_J_OBJ_TYPE_SHIFT);

	scratch = kmalloc(APFS_BLOCK_SIZE);
	if (scratch == NULL) {
		kprintf("apfs-strm: no memory -- skipped\n");
		goto clean;
	}
	if (!inode_slot_of(ino, scratch, &bno, &at, &nkeys)) {
		kprintf("apfs-strm: FAIL inode %llu is not in the tree\n",
		    (unsigned long long)ino);
		kfree(scratch);
		return;
	}
	/*
	 * The cut goes at the stream record, the one after.  A cut at zero is
	 * refused and a last record has nothing after it: skips, not failures.
	 */
	if (at == 0 || at + 1 >= nkeys || bno == g_apfs.ac_root_tree_bno) {
		kprintf("apfs-strm: inode %llu is at slot %u of %u in the node "
		    "at %llu, which cannot be cut after -- skipped\n",
		    (unsigned long long)ino, (unsigned)at, (unsigned)nkeys,
		    (unsigned long long)bno);
		kfree(scratch);
		goto clean;
	}
	rv = node_split_at(bno, at + 1, g_apfs.ac_xid + 1, scratch);
	kfree(scratch);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-strm: the node at %llu would not split at %u "
		    "(%d) -- skipped\n", (unsigned long long)bno,
		    (unsigned)(at + 1), rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-strm: FAIL the checkpoint after the split was "
		    "refused\n");
		return;
	}

	/*
	 * Check the arrangement: records left in one node would let this pass
	 * on the very writer it exists to catch.
	 */
	if (inode_where(ino, &ino_leaf) != FS_APFS_E_OK ||
	    leaf_home((const uint8_t *)&skey, (uint32_t)sizeof(skey),
	    &ds_leaf) != FS_APFS_E_OK) {
		kprintf("apfs-strm: FAIL the tree will not say where inode "
		    "%llu's records are\n", (unsigned long long)ino);
		goto clean;
	}
	if (ino_leaf == ds_leaf) {
		kprintf("apfs-strm: FAIL the cut at slot %u left the inode and "
		    "its stream both in the node at %llu\n", (unsigned)(at + 1),
		    (unsigned long long)ino_leaf);
		goto clean;
	}

	rv = fs_apfs_unlink(parent, "strm.txt", now);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-strm: FAIL cannot unlink a file whose stream "
		    "record is in another node (%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-strm: FAIL the checkpoint after the unlink was "
		    "refused\n");
		return;
	}

	/*
	 * The claim: nothing of the stream is left.  Asked by descending on the
	 * record's own key, where it would be if it were there.
	 */
	sp.sp_id    = ino;
	sp.sp_found = false;
	stopped     = false;
	if (!btree_scan(g_apfs.ac_root_tree_bno, (const uint8_t *)&skey,
	    (uint32_t)sizeof(skey), stream_see, &sp, 0, &stopped)) {
		kprintf("apfs-strm: FAIL the tree will not walk after the "
		    "unlink\n");
		return;
	}
	if (sp.sp_found) {
		kprintf("apfs-strm: FAIL inode %llu is gone and its data "
		    "stream record is still in the tree -- apfsck calls that "
		    "\"Data stream: has no references\"\n",
		    (unsigned long long)ino);
		return;
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-strm: FAIL the index is wrong after an unlink "
		    "spanning two nodes\n");
		return;
	}

	kprintf("apfs-strm: PASS -- inode %llu kept its stream record in the "
	    "node at %llu while its own was at %llu, and unlinking it took "
	    "both\n", (unsigned long long)ino, (unsigned long long)ds_leaf,
	    (unsigned long long)ino_leaf);
	return;

clean:
	(void)fs_apfs_unlink(parent, "strm.txt", now);
	(void)fs_apfs_checkpoint();
}

/*
 * A create makes its own room.
 *
 * A name's records go to two leaves -- the entry sorts under the directory's
 * object id, the inode under its own -- so either can be the full one.  A
 * full leaf is filled here rather than waited for: names go into a directory
 * one at a time, each with its own checkpoint, as an ordinary caller makes
 * them.
 *
 * Checked is what a split can get wrong and a create cannot notice: every
 * name made before the split still resolves to the inode it was given (a
 * record carried into the wrong half reads as a missing file), and every
 * node still starts where its parent says ("index key absent from child
 * node").
 *
 * Not arranged: the leaf that fills is the inodes' (an inode record is nine
 * times an entry's size), so a create finding both its leaves full is left
 * to make_at's retry loop.  All names are removed again; the nodes the splits
 * made stay, for the next boot to fill.
 */
/*
 * APFS_ROOM_LEAF is where the name starts in the path; APFS_ROOM_NAMES is a
 * bound, not an expectation (a leaf takes about eighteen inodes); and
 * APFS_ROOM_AFTER is how many creates must work after the first split.
 */
#define	APFS_ROOM_DIR		"/etc"
#define	APFS_ROOM_LEAF		5
#define	APFS_ROOM_NAMES		32
#define	APFS_ROOM_AFTER		2

/*
 * "/etc/roomNN.txt", byte by byte for want of snprintf.  Used whole for a
 * lookup by path, and from APFS_ROOM_LEAF for calls taking a parent.
 */
static void
room_path(char *buf, uint32_t i)
{

	buf[0]  = '/';
	buf[1]  = 'e';
	buf[2]  = 't';
	buf[3]  = 'c';
	buf[4]  = '/';
	buf[5]  = 'r';
	buf[6]  = 'o';
	buf[7]  = 'o';
	buf[8]  = 'm';
	buf[9]  = (char)('0' + (i / 10u) % 10u);
	buf[10] = (char)('0' + i % 10u);
	buf[11] = '.';
	buf[12] = 't';
	buf[13] = 'x';
	buf[14] = 't';
	buf[15] = '\0';
}

void
fs_apfs_room_selftest(uint64_t now)
{
	char		 path[16];
	uint64_t	*ino;
	uint64_t	 parent;
	uint64_t	 splits;
	uint64_t	 got;
	uint32_t	 made;
	uint32_t	 after;
	uint32_t	 i;
	int		 is_dir;
	int		 rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-room: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup(APFS_ROOM_DIR, &parent, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-room: %s is not there -- skipped\n",
		    APFS_ROOM_DIR);
		return;
	}
	ino = kmalloc(APFS_ROOM_NAMES * (uint32_t)sizeof(*ino));
	if (ino == NULL) {
		kprintf("apfs-room: no memory -- skipped\n");
		return;
	}

	/* Whatever an interrupted run left, so this starts from nothing. */
	for (i = 0; i < APFS_ROOM_NAMES; i++) {
		room_path(path, i);
		(void)fs_apfs_unlink(parent, path + APFS_ROOM_LEAF, now);
	}
	(void)fs_apfs_checkpoint();

	splits = split_n;
	made   = 0;
	after  = 0;
	for (i = 0; i < APFS_ROOM_NAMES; i++) {
		room_path(path, i);
		ino[i] = 0;
		rv = fs_apfs_create(parent, path + APFS_ROOM_LEAF, now, 0644,
		    &ino[i]);
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs-room: FAIL %s was refused (%d) -- name "
			    "%u, with %llu split(s) behind it\n", path, rv,
			    (unsigned)(i + 1),
			    (unsigned long long)(split_n - splits));
			goto clean;
		}
		made++;
		if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
			kprintf("apfs-room: FAIL the checkpoint after making "
			    "%s was refused\n", path);
			goto out;
		}
		/*
		 * Stop a couple of names past the first split, not at it, to
		 * catch a writer that splits and then refuses the record.
		 */
		if (split_n == splits)
			continue;
		after++;
		if (after > APFS_ROOM_AFTER)
			break;
	}

	if (split_n == splits) {
		kprintf("apfs-room: FAIL %u name(s) went into %s and no leaf "
		    "ever ran out of room -- nothing here was proved\n",
		    (unsigned)made, APFS_ROOM_DIR);
		goto clean;
	}
	if (after <= APFS_ROOM_AFTER) {
		kprintf("apfs-room: FAIL the first split came on the last name "
		    "there was room to try -- %u is too low a bound to show "
		    "the writer carrying on\n", (unsigned)APFS_ROOM_NAMES);
		goto clean;
	}

	/* Every one of them, still there and still its own inode. */
	for (i = 0; i < made; i++) {
		room_path(path, i);
		got = 0;
		if (fs_apfs_lookup(path, &got, &is_dir) != FS_APFS_E_OK) {
			kprintf("apfs-room: FAIL %s does not resolve, and it "
			    "did when it was made -- a split carried it "
			    "somewhere the descent does not look\n", path);
			goto clean;
		}
		if (got != ino[i] || is_dir) {
			kprintf("apfs-room: FAIL %s was made as inode %llu and "
			    "resolves to %llu%s\n", path,
			    (unsigned long long)ino[i], (unsigned long long)got,
			    is_dir ? ", as a directory" : "");
			goto clean;
		}
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-room: FAIL the index is wrong after a create "
		    "made room for itself\n");
		goto out;
	}

	/* And away again, all of them. */
	for (i = 0; i < made; i++) {
		room_path(path, i);
		rv = fs_apfs_unlink(parent, path + APFS_ROOM_LEAF, now);
		if (rv == FS_APFS_E_OK && fs_apfs_checkpoint() != FS_APFS_E_OK)
			rv = FS_APFS_E_IO;
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs-room: FAIL cannot take %s back out "
			    "(%d)\n", path, rv);
			goto out;
		}
	}
	room_path(path, 0);
	if (fs_apfs_lookup(path, &got, &is_dir) != FS_APFS_E_NOTFOUND) {
		kprintf("apfs-room: FAIL %s still resolves after being "
		    "unlinked\n", path);
		goto out;
	}

	kprintf("apfs-room: PASS -- %u name(s) into %s filled a leaf, the "
	    "writer split %llu of them and carried on, every one of them was "
	    "still under the inode it was given, and the volume ends as it "
	    "began\n", (unsigned)made, APFS_ROOM_DIR,
	    (unsigned long long)(split_n - splits));
out:
	kfree(ino);
	return;

clean:
	for (i = 0; i < made; i++) {
		room_path(path, i);
		(void)fs_apfs_unlink(parent, path + APFS_ROOM_LEAF, now);
	}
	(void)fs_apfs_checkpoint();
	kfree(ino);
}

/*
 * A name that moves.
 *
 * A rename creates and destroys nothing, so no count shows a half-done one.
 * After every move this asks what the volume can answer: does the new name
 * resolve to the old one's inode, and has the old name stopped resolving?
 *
 * The long names are for the bytes: the file's length lives in an extended
 * field after the name field in the packed inode record, and a rename that
 * rebuilt the record as a create writes one would leave a valid empty file.
 * So the file gets a length and is moved to a longer name and a shorter one,
 * its length asked after each.
 *
 * The directories' child counts are left to apfsck, which re-derives them
 * ("Inode record: wrong directory child count"); this test adds what apfsck
 * cannot see, the refusals, which leave no trace.  A move that meets a full
 * leaf takes the create's retry loop (arranged by apfs-room), and is reported
 * here when it happens, not required.
 */
#define	APFS_MOVE_DIR		"/etc"
#define	APFS_MOVE_OTHER		"/var"
#define	APFS_MOVE_SIZE		9000u

/* One move, and the three things that must be true after it. */
static bool
moved_ok(uint64_t odir, const char *oname, uint64_t ndir, const char *nname,
    uint64_t now, const char *opath, const char *npath, uint64_t want)
{
	uint64_t	got;
	int		is_dir;
	int		rv;

	rv = fs_apfs_rename(odir, oname, ndir, nname, now, NULL);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL %s -> %s was refused (%d)\n", opath,
		    npath, rv);
		return (false);
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL the checkpoint after %s -> %s was "
		    "refused\n", opath, npath);
		return (false);
	}
	got = 0;
	if (fs_apfs_lookup(npath, &got, &is_dir) != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL %s does not resolve after the move\n",
		    npath);
		return (false);
	}
	if (got != want) {
		kprintf("apfs-move: FAIL %s is inode %llu and %s named %llu\n",
		    npath, (unsigned long long)got, opath,
		    (unsigned long long)want);
		return (false);
	}
	if (fs_apfs_lookup(opath, &got, &is_dir) != FS_APFS_E_NOTFOUND) {
		kprintf("apfs-move: FAIL %s still resolves after moving to "
		    "%s\n", opath, npath);
		return (false);
	}
	return (true);
}

/*
 * Does the volume still give the file length `want`?  Complains as `who`,
 * since more than one test asks.
 */
static bool
size_still(const char *who, uint64_t ino, uint64_t want, const char *where)
{
	uint64_t	got;

	got = 0;
	if (fs_apfs_size(ino, &got) != FS_APFS_E_OK) {
		kprintf("%s: FAIL inode %llu has no length at all %s\n", who,
		    (unsigned long long)ino, where);
		return (false);
	}
	if (got != want) {
		kprintf("%s: FAIL inode %llu is %llu bytes %s and was %llu -- "
		    "the data stream did not come across\n", who,
		    (unsigned long long)ino, (unsigned long long)got, where,
		    (unsigned long long)want);
		return (false);
	}
	return (true);
}

void
fs_apfs_move_selftest(uint64_t now)
{
	uint64_t	etc, var;
	uint64_t	file, dir, inner, deep;
	uint64_t	splits;
	uint64_t	moves;
	uint64_t	got;
	int		is_dir;
	int		rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-move: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup(APFS_MOVE_DIR, &etc, &is_dir) != FS_APFS_E_OK ||
	    !is_dir ||
	    fs_apfs_lookup(APFS_MOVE_OTHER, &var, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-move: %s and %s are not both there -- skipped\n",
		    APFS_MOVE_DIR, APFS_MOVE_OTHER);
		return;
	}

	/*
	 * Whatever an interrupted run left behind.  The directory is looked for
	 * under each name it can have, and emptied through its object id, the
	 * one thing no move changes.
	 */
	got = 0;
	if (fs_apfs_lookup("/etc/mvdir", &got, &is_dir) == FS_APFS_E_OK ||
	    fs_apfs_lookup("/etc/mvdir2", &got, &is_dir) == FS_APFS_E_OK ||
	    fs_apfs_lookup("/var/mvdir2", &got, &is_dir) == FS_APFS_E_OK) {
		(void)fs_apfs_rmdir(got, "deeper", now);
		(void)fs_apfs_unlink(got, "inner.txt", now);
	}
	(void)fs_apfs_unlink(etc, "mvone.txt", now);
	(void)fs_apfs_unlink(etc, "mvtwo.txt", now);
	(void)fs_apfs_unlink(etc, "mvconsiderablylongername.txt", now);
	(void)fs_apfs_unlink(etc, "mvs", now);
	(void)fs_apfs_unlink(var, "mvone.txt", now);
	(void)fs_apfs_rmdir(var, "mvdir2", now);
	(void)fs_apfs_rmdir(etc, "mvdir2", now);
	(void)fs_apfs_rmdir(etc, "mvdir", now);
	(void)fs_apfs_checkpoint();

	splits = split_n;
	moves  = fs_apfs_moves();
	file   = 0;
	dir    = 0;
	inner  = 0;
	deep   = 0;

	rv = fs_apfs_create(etc, "mvone.txt", now, 0644, &file);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL cannot make the file to move (%d)\n",
		    rv);
		return;
	}
	/* Bytes under it: an empty file cannot show a lost data stream. */
	rv = fs_apfs_grow(file, file, APFS_MOVE_SIZE);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL cannot give the file a length (%d)\n",
		    rv);
		goto clean;
	}
	if (!size_still("apfs-move", file, APFS_MOVE_SIZE, "before any move"))
		goto clean;

	/* Same directory, same length of name: the plainest move there is. */
	if (!moved_ok(etc, "mvone.txt", etc, "mvtwo.txt", now,
	    "/etc/mvone.txt", "/etc/mvtwo.txt", file))
		goto clean;

	/* Longer, so the record grows; then shorter, so it shrinks. */
	if (!moved_ok(etc, "mvtwo.txt", etc, "mvconsiderablylongername.txt",
	    now, "/etc/mvtwo.txt", "/etc/mvconsiderablylongername.txt", file))
		goto clean;
	if (!size_still("apfs-move", file, APFS_MOVE_SIZE,
	    "under a longer name"))
		goto clean;
	if (!moved_ok(etc, "mvconsiderablylongername.txt", etc, "mvs", now,
	    "/etc/mvconsiderablylongername.txt", "/etc/mvs", file))
		goto clean;
	if (!size_still("apfs-move", file, APFS_MOVE_SIZE,
	    "under a shorter name"))
		goto clean;

	/* And into another directory, which is the half that moves counts. */
	if (!moved_ok(etc, "mvs", var, "mvone.txt", now, "/etc/mvs",
	    "/var/mvone.txt", file))
		goto clean;
	if (!size_still("apfs-move", file, APFS_MOVE_SIZE,
	    "in another directory"))
		goto clean;

	/*
	 * A name renamed to itself: POSIX requires success, and the file must
	 * still be there, which a writer that removed the entry first would
	 * not guarantee.
	 */
	rv = fs_apfs_rename(var, "mvone.txt", var, "mvone.txt", now, NULL);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL renaming a name to itself was "
		    "refused (%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_lookup("/var/mvone.txt", &got, &is_dir) != FS_APFS_E_OK ||
	    got != file) {
		kprintf("apfs-move: FAIL /var/mvone.txt did not survive being "
		    "renamed to itself\n");
		goto clean;
	}

	/* A directory moves with all of it, and none of it is touched. */
	rv = fs_apfs_mkdir(etc, "mvdir", now, 0755, &dir);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(dir, "inner.txt", now, 0644, &inner);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL cannot make the directory to move "
		    "(%d)\n", rv);
		goto clean;
	}
	if (!moved_ok(etc, "mvdir", etc, "mvdir2", now, "/etc/mvdir",
	    "/etc/mvdir2", dir))
		goto clean;
	if (!moved_ok(etc, "mvdir2", var, "mvdir2", now, "/etc/mvdir2",
	    "/var/mvdir2", dir))
		goto clean;
	/*
	 * The child was in neither move: its entry sorts under the directory's
	 * unchanged object id, so a subtree moves by moving one name.
	 */
	got = 0;
	if (fs_apfs_lookup("/var/mvdir2/inner.txt", &got, &is_dir) !=
	    FS_APFS_E_OK || got != inner) {
		kprintf("apfs-move: FAIL /var/mvdir2/inner.txt does not "
		    "resolve to inode %llu after its directory moved twice\n",
		    (unsigned long long)inner);
		goto clean;
	}

	/* The refusals, none of which may leave a mark. */
	rv = fs_apfs_rename(var, "mvone.txt", var, "mvdir2", now, NULL);
	if (rv != FS_APFS_E_ISDIR) {
		kprintf("apfs-move: FAIL moving a file onto a directory's "
		    "name answered %d, not ISDIR\n", rv);
		goto clean;
	}
	rv = fs_apfs_rename(var, "mvnothing.txt", var, "mvhere.txt", now, NULL);
	if (rv != FS_APFS_E_NOTFOUND) {
		kprintf("apfs-move: FAIL moving a name that is not there "
		    "answered %d, not NOTFOUND\n", rv);
		goto clean;
	}
	/*
	 * A directory into itself, directly and through a child: comparing the
	 * two object ids alone would pass the first and detach the subtree on
	 * the second.
	 */
	rv = fs_apfs_rename(var, "mvdir2", dir, "loop", now, NULL);
	if (rv != FS_APFS_E_INVAL) {
		kprintf("apfs-move: FAIL moving a directory into itself "
		    "answered %d, not INVAL\n", rv);
		goto clean;
	}
	rv = fs_apfs_mkdir(dir, "deeper", now, 0755, &deep);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL cannot make the directory below "
		    "(%d)\n", rv);
		goto clean;
	}
	rv = fs_apfs_rename(var, "mvdir2", deep, "loop", now, NULL);
	if (rv != FS_APFS_E_INVAL) {
		kprintf("apfs-move: FAIL moving a directory under its own "
		    "child answered %d, not INVAL\n", rv);
		goto clean;
	}
	/*
	 * Into something that is not a directory.  Finding the old name proves
	 * the source's parent is a directory; nothing proves the destination's,
	 * and a file would count the child in its link field.
	 */
	rv = fs_apfs_rename(var, "mvdir2", file, "loop", now, NULL);
	if (rv != FS_APFS_E_NOTDIR) {
		kprintf("apfs-move: FAIL moving into a file answered %d, not "
		    "NOTDIR\n", rv);
		goto clean;
	}
	/* Refused five times, and still exactly where it was. */
	got = 0;
	if (fs_apfs_lookup("/var/mvdir2/inner.txt", &got, &is_dir) !=
	    FS_APFS_E_OK || got != inner) {
		kprintf("apfs-move: FAIL the refusals did not leave "
		    "/var/mvdir2 as they found it\n");
		goto clean;
	}

	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-move: FAIL the index is wrong after the moves\n");
		goto clean;
	}

	kprintf("apfs-move: PASS -- %llu move(s): a file to a longer name and "
	    "a shorter one with its %u bytes intact, into another directory, "
	    "a directory with a child in it moved twice, five refusals that "
	    "left no mark%s\n", (unsigned long long)(fs_apfs_moves() - moves),
	    (unsigned)APFS_MOVE_SIZE,
	    split_n != splits ? ", and a leaf split to make room" : "");

clean:
	/*
	 * Through the directory's object id: after a failure, which name it is
	 * under cannot be assumed.
	 */
	if (dir != 0) {
		(void)fs_apfs_rmdir(dir, "deeper", now);
		(void)fs_apfs_unlink(dir, "inner.txt", now);
	}
	(void)fs_apfs_rmdir(var, "mvdir2", now);
	(void)fs_apfs_rmdir(etc, "mvdir2", now);
	(void)fs_apfs_rmdir(etc, "mvdir", now);
	(void)fs_apfs_unlink(var, "mvone.txt", now);
	(void)fs_apfs_unlink(etc, "mvone.txt", now);
	(void)fs_apfs_unlink(etc, "mvtwo.txt", now);
	(void)fs_apfs_unlink(etc, "mvconsiderablylongername.txt", now);
	(void)fs_apfs_unlink(etc, "mvs", now);
	(void)fs_apfs_checkpoint();
}

/*
 * A file with no name.
 *
 * What is checked is that the bytes outlive the name: a file is given a
 * length, its name taken away, and then asked what only a whole file can
 * answer -- its length, its object id, its records.  A writer that treated
 * an orphan as a delayed deletion would fail the first.  The path must not
 * resolve: an orphan reachable by name is not one.
 *
 * It is reaped at the end and the private directory checked empty: an orphan
 * never reaped is a leak apfsck calls valid.
 */
#define	APFS_ORPHAN_DIR		"/etc"
#define	APFS_ORPHAN_SIZE	7000u

/*
 * Is the private directory holding anything?  Asked of the tree, not the
 * child count in its inode record.
 */
static bool
priv_dir_empty(uint64_t now)
{
	uint32_t	n;

	n = 0;
	/*
	 * Asked by reaping: over an empty directory a reap-all does nothing and
	 * reports zero.  If the answer is unwanted it has also cleaned up, so
	 * it gets the real clock.
	 */
	if (fs_apfs_reap_all(now, &n) != FS_APFS_E_OK)
		return (false);
	return (n == 0);
}

void
fs_apfs_orphan_selftest(uint64_t now)
{
	uint64_t	etc;
	uint64_t	file, gone;
	uint64_t	orphans, reaps;
	uint64_t	got;
	uint32_t	left;
	int		is_dir;
	int		rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-orphan: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup(APFS_ORPHAN_DIR, &etc, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-orphan: %s is not there -- skipped\n",
		    APFS_ORPHAN_DIR);
		return;
	}

	/*
	 * Whatever an interrupted run left behind, by name and then through
	 * the private directory, the only way to reach an orphan.
	 */
	(void)fs_apfs_unlink(etc, "orphan.txt", now);
	left = 0;
	(void)fs_apfs_reap_all(now, &left);
	(void)fs_apfs_checkpoint();

	orphans = fs_apfs_orphans();
	reaps   = fs_apfs_reaps();
	file    = 0;
	gone    = 0;

	rv = fs_apfs_create(etc, "orphan.txt", now, 0644, &file);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_grow(file, file, APFS_ORPHAN_SIZE);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-orphan: FAIL cannot make the file to orphan "
		    "(%d)\n", rv);
		return;
	}
	if (!size_still("apfs-orphan", file, APFS_ORPHAN_SIZE,
	    "before its name went"))
		goto clean;

	/* The name goes. */
	rv = fs_apfs_orphan(etc, "orphan.txt", now, &gone);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-orphan: FAIL taking the name away was refused "
		    "(%d)\n", rv);
		goto clean;
	}
	if (gone != file) {
		kprintf("apfs-orphan: FAIL the name stood for inode %llu and "
		    "the orphan is %llu\n", (unsigned long long)file,
		    (unsigned long long)gone);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-orphan: FAIL the checkpoint after it was "
		    "refused\n");
		goto clean;
	}

	/* No path reaches it... */
	if (fs_apfs_lookup("/etc/orphan.txt", &got, &is_dir) !=
	    FS_APFS_E_NOTFOUND) {
		kprintf("apfs-orphan: FAIL /etc/orphan.txt still resolves "
		    "after its name was taken away\n");
		goto clean;
	}
	/* ...and the file is entirely there. */
	if (!size_still("apfs-orphan", file, APFS_ORPHAN_SIZE,
	    "with no name left"))
		goto clean;

	/*
	 * A second orphaning of the same name must fail: it is what a wrong
	 * reference count in the layer above would produce.
	 */
	rv = fs_apfs_orphan(etc, "orphan.txt", now, &got);
	if (rv != FS_APFS_E_NOTFOUND) {
		kprintf("apfs-orphan: FAIL orphaning a name that is already "
		    "gone answered %d, not NOTFOUND\n", rv);
		goto clean;
	}
	/* An inode that still has a name (here /etc) cannot be let go. */
	rv = fs_apfs_reap(etc, now);
	if (rv != FS_APFS_E_INVAL) {
		kprintf("apfs-orphan: FAIL letting go of a directory that is "
		    "still named answered %d, not INVAL\n", rv);
		goto clean;
	}

	/* Then it is let go, and the volume is back where it started. */
	rv = fs_apfs_reap(file, now);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-orphan: FAIL letting the orphan go was refused "
		    "(%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_size(file, &got) == FS_APFS_E_OK) {
		kprintf("apfs-orphan: FAIL inode %llu still has a length after "
		    "being let go\n", (unsigned long long)file);
		goto clean;
	}
	if (!priv_dir_empty(now)) {
		kprintf("apfs-orphan: FAIL the private directory is still "
		    "holding something\n");
		goto clean;
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-orphan: FAIL the index is wrong afterwards\n");
		goto clean;
	}
	file = 0;

	kprintf("apfs-orphan: PASS -- %llu file(s) kept whole with no name at "
	    "all (%u bytes each), %llu let go, %u found waiting from an "
	    "earlier run, two refusals\n",
	    (unsigned long long)(fs_apfs_orphans() - orphans),
	    (unsigned)APFS_ORPHAN_SIZE,
	    (unsigned long long)(fs_apfs_reaps() - reaps), (unsigned)left);

clean:
	/*
	 * Both sweeps: after a failure the file may still have its name or be
	 * waiting with none.
	 */
	(void)fs_apfs_unlink(etc, "orphan.txt", now);
	if (file != 0)
		(void)fs_apfs_reap(file, now);
	(void)fs_apfs_checkpoint();
}

/*
 * A name taken over: rename onto a taken name, where POSIX says a reader
 * sees the occupant or the newcomer, never an absent name.  The absence
 * cannot be watched for from here, so what the one-edit shape implies is
 * checked: the name answers with the newcomer at once, the occupant waits in
 * the private directory with its bytes intact, the reap returns what it
 * held, and a replaced empty directory is simply gone.
 *
 * The two files get different lengths, carried in the packed inode record's
 * data stream field, so "the name answers 5000, the orphan 11000" tells
 * every wrong ending apart: an entry pointing at the old file, an occupant
 * rebuilt as a fresh record, a newcomer that arrived empty.
 */
#define	APFS_CLOB_DIR		"/etc"
#define	APFS_CLOB_OTHER		"/var"
#define	APFS_CLOB_NEW_SIZE	5000u
#define	APFS_CLOB_OLD_SIZE	11000u

void
fs_apfs_clobber_selftest(uint64_t now)
{
	uint64_t	etc, var;
	uint64_t	winner, loser, second, dnew, dold, full, inner;
	uint64_t	moves, clobs, orphans, reaps;
	uint64_t	victim;
	uint64_t	got;
	uint32_t	left;
	int		is_dir;
	int		rv;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-clobber: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup(APFS_CLOB_DIR, &etc, &is_dir) != FS_APFS_E_OK ||
	    !is_dir ||
	    fs_apfs_lookup(APFS_CLOB_OTHER, &var, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-clobber: %s and %s are not both there -- "
		    "skipped\n", APFS_CLOB_DIR, APFS_CLOB_OTHER);
		return;
	}

	/*
	 * Whatever an interrupted run left behind: the names, then the private
	 * directory, where a half-finished takeover leaves what no name
	 * reaches.
	 */
	got = 0;
	if (fs_apfs_lookup("/etc/cbfull", &got, &is_dir) == FS_APFS_E_OK)
		(void)fs_apfs_unlink(got, "held.txt", now);
	(void)fs_apfs_unlink(etc, "cbnew.txt", now);
	(void)fs_apfs_unlink(etc, "cbold.txt", now);
	(void)fs_apfs_unlink(var, "cbold.txt", now);
	(void)fs_apfs_rmdir(etc, "cbfull", now);
	(void)fs_apfs_rmdir(etc, "cbdir", now);
	(void)fs_apfs_rmdir(etc, "cbdir2", now);
	left = 0;
	(void)fs_apfs_reap_all(now, &left);
	(void)fs_apfs_checkpoint();

	moves   = fs_apfs_moves();
	clobs   = fs_apfs_clobbers();
	orphans = fs_apfs_orphans();
	reaps   = fs_apfs_reaps();
	winner  = 0;
	loser   = 0;
	second  = 0;
	dnew    = 0;
	dold    = 0;
	full    = 0;
	inner   = 0;

	/* The occupant, with the larger length, and the newcomer beside it. */
	rv = fs_apfs_create(etc, "cbold.txt", now, 0644, &loser);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_grow(loser, loser, APFS_CLOB_OLD_SIZE);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(etc, "cbnew.txt", now, 0644, &winner);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_grow(winner, winner, APFS_CLOB_NEW_SIZE);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL cannot make the two files (%d)\n",
		    rv);
		goto clean;
	}

	/* The move lands on the taken name. */
	victim = 0;
	rv = fs_apfs_rename(etc, "cbnew.txt", etc, "cbold.txt", now, &victim);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL taking the name over was refused "
		    "(%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL the checkpoint after it was "
		    "refused\n");
		goto clean;
	}
	if (victim != loser) {
		kprintf("apfs-clobber: FAIL inode %llu stood at the name and "
		    "the victim reported is %llu\n",
		    (unsigned long long)loser, (unsigned long long)victim);
		goto clean;
	}
	/* The name answers with the newcomer... */
	got = 0;
	if (fs_apfs_lookup("/etc/cbold.txt", &got, &is_dir) != FS_APFS_E_OK ||
	    got != winner) {
		kprintf("apfs-clobber: FAIL /etc/cbold.txt is inode %llu "
		    "after the takeover, not %llu\n", (unsigned long long)got,
		    (unsigned long long)winner);
		goto clean;
	}
	if (fs_apfs_lookup("/etc/cbnew.txt", &got, &is_dir) !=
	    FS_APFS_E_NOTFOUND) {
		kprintf("apfs-clobber: FAIL /etc/cbnew.txt still resolves "
		    "after moving away\n");
		goto clean;
	}
	if (!size_still("apfs-clobber", winner, APFS_CLOB_NEW_SIZE,
	    "under the name it took"))
		goto clean;
	/* ...and the occupant is whole, reachable by nothing but its id. */
	if (!size_still("apfs-clobber", loser, APFS_CLOB_OLD_SIZE,
	    "waiting with no name"))
		goto clean;

	/* Let go, it takes its bytes with it. */
	rv = fs_apfs_reap(loser, now);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL letting the occupant go was "
		    "refused (%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_size(loser, &got) == FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL inode %llu still has a length "
		    "after being let go\n", (unsigned long long)loser);
		goto clean;
	}
	loser = 0;
	if (!priv_dir_empty(now)) {
		kprintf("apfs-clobber: FAIL the private directory is still "
		    "holding something\n");
		goto clean;
	}

	/*
	 * Across directories, onto an occupant with no bytes: two parents'
	 * counts move in one edit, and an orphan with an empty stream is
	 * reaped.
	 */
	rv = fs_apfs_create(var, "cbold.txt", now, 0644, &second);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL cannot make the second occupant "
		    "(%d)\n", rv);
		goto clean;
	}
	victim = 0;
	rv = fs_apfs_rename(etc, "cbold.txt", var, "cbold.txt", now, &victim);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK || victim != second) {
		kprintf("apfs-clobber: FAIL the takeover across directories "
		    "was refused (%d) or named inode %llu, not %llu\n", rv,
		    (unsigned long long)victim, (unsigned long long)second);
		goto clean;
	}
	got = 0;
	if (fs_apfs_lookup("/var/cbold.txt", &got, &is_dir) != FS_APFS_E_OK ||
	    got != winner ||
	    fs_apfs_lookup("/etc/cbold.txt", &got, &is_dir) !=
	    FS_APFS_E_NOTFOUND) {
		kprintf("apfs-clobber: FAIL the name did not follow the move "
		    "into %s\n", APFS_CLOB_OTHER);
		goto clean;
	}
	rv = fs_apfs_reap(second, now);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL letting the empty occupant go was "
		    "refused (%d)\n", rv);
		goto clean;
	}
	second = 0;

	/*
	 * A directory replaced: an empty one is two records, both removed in
	 * the edit, so nothing waits and the victim reported is zero.
	 */
	rv = fs_apfs_mkdir(etc, "cbdir", now, 0755, &dold);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_mkdir(etc, "cbdir2", now, 0755, &dnew);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL cannot make the two directories "
		    "(%d)\n", rv);
		goto clean;
	}
	victim = 1;
	rv = fs_apfs_rename(etc, "cbdir2", etc, "cbdir", now, &victim);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK || victim != 0) {
		kprintf("apfs-clobber: FAIL replacing an empty directory was "
		    "refused (%d) or left inode %llu waiting\n", rv,
		    (unsigned long long)victim);
		goto clean;
	}
	dold = 0;
	got  = 0;
	if (fs_apfs_lookup("/etc/cbdir", &got, &is_dir) != FS_APFS_E_OK ||
	    got != dnew || !is_dir ||
	    fs_apfs_lookup("/etc/cbdir2", &got, &is_dir) !=
	    FS_APFS_E_NOTFOUND) {
		kprintf("apfs-clobber: FAIL /etc/cbdir is not the directory "
		    "that moved onto it\n");
		goto clean;
	}
	/* And it is a directory the writer will file a name under. */
	rv = fs_apfs_create(dnew, "held.txt", now, 0644, &inner);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_unlink(dnew, "held.txt", now);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL the directory that took the name "
		    "does not take a name itself (%d)\n", rv);
		goto clean;
	}
	inner = 0;

	/*
	 * The refusals, none of which may leave a mark.  A directory holding a
	 * name may not be replaced (its children would lose every path), and
	 * the two ends must be the same kind, each mismatch answered as POSIX
	 * answers it.
	 */
	rv = fs_apfs_mkdir(etc, "cbfull", now, 0755, &full);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(full, "held.txt", now, 0644, &inner);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(etc, "cbnew.txt", now, 0644, &winner);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-clobber: FAIL cannot arrange the refusals "
		    "(%d)\n", rv);
		goto clean;
	}
	rv = fs_apfs_rename(etc, "cbdir", etc, "cbfull", now, NULL);
	if (rv != FS_APFS_E_NOTEMPTY) {
		kprintf("apfs-clobber: FAIL replacing a directory that holds "
		    "a name answered %d, not NOTEMPTY\n", rv);
		goto clean;
	}
	rv = fs_apfs_rename(etc, "cbnew.txt", etc, "cbfull", now, NULL);
	if (rv != FS_APFS_E_ISDIR) {
		kprintf("apfs-clobber: FAIL a file taking a directory's name "
		    "answered %d, not ISDIR\n", rv);
		goto clean;
	}
	rv = fs_apfs_rename(etc, "cbdir", etc, "cbnew.txt", now, NULL);
	if (rv != FS_APFS_E_NOTDIR) {
		kprintf("apfs-clobber: FAIL a directory taking a file's name "
		    "answered %d, not NOTDIR\n", rv);
		goto clean;
	}
	got = 0;
	if (fs_apfs_lookup("/etc/cbfull/held.txt", &got, &is_dir) !=
	    FS_APFS_E_OK || got != inner) {
		kprintf("apfs-clobber: FAIL the refusals did not leave "
		    "/etc/cbfull as they found it\n");
		goto clean;
	}

	if (fs_apfs_moves() - moves != 3 || fs_apfs_clobbers() - clobs != 3 ||
	    fs_apfs_orphans() - orphans != 2 || fs_apfs_reaps() - reaps != 2) {
		kprintf("apfs-clobber: FAIL %llu move(s), %llu takeover(s), "
		    "%llu orphaned, %llu let go -- and this test made 3, 3, "
		    "2, 2\n", (unsigned long long)(fs_apfs_moves() - moves),
		    (unsigned long long)(fs_apfs_clobbers() - clobs),
		    (unsigned long long)(fs_apfs_orphans() - orphans),
		    (unsigned long long)(fs_apfs_reaps() - reaps));
		goto clean;
	}
	if (!index_check(g_apfs.ac_root_tree_bno, 0)) {
		kprintf("apfs-clobber: FAIL the index is wrong after the "
		    "takeovers\n");
		goto clean;
	}

	kprintf("apfs-clobber: PASS -- a taken name answered with the "
	    "newcomer's %u bytes at once, twice, the occupants waited whole "
	    "(%u and none) and were let go, a directory replaced an empty "
	    "one and files a name, three refusals that left no mark\n",
	    (unsigned)APFS_CLOB_NEW_SIZE, (unsigned)APFS_CLOB_OLD_SIZE);

clean:
	/*
	 * Names, then the private directory: after a failure, which side of the
	 * takeover anything is on is unknown.
	 */
	if (full != 0)
		(void)fs_apfs_unlink(full, "held.txt", now);
	(void)fs_apfs_unlink(etc, "cbnew.txt", now);
	(void)fs_apfs_unlink(etc, "cbold.txt", now);
	(void)fs_apfs_unlink(var, "cbold.txt", now);
	(void)fs_apfs_rmdir(etc, "cbfull", now);
	(void)fs_apfs_rmdir(etc, "cbdir", now);
	(void)fs_apfs_rmdir(etc, "cbdir2", now);
	left = 0;
	(void)fs_apfs_reap_all(now, &left);
	(void)fs_apfs_checkpoint();
}

/*
 * The extent reference tree outgrows its root.
 *
 * Two files grow one block at a time in alternation, so each append lands
 * after the other file's and none can merge: every one is a fresh record.
 * Twenty-eight force the root to divide and then a leaf to split, and every
 * grow must be answered.
 *
 * The read-back is the cut: truncating both files to nothing makes the
 * writer find every record again by descent, and one a split misplaced or a
 * separator hides fails the truncate there.  apfsck checks the rest after
 * the boot: the bitmap, the counts, and the two trees against each other.
 */
#define	APFS_EXTREF_DIR		"/var/db"
#define	APFS_EXTREF_ROUNDS	14u

/*
 * Cut a file of many one-block runs down to nothing, in stages: a truncate
 * takes out at most eight runs per call (APFS_TRUNC_MAX in apfs.c).
 */
static int
extref_cut(uint64_t ino)
{
	uint64_t	size;
	uint64_t	next;
	int		rv;

	rv = fs_apfs_size(ino, &size);
	if (rv != FS_APFS_E_OK)
		return (rv);
	while (size > 0) {
		next = size > 6u * APFS_BLOCK_SIZE ?
		    size - 6u * APFS_BLOCK_SIZE : 0;
		rv = fs_apfs_truncate(ino, ino, next);
		if (rv == FS_APFS_E_OK)
			rv = fs_apfs_checkpoint();
		if (rv != FS_APFS_E_OK)
			return (rv);
		size = next;
	}
	return (FS_APFS_E_OK);
}

void
fs_apfs_extref_selftest(uint64_t now)
{
	struct btree_layout	 bl;
	uint8_t			*node;
	uint64_t		 db;
	uint64_t		 a, b;
	uint64_t		 asz, bsz;
	uint64_t		 grows, splits, drops;
	uint32_t		 round;
	int			 is_dir;
	int			 rv;
	bool			 two;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-extref: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup(APFS_EXTREF_DIR, &db, &is_dir) != FS_APFS_E_OK ||
	    !is_dir) {
		kprintf("apfs-extref: %s is not there -- skipped\n",
		    APFS_EXTREF_DIR);
		return;
	}

	/* Whatever an interrupted run left behind, cut in stages first. */
	a = 0;
	b = 0;
	if (fs_apfs_lookup("/var/db/exta.bin", &a, &is_dir) == FS_APFS_E_OK)
		(void)extref_cut(a);
	if (fs_apfs_lookup("/var/db/extb.bin", &b, &is_dir) == FS_APFS_E_OK)
		(void)extref_cut(b);
	(void)fs_apfs_unlink(db, "exta.bin", now);
	(void)fs_apfs_unlink(db, "extb.bin", now);
	(void)fs_apfs_checkpoint();

	/*
	 * The tree's shape now decides what can be demanded.  A single node
	 * must divide and split; once the index level exists (it never folds
	 * back while the volume owns a run), the claim is that all twenty-eight
	 * appends are answered through it.
	 */
	two = false;
	node = kmalloc(APFS_BLOCK_SIZE);
	if (node != NULL &&
	    fs_apfs_read_block(g_apfs.ac_extref_bno, node) == FS_APFS_E_OK) {
		btree_layout(node, &bl);
		two = bl.bl_level != 0;
	}
	kfree(node);

	grows  = fs_apfs_extref_grows();
	splits = fs_apfs_extref_splits();
	drops  = fs_apfs_extref_drops();
	a      = 0;
	b      = 0;

	rv = fs_apfs_create(db, "exta.bin", now, 0644, &a);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_create(db, "extb.bin", now, 0644, &b);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-extref: FAIL cannot make the two files (%d)\n",
		    rv);
		goto clean;
	}

	asz = 0;
	bsz = 0;
	for (round = 0; round < APFS_EXTREF_ROUNDS; round++) {
		asz += APFS_BLOCK_SIZE;
		rv = fs_apfs_grow(a, a, asz);
		if (rv == FS_APFS_E_OK) {
			bsz += APFS_BLOCK_SIZE;
			rv = fs_apfs_grow(b, b, bsz);
		}
		if (rv == FS_APFS_E_OK)
			rv = fs_apfs_checkpoint();
		if (rv != FS_APFS_E_OK) {
			kprintf("apfs-extref: FAIL append %u of %u was "
			    "refused (%d) -- the very refusal the extent "
			    "reference tree's growth exists to end\n",
			    (unsigned)round,
			    (unsigned)APFS_EXTREF_ROUNDS, rv);
			goto clean;
		}
	}

	if (!two && fs_apfs_extref_grows() == grows) {
		kprintf("apfs-extref: FAIL %u unmergeable appends into a "
		    "single-node tree and it never grew its index level\n",
		    (unsigned)(APFS_EXTREF_ROUNDS * 2));
		goto clean;
	}
	if (!two && fs_apfs_extref_splits() == splits) {
		kprintf("apfs-extref: FAIL the tree grew a level and no leaf "
		    "ever split\n");
		goto clean;
	}
	if (!size_still("apfs-extref", a, asz, "after the appends") ||
	    !size_still("apfs-extref", b, bsz, "after the appends"))
		goto clean;

	/* The cuts, which must find every record the splits scattered. */
	rv = extref_cut(a);
	if (rv == FS_APFS_E_OK)
		rv = extref_cut(b);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-extref: FAIL a cut could not find its records "
		    "again (%d)\n", rv);
		goto clean;
	}
	if (!size_still("apfs-extref", a, 0, "after the cut") ||
	    !size_still("apfs-extref", b, 0, "after the cut"))
		goto clean;

	if (two)
		kprintf("apfs-extref: PASS -- %u appends that could not "
		    "merge, all answered through the index level an earlier "
		    "run left standing (%llu leaf split(s), %llu emptied "
		    "leaf(s) left the tree)\n",
		    (unsigned)(APFS_EXTREF_ROUNDS * 2),
		    (unsigned long long)(fs_apfs_extref_splits() - splits),
		    (unsigned long long)(fs_apfs_extref_drops() - drops));
	else
		kprintf("apfs-extref: PASS -- %u appends that could not "
		    "merge, the tree grew %llu index level(s) and split "
		    "%llu leaf(s), every grow was answered, and the cuts "
		    "walked all of it back (%llu emptied leaf(s) left the "
		    "tree)\n", (unsigned)(APFS_EXTREF_ROUNDS * 2),
		    (unsigned long long)(fs_apfs_extref_grows() - grows),
		    (unsigned long long)(fs_apfs_extref_splits() - splits),
		    (unsigned long long)(fs_apfs_extref_drops() - drops));

clean:
	/*
	 * Unlink truncates to nothing in one call, so a file of many one-block
	 * runs is cut in stages first (as the sweep at the top does).
	 */
	if (a != 0)
		(void)extref_cut(a);
	if (b != 0)
		(void)extref_cut(b);
	(void)fs_apfs_unlink(db, "exta.bin", now);
	(void)fs_apfs_unlink(db, "extb.bin", now);
	(void)fs_apfs_checkpoint();
}

/*
 * A descent finds what a walk finds.
 *
 * A walk needs no key order; a descent claims this kernel's order is the
 * one the volume was written in.  apfsck cannot check that (it would agree
 * with itself whatever this file believed), so the oracle is the walk.
 *
 * Every record is sought by its own key and must come back as the same
 * record, from the same leaf, followed by the rest of the tree in the same
 * order.  The tail matters: an off-by-one in pruning lands correctly and
 * then skips a subtree, so the tail is fingerprinted record by record and
 * compared with the walk's tail from the same place.
 *
 * It runs over what Apple's tools put on the volume -- inodes, hashed
 * directory entries, data streams, extents, xattrs, sibling links --
 * including types jkey_cmp has no ordering rule for; two records it calls
 * equal but the tree keeps apart would be unreachable, and this says so.
 *
 * leaf_home, where an insert would go, is checked in the same pass against
 * the walk-based leaf_find.
 */
#define	APFS_SEEK_KEY_MAX	160u
#define	APFS_SEEK_RECS_MAX	1024u

struct seek_probe {
	uint32_t	*sp_fp;		/* fingerprint per record, or NULL */
	uint32_t	 sp_n;		/* records seen                    */
	uint32_t	 sp_max;
	uint32_t	 sp_want;	/* which one to keep a copy of     */
	uint32_t	 sp_hash;	/* order-sensitive, over them all  */
	uint8_t		 sp_key[APFS_SEEK_KEY_MAX];
	uint32_t	 sp_klen;
	uint64_t	 sp_bno;
	uint64_t	 sp_oid;
	uint32_t	 sp_type;
	bool		 sp_hit;
	bool		 sp_wide;	/* a key too long to have kept     */
};

static void
seek_probe_init(struct seek_probe *sp, uint32_t *fp, uint32_t max,
    uint32_t want)
{

	sp->sp_fp   = fp;
	sp->sp_n    = 0;
	sp->sp_max  = max;
	sp->sp_want = want;
	sp->sp_hash = 0;
	sp->sp_klen = 0;
	sp->sp_bno  = 0;
	sp->sp_oid  = 0;
	sp->sp_type = 0;
	sp->sp_hit  = false;
	sp->sp_wide = false;
}

static bool
seek_see(uint64_t oid, uint32_t type, const uint8_t *key, uint32_t klen,
    const uint8_t *val, uint32_t vlen, uint64_t bno, void *arg)
{
	struct seek_probe	*sp;
	uint32_t		 fp;
	uint32_t		 i;

	sp = arg;
	fp = crc32c(crc32c(0xFFFFFFFFu, key, klen), val, vlen);
	if (sp->sp_fp != NULL && sp->sp_n < sp->sp_max)
		sp->sp_fp[sp->sp_n] = fp;
	sp->sp_hash = sp->sp_hash * 31u + fp;
	if (sp->sp_n == sp->sp_want && !sp->sp_hit) {
		if (klen > APFS_SEEK_KEY_MAX)
			sp->sp_wide = true;
		else
			for (i = 0; i < klen; i++)
				sp->sp_key[i] = key[i];
		sp->sp_klen = klen;
		sp->sp_bno  = bno;
		sp->sp_oid  = oid;
		sp->sp_type = type;
		sp->sp_hit  = true;
	}
	sp->sp_n++;
	return (true);
}

void
fs_apfs_seek_selftest(void)
{
	struct seek_probe	 all;
	struct seek_probe	 one;
	struct seek_probe	 tail;
	struct leaf_find	 lf;
	uint32_t		*fp;
	uint64_t		 home;
	uint64_t		 gap_key[1];
	uint64_t		 was_reads;
	uint64_t		 was_nodes;
	uint64_t		 was_recs;
	uint32_t		 total;
	uint32_t		 i, j;
	uint32_t		 gaps;
	uint64_t		 prev_oid;
	uint32_t		 prev_type;
	bool			 stopped;

	if (!g_apfs.ac_mounted) {
		kprintf("apfs-seek: SKIP not mounted\n");
		return;
	}

	fp = kmalloc(APFS_SEEK_RECS_MAX * sizeof(*fp));
	if (fp == NULL) {
		kprintf("apfs-seek: SKIP no memory\n");
		return;
	}

	/*
	 * This test's own cost, for the PASS line, so the boot's read counters
	 * can be reported without it (it walks the tree three times a record).
	 */
	was_reads = g_n_walks + g_n_seeks;
	was_nodes = g_n_nodes;
	was_recs  = g_n_recs;

	/* Pass one: the whole tree, every record's fingerprint in order. */
	seek_probe_init(&all, fp, APFS_SEEK_RECS_MAX, 0xFFFFFFFFu);
	stopped = false;
	if (!btree_walk(g_apfs.ac_root_tree_bno, seek_see, &all, 0, &stopped)) {
		kprintf("apfs-seek: FAIL the tree will not walk\n");
		kfree(fp);
		return;
	}
	total = all.sp_n;
	if (total == 0 || total > APFS_SEEK_RECS_MAX) {
		kprintf("apfs-seek: FAIL the tree holds %u records, which this "
		    "test is not built for\n", (unsigned)total);
		kfree(fp);
		return;
	}

	gaps = 0;
	prev_oid  = 0;
	prev_type = 0;
	for (i = 0; i < total; i++) {
		uint32_t	expect;

		/* The i'th record, by walking to it. */
		seek_probe_init(&one, NULL, 0, i);
		stopped = false;
		if (!btree_walk(g_apfs.ac_root_tree_bno, seek_see, &one, 0,
		    &stopped) || !one.sp_hit) {
			kprintf("apfs-seek: FAIL record %u will not come out of "
			    "a walk\n", (unsigned)i);
			goto out;
		}
		if (one.sp_wide) {
			kprintf("apfs-seek: FAIL record %u has a %u-byte key, "
			    "longer than this test can hold\n", (unsigned)i,
			    (unsigned)one.sp_klen);
			goto out;
		}

		/* The same record, by descending on its key. */
		seek_probe_init(&tail, NULL, 0, 0);
		stopped = false;
		if (!btree_scan(g_apfs.ac_root_tree_bno, one.sp_key,
		    one.sp_klen, seek_see, &tail, 0, &stopped) ||
		    !tail.sp_hit) {
			kprintf("apfs-seek: FAIL the descent on record %u's own "
			    "key found nothing\n", (unsigned)i);
			goto out;
		}
		if (tail.sp_oid != one.sp_oid || tail.sp_type != one.sp_type ||
		    tail.sp_klen != one.sp_klen || tail.sp_bno != one.sp_bno) {
			kprintf("apfs-seek: FAIL record %u is object %llu type "
			    "%u in the leaf at %llu, and its own key descends "
			    "to object %llu type %u in the leaf at %llu\n",
			    (unsigned)i, (unsigned long long)one.sp_oid,
			    (unsigned)one.sp_type, (unsigned long long)one.sp_bno,
			    (unsigned long long)tail.sp_oid,
			    (unsigned)tail.sp_type,
			    (unsigned long long)tail.sp_bno);
			goto out;
		}

		/*
		 * And everything after it, hashed as the walk's tail is: a sum
		 * would pass the right records in the wrong order.
		 */
		expect = 0;
		for (j = i; j < total; j++)
			expect = expect * 31u + fp[j];
		if (tail.sp_n != total - i || tail.sp_hash != expect) {
			kprintf("apfs-seek: FAIL entered at record %u the tree "
			    "yields %u records (%08x), and the walk yields %u "
			    "from there (%08x)\n", (unsigned)i,
			    (unsigned)tail.sp_n, (unsigned)tail.sp_hash,
			    (unsigned)(total - i), (unsigned)expect);
			goto out;
		}

		/* Where an insert would put this key, both ways of asking. */
		if (leaf_home(one.sp_key, one.sp_klen, &home) != FS_APFS_E_OK) {
			kprintf("apfs-seek: FAIL no leaf claims record %u's "
			    "key\n", (unsigned)i);
			goto out;
		}
		lf.lf_key   = one.sp_key;
		lf.lf_klen  = one.sp_klen;
		lf.lf_bno   = 0;
		lf.lf_first = 0;
		lf.lf_any   = false;
		stopped = false;
		if (!btree_walk(g_apfs.ac_root_tree_bno, leaf_find, &lf, 0,
		    &stopped)) {
			kprintf("apfs-seek: FAIL the tree will not walk\n");
			goto out;
		}
		if (home != (lf.lf_bno != 0 ? lf.lf_bno : lf.lf_first) ||
		    home != one.sp_bno) {
			kprintf("apfs-seek: FAIL record %u lives in the leaf at "
			    "%llu, the descent puts its key in %llu and the "
			    "walk puts it in %llu\n", (unsigned)i,
			    (unsigned long long)one.sp_bno,
			    (unsigned long long)home,
			    (unsigned long long)(lf.lf_bno != 0 ? lf.lf_bno :
			    lf.lf_first));
			goto out;
		}

		/*
		 * A key not on the volume, where the records leave room: a type
		 * between two this object has.  The descent must land on the
		 * record after it.
		 */
		if (one.sp_oid == prev_oid && one.sp_type > prev_type + 1u) {
			gap_key[0] = (one.sp_oid & APFS_J_OBJ_ID_MASK) |
			    ((uint64_t)(prev_type + 1u) <<
			    APFS_J_OBJ_TYPE_SHIFT);
			seek_probe_init(&tail, NULL, 0, 0);
			stopped = false;
			if (!btree_scan(g_apfs.ac_root_tree_bno,
			    (const uint8_t *)gap_key, (uint32_t)sizeof(gap_key),
			    seek_see, &tail, 0, &stopped) || !tail.sp_hit ||
			    tail.sp_oid != one.sp_oid ||
			    tail.sp_type != one.sp_type ||
			    tail.sp_n != total - i) {
				kprintf("apfs-seek: FAIL object %llu has no "
				    "type-%u record, and a descent for one "
				    "should have stopped at its type-%u -- it "
				    "reached object %llu type %u\n",
				    (unsigned long long)one.sp_oid,
				    (unsigned)(prev_type + 1u),
				    (unsigned)one.sp_type,
				    (unsigned long long)tail.sp_oid,
				    (unsigned)tail.sp_type);
				goto out;
			}
			gaps++;
		}
		prev_oid  = one.sp_oid;
		prev_type = one.sp_type;
	}

	/*
	 * Below and above everything.  Object id zero names nothing, so a key
	 * under the smallest must bring back the whole tree; the largest id
	 * with type 15 (the most four bits hold) is above every record and must
	 * bring back nothing.
	 */
	gap_key[0] = 0;
	seek_probe_init(&tail, NULL, 0, 0);
	stopped = false;
	if (!btree_scan(g_apfs.ac_root_tree_bno, (const uint8_t *)gap_key,
	    (uint32_t)sizeof(gap_key), seek_see, &tail, 0, &stopped) ||
	    tail.sp_n != total || tail.sp_hash != all.sp_hash) {
		kprintf("apfs-seek: FAIL a key below every record brings back "
		    "%u of %u records\n", (unsigned)tail.sp_n, (unsigned)total);
		goto out;
	}
	gap_key[0] = APFS_J_OBJ_ID_MASK | (15ULL << APFS_J_OBJ_TYPE_SHIFT);
	seek_probe_init(&tail, NULL, 0, 0);
	stopped = false;
	if (!btree_scan(g_apfs.ac_root_tree_bno, (const uint8_t *)gap_key,
	    (uint32_t)sizeof(gap_key), seek_see, &tail, 0, &stopped) ||
	    tail.sp_n != 0) {
		kprintf("apfs-seek: FAIL a key above every record brings back "
		    "%u\n", (unsigned)tail.sp_n);
		goto out;
	}

	kprintf("apfs-seek: PASS -- each of %u records answers to its own key "
	    "out of the leaf it lives in, with the rest of the tree behind it "
	    "in order; %u key(s) that are not there land on the record after "
	    "them; and the leaf an insert would use is the one the walk names "
	    "(checking it cost %llu tree reads, %llu nodes and %llu records of "
	    "the totals below)\n", (unsigned)total, (unsigned)gaps,
	    (unsigned long long)(g_n_walks + g_n_seeks - was_reads),
	    (unsigned long long)(g_n_nodes - was_nodes),
	    (unsigned long long)(g_n_recs - was_recs));
out:
	kfree(fp);
}

/*
 * Ask the disk the four questions a checkpoint claims to have settled -- of
 * the platter, not g_apfs, since the failure worth catching is a kernel that
 * only believes it wrote a checkpoint.
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
	 * Three: the checkpoint this one replaced is untouched -- the state a
	 * crash falls back to, which nothing else would miss until then.
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
	 * Four: the map on disk names the objects we think it does, each where
	 * it says and at the new xid.  Read from the block, not ac_eph[], so a
	 * wrong map is not confirmed by the table it was written from.
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
 * Walk the spine from the committed superblock and check it arrives where
 * this kernel thinks it does.  A copy that forgets to update one link still
 * mounts, checksums and reads correctly -- out of memory -- until the next
 * boot follows the chain from block zero somewhere else.
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
 * Write two checkpoints and check the disk after each.  Two, because only
 * the second tests the ring cursors: a writer that forgets to move them
 * writes the second over the first, which still checksums and mounts.
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

/*
 * The published past reads back, exactly as far back as promised.
 *
 * apfs-ckpt proves one block, a checkpoint's superblock, survives the next
 * checkpoint.  The free queue promises every block: nothing a transaction
 * stopped using is reused for APFS_FQ_KEEP checkpoints.  This reads through
 * old checkpoints, byte for byte, to see the promise kept.
 *
 * A file takes three states across three consecutive checkpoints -- absent,
 * one block of one byte, two blocks of another -- and is absent again in a
 * fourth; each view must show its own checkpoint's state, so a view that
 * resolved the wrong root answers from the wrong column.  The mount state is
 * copied before the views and compared after: a view that moved the live
 * root would pass every read and corrupt the next write.
 *
 * The window's edge is asked for too: the checkpoint just below the floor
 * must be refused GONE, and the listing must name exactly those inside.
 * Expectations are written against APFS_FQ_KEEP, not the number 2.
 */
#define	APFS_VIEW_NAME	"view.txt"
#define	APFS_VIEW_PATH	"/etc/view.txt"

/* 1 = the file is `blocks` blocks of `fill`, 0 = it is not, -1 = no file */
static int
view_shaped(uint32_t blocks, uint8_t fill)
{
	uint8_t		*buf;
	uint32_t	 got;
	uint32_t	 i;
	int		 ok;

	if (fs_apfs_slurp(APFS_VIEW_PATH, &buf, &got) != FS_APFS_E_OK)
		return (-1);
	ok = (got == blocks * APFS_BLOCK_SIZE);
	for (i = 0; ok && i < got; i++)
		ok = (buf[i] == fill);
	kfree(buf);
	return (ok);
}

/*
 * What the file looks like through a view of `xid`, against what it should:
 * `blocks` of `fill`, or absent when blocks is 0.  `want_rv` is what opening
 * the view itself must answer -- OK inside the window, GONE below it,
 * NOTFOUND above it -- and only an OK view is entered and read.
 */
static bool
view_expect(uint64_t xid, int want_rv, uint32_t blocks, uint8_t fill)
{
	struct fs_apfs_view	v;
	uint64_t		oid;
	int			is_dir;
	int			rv;
	int			shape;

	rv = fs_apfs_view_open(xid, &v);
	if (rv != want_rv) {
		kprintf("apfs-view: FAIL opening a view of xid %llu answered "
		    "%d, wanted %d\n", (unsigned long long)xid, rv, want_rv);
		return (false);
	}
	if (want_rv != FS_APFS_E_OK)
		return (true);
	rv = fs_apfs_view_enter(&v);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL entering the view of xid %llu "
		    "answered %d\n", (unsigned long long)xid, rv);
		return (false);
	}
	if (blocks == 0) {
		rv = fs_apfs_lookup(APFS_VIEW_PATH, &oid, &is_dir);
		fs_apfs_view_leave();
		if (rv != FS_APFS_E_NOTFOUND) {
			kprintf("apfs-view: FAIL through xid %llu the name "
			    "answers %d, and it did not exist then\n",
			    (unsigned long long)xid, rv);
			return (false);
		}
		return (true);
	}
	shape = view_shaped(blocks, fill);
	fs_apfs_view_leave();
	if (shape != 1) {
		kprintf("apfs-view: FAIL through xid %llu the file is not %u "
		    "block(s) of '%c' (%s)\n", (unsigned long long)xid,
		    (unsigned)blocks, (char)fill,
		    shape < 0 ? "it does not read" : "the bytes differ");
		return (false);
	}
	return (true);
}

static bool
view_fill_block(uint64_t ino, uint64_t size, uint64_t off, uint8_t fill,
    uint8_t *blk)
{
	uint32_t	put;
	uint32_t	i;

	for (i = 0; i < APFS_BLOCK_SIZE; i++)
		blk[i] = fill;
	if (fs_apfs_pwrite(ino, size, off, blk, APFS_BLOCK_SIZE, &put) !=
	    FS_APFS_E_OK || put != APFS_BLOCK_SIZE)
		return (false);
	return (true);
}

void
fs_apfs_view_selftest(uint64_t now)
{
	uint64_t		 xids[APFS_FQ_KEEP + 2];
	struct apfs_mount	*before;
	uint8_t			*blk;
	const uint8_t		*a;
	const uint8_t		*b;
	uint64_t		 etc;
	uint64_t		 ino;
	uint64_t		 oid;
	uint64_t		 x0, x1, x2, x3;
	uint64_t		 floor;
	uint32_t		 n;
	uint32_t		 i;
	int			 is_dir;
	int			 rv;
	bool			 ok;

	if (!g_apfs.ac_mounted || !g_apfs.ac_ip_valid) {
		kprintf("apfs-view: nothing writable -- skipped\n");
		return;
	}
	if (fs_apfs_lookup("/etc", &etc, &is_dir) != FS_APFS_E_OK || !is_dir) {
		kprintf("apfs-view: /etc is not there -- skipped\n");
		return;
	}
	before = kmalloc(sizeof(*before));
	blk    = kmalloc(APFS_BLOCK_SIZE);
	if (before == NULL || blk == NULL) {
		kprintf("apfs-view: no memory -- skipped\n");
		kfree(before);
		kfree(blk);
		return;
	}
	ino = 0;

	/* Whatever an interrupted run left, then a checkpoint to stand on. */
	(void)fs_apfs_unlink(etc, APFS_VIEW_NAME, now);
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL the opening checkpoint was refused\n");
		goto clean;
	}
	x0 = g_apfs.ac_xid;

	/* x1: the file, one block of 'A'. */
	rv = fs_apfs_create(etc, APFS_VIEW_NAME, now, 0644, &ino);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_grow(ino, ino, APFS_BLOCK_SIZE);
	if (rv == FS_APFS_E_OK &&
	    !view_fill_block(ino, APFS_BLOCK_SIZE, 0, 'A', blk))
		rv = FS_APFS_E_IO;
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL cannot make the file's first state "
		    "(%d)\n", rv);
		goto clean;
	}
	x1 = g_apfs.ac_xid;

	/* x2: the first block rewritten as 'B' and a second block of it. */
	rv = FS_APFS_E_OK;
	if (!view_fill_block(ino, APFS_BLOCK_SIZE, 0, 'B', blk))
		rv = FS_APFS_E_IO;
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_grow(ino, ino, 2 * APFS_BLOCK_SIZE);
	if (rv == FS_APFS_E_OK &&
	    !view_fill_block(ino, 2 * APFS_BLOCK_SIZE, APFS_BLOCK_SIZE, 'B',
	    blk))
		rv = FS_APFS_E_IO;
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL cannot make the file's second state "
		    "(%d)\n", rv);
		goto clean;
	}
	x2 = g_apfs.ac_xid;

	/*
	 * The name goes and the transaction stays open, so the live volume and
	 * the newest checkpoint disagree -- the usual state under the sync
	 * policy, and what a view of the newest checkpoint is for.
	 */
	rv = fs_apfs_unlink(etc, APFS_VIEW_NAME, now);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL the unlink was refused (%d)\n", rv);
		goto clean;
	}
	if (fs_apfs_lookup(APFS_VIEW_PATH, &oid, &is_dir) !=
	    FS_APFS_E_NOTFOUND) {
		kprintf("apfs-view: FAIL the live volume still names the "
		    "unlinked file\n");
		goto clean;
	}

	/* The past, with the mount photographed before and compared after. */
	*before = g_apfs;
	ok = view_expect(x2, FS_APFS_E_OK, 2, 'B') &&
	    view_expect(x1, FS_APFS_E_OK, 1, 'A') &&
	    view_expect(x0, FS_APFS_E_OK, 0, 0) &&
	    view_expect(x2 + 1, FS_APFS_E_NOTFOUND, 0, 0);
	if (!ok)
		goto clean;
	a = (const uint8_t *)before;
	b = (const uint8_t *)&g_apfs;
	for (i = 0; i < sizeof(g_apfs); i++) {
		if (a[i] != b[i]) {
			kprintf("apfs-view: FAIL reading the past moved the "
			    "mount (byte %u of the mount state changed)\n",
			    (unsigned)i);
			goto clean;
		}
	}
	if (fs_apfs_view_xid() != 0) {
		kprintf("apfs-view: FAIL a view of xid %llu was left "
		    "entered\n", (unsigned long long)fs_apfs_view_xid());
		goto clean;
	}

	/*
	 * Publishing the unlink slides the window over this test's three
	 * checkpoints; which survive is APFS_FQ_KEEP's decision.
	 */
	if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL the publishing checkpoint was "
		    "refused\n");
		goto clean;
	}
	x3    = g_apfs.ac_xid;
	floor = x3 - APFS_FQ_KEEP;
	if (view_floor() != floor) {
		kprintf("apfs-view: FAIL the floor stands at %llu after "
		    "checkpoint %llu; APFS_FQ_KEEP of %u puts it at %llu\n",
		    (unsigned long long)view_floor(), (unsigned long long)x3,
		    (unsigned)APFS_FQ_KEEP, (unsigned long long)floor);
		goto clean;
	}
	ok = view_expect(x0, x0 < floor ? FS_APFS_E_GONE : FS_APFS_E_OK, 0,
	    0) &&
	    view_expect(x1, x1 < floor ? FS_APFS_E_GONE : FS_APFS_E_OK, 1,
	    'A') &&
	    view_expect(x2, x2 < floor ? FS_APFS_E_GONE : FS_APFS_E_OK, 2,
	    'B') &&
	    view_expect(x3, FS_APFS_E_OK, 0, 0);
	if (!ok)
		goto clean;

	/* And the listing names the window exactly: floor to x3, in order. */
	rv = fs_apfs_view_list(xids, (uint32_t)(sizeof(xids) /
	    sizeof(xids[0])), &n);
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-view: FAIL listing the views answered %d\n", rv);
		goto clean;
	}
	if (n != APFS_FQ_KEEP + 1) {
		kprintf("apfs-view: FAIL %u checkpoint(s) listed, the window "
		    "is %u wide\n", (unsigned)n, (unsigned)(APFS_FQ_KEEP + 1));
		goto clean;
	}
	for (i = 0; i < n; i++) {
		if (xids[i] != floor + i) {
			kprintf("apfs-view: FAIL entry %u of the listing is "
			    "xid %llu, wanted %llu\n", (unsigned)i,
			    (unsigned long long)xids[i],
			    (unsigned long long)(floor + i));
			goto clean;
		}
	}

	kprintf("apfs-view: PASS -- through the ring the file read as absent "
	    "at xid %llu, 1 block of 'A' at %llu and 2 of 'B' at %llu while "
	    "the live volume had unlinked it; after xid %llu the window is "
	    "%llu..%llu, %llu refused as let go, the mount untouched\n",
	    (unsigned long long)x0, (unsigned long long)x1,
	    (unsigned long long)x2, (unsigned long long)x3,
	    (unsigned long long)floor, (unsigned long long)x3,
	    (unsigned long long)(floor - 1));

clean:
	if (fs_apfs_view_xid() != 0)
		fs_apfs_view_leave();
	(void)fs_apfs_unlink(etc, APFS_VIEW_NAME, now);
	if (g_apfs.ac_dirty)
		(void)fs_apfs_checkpoint();
	kfree(before);
	kfree(blk);
}
