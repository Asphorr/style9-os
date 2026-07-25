/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "fs_apfs.h"
#include "fs_txn.h"
#include "kmem.h"
#include "kprintf.h"

/*
 * See fs_txn.h for what this guarantees and what it deliberately does not.
 *
 * The block primitives come from fs_apfs.c because APFS is the only backend
 * here that can be written; a second writable format would want its own read
 * and seal, and this would take them as arguments rather than by name.  The
 * orchestration -- collect, coalesce, all-or-nothing before the flush -- is
 * what is format-independent, and it is all that lives here.
 *
 * No lock.  Every caller reaches this while holding the volume lock in fs.c,
 * and a transaction is a local variable belonging to one operation on one
 * thread; there is nothing here for a second thread to find.
 */

static uint64_t	txn_n_commit;		/* transactions that reached the disk */
static uint64_t	txn_n_blocks;		/* metadata blocks written            */
static uint64_t	txn_n_abort;		/* built and thrown away              */

void
fs_txn_begin(struct fs_txn *t)
{
	unsigned	i;

	for (i = 0; i < FS_TXN_MAX_BLOCKS; i++) {
		t->tx_slot[i].ts_bno   = 0;
		t->tx_slot[i].ts_buf   = NULL;
		t->tx_slot[i].ts_dirty = false;
	}
	t->tx_n      = 0;
	t->tx_failed = false;
}

static void
release(struct fs_txn *t)
{
	unsigned	i;

	for (i = 0; i < t->tx_n; i++) {
		if (t->tx_slot[i].ts_buf != NULL)
			kfree(t->tx_slot[i].ts_buf);
		t->tx_slot[i].ts_buf = NULL;
	}
	t->tx_n = 0;
}

int
fs_txn_get(struct fs_txn *t, uint64_t bno, void **buf_out)
{
	struct fs_txn_slot	*s;
	unsigned		 i;
	int			 rv;

	if (buf_out == NULL)
		return (FS_TXN_E_IO);
	*buf_out = NULL;
	if (t->tx_failed)
		return (FS_TXN_E_IO);

	/*
	 * Already here?  Hand back the same buffer.  Without this, two changes
	 * to one B-tree leaf would each read a fresh copy and the second write
	 * would silently undo the first -- a lost update that leaves a block
	 * with a perfectly correct checksum over the wrong contents.
	 */
	for (i = 0; i < t->tx_n; i++) {
		if (t->tx_slot[i].ts_bno == bno) {
			*buf_out = t->tx_slot[i].ts_buf;
			return (FS_TXN_E_OK);
		}
	}

	if (t->tx_n >= FS_TXN_MAX_BLOCKS) {
		t->tx_failed = true;
		kprintf("fs_txn: more than %u blocks in one operation\n",
		    (unsigned)FS_TXN_MAX_BLOCKS);
		return (FS_TXN_E_FULL);
	}

	s = &t->tx_slot[t->tx_n];
	s->ts_buf = kmalloc(APFS_BLOCK_SIZE);
	if (s->ts_buf == NULL) {
		t->tx_failed = true;
		return (FS_TXN_E_NOMEM);
	}
	rv = fs_apfs_read_block(bno, s->ts_buf);
	if (rv != FS_APFS_E_OK) {
		kfree(s->ts_buf);
		s->ts_buf    = NULL;
		t->tx_failed = true;
		return (FS_TXN_E_IO);
	}
	s->ts_bno   = bno;
	s->ts_dirty = false;
	t->tx_n++;

	*buf_out = s->ts_buf;
	return (FS_TXN_E_OK);
}

void
fs_txn_dirty(struct fs_txn *t, uint64_t bno)
{
	unsigned	i;

	for (i = 0; i < t->tx_n; i++) {
		if (t->tx_slot[i].ts_bno == bno) {
			t->tx_slot[i].ts_dirty = true;
			return;
		}
	}
	/*
	 * Marking a block the transaction never fetched means the caller is
	 * confused about which block it changed, and the change it thinks it
	 * made is somewhere else.  Poison rather than ignore.
	 */
	t->tx_failed = true;
	kprintf("fs_txn: dirty on block %llu, which was never fetched\n",
	    (unsigned long long)bno);
}

int
fs_txn_commit(struct fs_txn *t)
{
	unsigned	i;
	int		rv;

	if (t->tx_failed) {
		release(t);
		txn_n_abort++;
		return (FS_TXN_E_IO);
	}

	rv = FS_TXN_E_OK;
	for (i = 0; i < t->tx_n; i++) {
		if (!t->tx_slot[i].ts_dirty)
			continue;
		if (fs_apfs_write_block(t->tx_slot[i].ts_bno,
		    t->tx_slot[i].ts_buf) != FS_APFS_E_OK) {
			/*
			 * Past this point the volume is already partly
			 * updated, and there is no undo: the older version of
			 * an in-place block is gone.  Report it and keep
			 * going, because stopping here would leave FEWER
			 * blocks written and no more consistency than
			 * finishing does.  Rung three is where this stops
			 * being the best available answer.
			 */
			kprintf("fs_txn: write of block %llu failed mid-commit\n",
			    (unsigned long long)t->tx_slot[i].ts_bno);
			rv = FS_TXN_E_IO;
			continue;
		}
		txn_n_blocks++;
	}
	release(t);
	if (rv == FS_TXN_E_OK)
		txn_n_commit++;
	return (rv);
}

void
fs_txn_abort(struct fs_txn *t)
{

	if (t->tx_n != 0)
		txn_n_abort++;
	release(t);
	t->tx_failed = false;
}

void
fs_txn_stats(void)
{

	if (txn_n_commit == 0 && txn_n_abort == 0)
		return;
	kprintf("fs_txn: %llu commits (%llu metadata blocks), %llu aborted\n",
	    (unsigned long long)txn_n_commit,
	    (unsigned long long)txn_n_blocks,
	    (unsigned long long)txn_n_abort);
}
