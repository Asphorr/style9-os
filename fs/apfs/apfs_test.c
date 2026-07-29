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
 * WHAT THIS KERNEL CAN PROVE ABOUT ITS OWN WRITES
 *
 * Every claim the APFS writer makes is checked at boot, and this is where the
 * checking lives.  It is a fifth of everything that was in apfs.c and none of
 * it runs unless a test asks, which is the whole reason it is its own file:
 * reading the writer meant scrolling past the proofs of the rung before.
 *
 * These are not unit tests and they are not decoration.  Most of them exist
 * because something WAS wrong and passed everything else -- a split whose
 * separator came from the wrong half read back perfectly and left a tree
 * apfsck called out of order; a node emptied and left in place did the same;
 * a counter left decremented after a refused write was invisible to every
 * oracle there is.  So the shape they share is: arrange the case rather than
 * wait for it, then ask the DISK rather than the kernel's idea of the disk.
 *
 * apfsck from apfsprogs is the outside oracle for everything about the
 * FORMAT.  What it cannot check is what this kernel believes -- it reads a
 * volume with its own idea of key order and would agree with itself whatever
 * this code thought -- which is why apfs-seek exists and why its oracle is
 * the whole-tree walk that was right before the descent replaced it.
 *
 * The internals these reach for are declared in apfs_priv.h, which exists for
 * this file and nothing else.
 */

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
	/*
	 * The descent starts at the record's own key, so anything under a
	 * larger object id is past the answer and the walk can stop there.
	 */
	if (oid != sp->sp_id)
		return (false);
	if (type == APFS_TYPE_DSTREAM_ID)
		sp->sp_found = true;
	return (true);
}

/*
 * AN INODE AND ITS DATA STREAM, IN TWO DIFFERENT NODES
 *
 * A file this kernel makes gets two records under its own object id, one after
 * the other: the inode, and the reference count of the stream its bytes hang
 * off.  They are written into the same node because nothing can sort between
 * them, and unlinking looked for the second where the first was on exactly that
 * reasoning.
 *
 * It is the reasoning, not the arithmetic, that was wrong.  Adjacent in key
 * order means the same node only until a SPLIT falls between them -- and once
 * both records exist, a cut at the middle of a leaf full of inodes lands
 * between a file and its own stream as readily as between two files.  The
 * unlink then took the inode and the entry and left the stream record behind,
 * and said so in a line that reads as a normal case:
 *
 *	apfs: inode 332 has no dstream id record -- one fewer to take out
 *
 * which is what a file that came off the image genuinely looks like.  What the
 * volume was left with is what apfsck calls
 *
 *	Data stream: has no references.
 *
 * FOUND ON THE FOURTH BOOT, by the test below this one, which is another way of
 * saying it would have been found by a user.  So it is arranged here instead:
 * make a file, split the leaf holding its records AT the stream record so that
 * the inode ends the lower half and the stream starts the upper, and unlink it.
 * The claim is checked where the bug was invisible -- not that the unlink
 * answered success, which it always did, but that no record of that stream is
 * left anywhere in the tree afterwards.
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
	 * The stream record is the one after it, and the cut goes there.  A cut
	 * at zero is refused and a record that is already last has nothing
	 * after it to cut before, so both are honest reasons to leave this
	 * boot alone rather than to fail.
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
	 * And the arrangement is CHECKED before it is used.  A split that put
	 * them back in the same node would leave this test passing on the very
	 * writer it exists to catch.
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
	 * The whole claim: not that the unlink said yes -- it always did -- but
	 * that nothing of the stream is left.  Asked of the tree by descending
	 * on the record's own key, which is where it would be if it were there.
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
 * A CREATE MAKES ITS OWN ROOM
 *
 * Every writer here could once refuse for a reason that had nothing to do with
 * what it was asked: the leaf a record belonged in was full, and a full leaf
 * was somebody else's rung.  Growing a file stopped answering that way when it
 * learned to split and start over.  Making a name is the same answer to a
 * harder question, because a name's records do not all go in one place -- the
 * entry sorts under the DIRECTORY's object id and the inode under its OWN -- so
 * there are two leaves that can be the full one and splitting the first can be
 * answered by the second.
 *
 * WAITING FOR A FULL LEAF IS NOT A TEST.  Whether one turns up depends on how
 * many names an image happens to carry and on where earlier splits fell; the
 * make test skipped on it for a rung and a half, which is exactly as much
 * evidence as never having run.  So this fills one, honestly: names go into a
 * directory one at a time, each with its own checkpoint, the way an ordinary
 * caller makes them -- and around eighteen inode records fill a leaf of this
 * volume, so the bound below is generous rather than tight.
 *
 * WHAT IS CHECKED is what a split can get wrong and a create cannot notice.
 * Every name made before the split has to still be there afterwards under the
 * inode it was given: a record carried into the wrong half reads back as a
 * missing file, and the create that caused it has already reported success.
 * And every node has to still start where its parent says it does, which is the
 * invariant apfsck states as "index key absent from child node" and which no
 * amount of reading the volume from in here would notice.
 *
 * WHAT IT DOES NOT ARRANGE, and the honest limit of it: the leaf that fills
 * here is the one holding INODES, because an inode record is nine times the
 * size of an entry and the names all sort together under one directory.  A
 * create that finds BOTH of its leaves full in the same breath is handled by
 * construction -- the writer asks again after each split rather than once --
 * and is not reached by any arrangement here.
 *
 * They are all taken away again, so the volume ends the boot as it began.  The
 * nodes the splits made stay, and that is not untidiness: this tree does not
 * give a level back, and a node with room in it is what the next boot fills.
 */
/*
 * APFS_ROOM_LEAF is where the name starts in the path; APFS_ROOM_NAMES is a
 * bound and not an expectation (a leaf takes about eighteen inodes); and
 * APFS_ROOM_AFTER is how many creates have to work once one has split.
 */
#define	APFS_ROOM_DIR		"/etc"
#define	APFS_ROOM_LEAF		5
#define	APFS_ROOM_NAMES		32
#define	APFS_ROOM_AFTER		2

/*
 * "/etc/roomNN.txt", written out a byte at a time rather than formatted: there
 * is no snprintf in here, and the name is wanted both whole, for a lookup by
 * path, and from the slash, for the calls that take a parent and a leaf.
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
		 * Stop a couple of names PAST the first split rather than at
		 * it: a writer that split and then refused the very record it
		 * had made room for would otherwise leave with a clean sheet.
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
 * A NAME THAT MOVES
 *
 * A rename creates nothing and destroys nothing, which is what makes it hard
 * to check from outside: the counts do not move, the record total does not
 * move, and a writer that did half the job leaves a volume with exactly as
 * many things in it as a writer that did all of it.  So this asks the two
 * questions the volume can answer -- does the new name resolve to the inode
 * the old one named, and has the old name stopped resolving -- after every
 * move, and one more that only a file can answer.
 *
 * THE BYTES ARE THE POINT OF THE LONG NAMES.  A file's length and the blocks
 * under it live in an extended field of the inode record, beside the field
 * holding its name; the record is packed, so a name of a different length puts
 * everything after it somewhere else.  A rename that rebuilt that record the
 * way a create writes one -- which is the obvious way to write it -- would
 * produce a perfectly valid empty file.  So a file is given a length here,
 * moved to a longer name and then to a shorter one, and asked its length after
 * each: three sizes that must all be the one it was given.
 *
 * WHAT IS NOT CHECKED IN HERE is the two directories' child counts, and that
 * is deliberate rather than forgotten.  The count is a claim about records
 * that a reader would have to re-derive to disprove, which is precisely what
 * apfsck does with it -- "Inode record: wrong directory child count" is the
 * measured answer to leaving either half out -- and the acceptance run checks
 * every boot with it.  What this test adds is the half apfsck cannot see: the
 * refusals, which leave no trace on the volume at all.
 *
 * A RENAME THAT MEETS A FULL LEAF takes the same loop a create does, and it is
 * arranged for the create by apfs-room rather than again here.  Whether one
 * turns up during these moves depends on how full the leaves already are, so
 * it is reported when it happens and not required.
 */
#define	APFS_MOVE_DIR		"/etc"
#define	APFS_MOVE_OTHER		"/var"
#define	APFS_MOVE_SIZE		9000u

/*
 * One move, and the three things that must be true after it.  Every step below
 * is one of these, which is what keeps the test itself from being longer than
 * the writer.
 */
static bool
moved_ok(uint64_t odir, const char *oname, uint64_t ndir, const char *nname,
    uint64_t now, const char *opath, const char *npath, uint64_t want)
{
	uint64_t	got;
	int		is_dir;
	int		rv;

	rv = fs_apfs_rename(odir, oname, ndir, nname, now);
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

/* The length the volume says a file has, or a complaint and false. */
static bool
size_still(uint64_t ino, uint64_t want, const char *where)
{
	uint64_t	got;

	got = 0;
	if (fs_apfs_size(ino, &got) != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL inode %llu has no length at all %s\n",
		    (unsigned long long)ino, where);
		return (false);
	}
	if (got != want) {
		kprintf("apfs-move: FAIL inode %llu is %llu bytes %s and was "
		    "%llu -- the data stream did not come across\n",
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
	 * by path under each name it can be sitting at, and emptied through its
	 * OBJECT ID -- which is the one thing about it that no move changed.
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
	/*
	 * And bytes under it, because an empty file cannot tell a rename that
	 * carried its data stream across from one that wrote a fresh record.
	 */
	rv = fs_apfs_grow(file, file, APFS_MOVE_SIZE);
	if (rv == FS_APFS_E_OK)
		rv = fs_apfs_checkpoint();
	if (rv != FS_APFS_E_OK) {
		kprintf("apfs-move: FAIL cannot give the file a length (%d)\n",
		    rv);
		goto clean;
	}
	if (!size_still(file, APFS_MOVE_SIZE, "before any move"))
		goto clean;

	/* Same directory, same length of name: the plainest move there is. */
	if (!moved_ok(etc, "mvone.txt", etc, "mvtwo.txt", now,
	    "/etc/mvone.txt", "/etc/mvtwo.txt", file))
		goto clean;

	/* Longer, so the record grows; then shorter, so it shrinks. */
	if (!moved_ok(etc, "mvtwo.txt", etc, "mvconsiderablylongername.txt",
	    now, "/etc/mvtwo.txt", "/etc/mvconsiderablylongername.txt", file))
		goto clean;
	if (!size_still(file, APFS_MOVE_SIZE, "under a longer name"))
		goto clean;
	if (!moved_ok(etc, "mvconsiderablylongername.txt", etc, "mvs", now,
	    "/etc/mvconsiderablylongername.txt", "/etc/mvs", file))
		goto clean;
	if (!size_still(file, APFS_MOVE_SIZE, "under a shorter name"))
		goto clean;

	/* And into another directory, which is the half that moves counts. */
	if (!moved_ok(etc, "mvs", var, "mvone.txt", now, "/etc/mvs",
	    "/var/mvone.txt", file))
		goto clean;
	if (!size_still(file, APFS_MOVE_SIZE, "in another directory"))
		goto clean;

	/*
	 * A name renamed to ITSELF changes nothing and says so.  POSIX requires
	 * the success; what is checked here is that the file is still there
	 * afterwards, since a writer that took the entry out before putting it
	 * back would answer the same and leave nothing behind.
	 */
	rv = fs_apfs_rename(var, "mvone.txt", var, "mvone.txt", now);
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

	/* A DIRECTORY MOVES WITH ALL OF IT, and none of it is touched. */
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
	 * The child was never mentioned in either move.  Its entry sorts under
	 * the directory's object id, which did not change, and its own record
	 * names that same id as its parent -- so a subtree moves by moving one
	 * name, and this is what says so.
	 */
	got = 0;
	if (fs_apfs_lookup("/var/mvdir2/inner.txt", &got, &is_dir) !=
	    FS_APFS_E_OK || got != inner) {
		kprintf("apfs-move: FAIL /var/mvdir2/inner.txt does not "
		    "resolve to inode %llu after its directory moved twice\n",
		    (unsigned long long)inner);
		goto clean;
	}

	/* THE REFUSALS, none of which may leave a mark. */
	rv = fs_apfs_rename(var, "mvone.txt", var, "mvdir2", now);
	if (rv != FS_APFS_E_EXIST) {
		kprintf("apfs-move: FAIL moving onto a name that is taken "
		    "answered %d, not EXIST\n", rv);
		goto clean;
	}
	rv = fs_apfs_rename(var, "mvnothing.txt", var, "mvhere.txt", now);
	if (rv != FS_APFS_E_NOTFOUND) {
		kprintf("apfs-move: FAIL moving a name that is not there "
		    "answered %d, not NOTFOUND\n", rv);
		goto clean;
	}
	/*
	 * And a directory into itself, asked twice: once directly, and once
	 * through a child, because a check that only compared the two object
	 * ids would pass the first and detach the volume on the second.
	 */
	rv = fs_apfs_rename(var, "mvdir2", dir, "loop", now);
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
	rv = fs_apfs_rename(var, "mvdir2", deep, "loop", now);
	if (rv != FS_APFS_E_INVAL) {
		kprintf("apfs-move: FAIL moving a directory under its own "
		    "child answered %d, not INVAL\n", rv);
		goto clean;
	}
	/*
	 * And into something that is not a directory at all.  Finding the old
	 * name says the SOURCE's parent holds entries; nothing says that of the
	 * destination, and a file would have taken a child in the field it
	 * counts links in.
	 */
	rv = fs_apfs_rename(var, "mvdir2", file, "loop", now);
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
	 * Emptied through the directory's object id for the reason above: this
	 * runs after a failure too, and a failure is exactly the case where
	 * which name the directory is under is the thing not to be assumed.
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
 * A DESCENT FINDS WHAT A WALK FINDS
 *
 * Reading the whole tree needs no key ordering; descending on one is a claim
 * that this kernel's idea of the order is the order the volume was actually
 * written in.  apfsck cannot check that -- it reads the volume with its own
 * ordering and would agree with itself whatever this file believed -- so the
 * proof has to be here, and the only oracle worth having is the walk that was
 * right before.
 *
 * Every record on the volume is sought BY ITS OWN KEY, and three things are
 * demanded of the answer: the same record, out of the same leaf block, and
 * then the whole of the rest of the tree behind it in the same order.  That
 * last one is the part worth paying for.  A descent that lands correctly and
 * then skips a subtree on the way right would pass a test that only looked at
 * the first record, and skipping a subtree is exactly what an off-by-one in
 * the pruning does; so the tail is fingerprinted record by record, order and
 * contents both, and compared against the walk's own tail from the same place.
 *
 * It runs over what Apple's tools put on this volume, not over what this
 * kernel writes: inodes, directory entries with hashed keys, data streams,
 * extents, extended attributes, sibling links.  Types this file has no
 * ordering rule for are in there too, and they are the ones that would break
 * it -- jkey_cmp returns EQUAL for two of them, and two records that compare
 * equal but are kept apart by the tree are a record the descent cannot reach.
 * If that is ever true of this volume, this test says so.
 *
 * The insertion answer is checked against its own oracle in the same pass:
 * leaf_home descends to the leaf a key belongs in, the walk-based leaf_find it
 * replaces says the same thing a slower way, and the writer only changed over
 * once the two had agreed about every key here.
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
	 * What this costs, kept so the PASS line can say it.  The counters this
	 * kernel prints at the end of a boot are about reading the FILESYSTEM,
	 * and a test that reads the whole tree three times per record would
	 * otherwise be most of what they measured.
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
		 * And everything behind it.  The walk's tail from the same
		 * record, combined the same way -- a sum would pass on a
		 * descent that returned the right records in the wrong order.
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
		 * A key that is NOT on the volume, when the records leave room
		 * for one: a type between two this object really has.  The
		 * descent must land on the record after it, which is the whole
		 * of what "where it would go" means.
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
	 * Below everything and above everything.  An object id of zero names
	 * nothing -- the root directory is 2 -- so a key under the smallest one
	 * has to bring the whole tree back, and one over the largest nothing at
	 * all.  Fifteen is the largest type the four bits above an object id
	 * can hold, which is what makes the second key unreachable rather than
	 * merely unlikely.
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
