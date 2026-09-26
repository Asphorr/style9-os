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
#include "fs_txn.h"
#include "kmem.h"
#include "kprintf.h"

/*
 * See fs_txn.h for what this guarantees and what it does not.
 *
 * The block primitives are APFS's (fs/apfs/apfs.c), the only writable
 * format; only the orchestration -- collect, coalesce, all-or-nothing
 * before the flush -- is format-independent.
 *
 * No lock: callers hold the volume lock (fs.c), and a transaction is a
 * local of one operation on one thread.
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

static int
txn_get(struct fs_txn *t, uint64_t bno, void **buf_out, bool raw)
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
	 * Already here?  The same buffer, or a second change to one leaf
	 * would read a fresh copy and its write would undo the first.
	 */
	for (i = 0; i < t->tx_n; i++) {
		if (t->tx_slot[i].ts_bno != bno)
			continue;
		/* Raw and checked at once: one caller is wrong about it. */
		if (t->tx_slot[i].ts_raw != raw) {
			t->tx_failed = true;
			kprintf("fs_txn: block %llu fetched both raw and "
			    "checked\n", (unsigned long long)bno);
			return (FS_TXN_E_IO);
		}
		*buf_out = t->tx_slot[i].ts_buf;
		return (FS_TXN_E_OK);
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
	rv = raw ? fs_apfs_read_block_raw(bno, s->ts_buf) :
	    fs_apfs_read_block(bno, s->ts_buf);
	if (rv != FS_APFS_E_OK) {
		kfree(s->ts_buf);
		s->ts_buf    = NULL;
		t->tx_failed = true;
		return (FS_TXN_E_IO);
	}
	s->ts_bno   = bno;
	s->ts_dirty = false;
	s->ts_raw   = raw;
	t->tx_n++;

	*buf_out = s->ts_buf;
	return (FS_TXN_E_OK);
}

int
fs_txn_get(struct fs_txn *t, uint64_t bno, void **buf_out)
{

	return (txn_get(t, bno, buf_out, false));
}

int
fs_txn_get_raw(struct fs_txn *t, uint64_t bno, void **buf_out)
{

	return (txn_get(t, bno, buf_out, true));
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
	 * A block never fetched: the caller's change went somewhere else.
	 * Poison rather than ignore.
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
		if ((t->tx_slot[i].ts_raw ?
		    fs_apfs_write_block_raw(t->tx_slot[i].ts_bno,
		    t->tx_slot[i].ts_buf) :
		    fs_apfs_write_block(t->tx_slot[i].ts_bno,
		    t->tx_slot[i].ts_buf)) != FS_APFS_E_OK) {
			/*
			 * The volume is already partly updated and in-place
			 * writes cannot be undone.  Report and keep going:
			 * stopping would be no more consistent.
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
