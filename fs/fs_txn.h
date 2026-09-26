/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_FS_TXN_H_
#define	_SYS_FS_TXN_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * A set of metadata blocks changed together, written together, in place.
 *
 * Guarantee: nothing reaches the disk until every change has been computed.
 * Any failure while building (no memory, a block failing its checksum, a
 * record that will not fit) aborts having written nothing.
 *
 * Not guaranteed: the flush itself is not atomic.  The drive takes the
 * blocks one at a time, and a power cut mid-flush leaves some written.
 * This is not a journal; it only narrows the window to the flush.
 *
 * Metadata only: in APFS only metadata blocks carry an obj_phys header and
 * a checksum, and metadata is a handful of blocks per operation, while
 * file data can be megabytes and is streamed straight through.
 *
 * Blocks are coalesced by number: asking twice for a block yields the same
 * buffer, so two records changed in one B-tree leaf make one read and one
 * write rather than a lost update.
 *
 * Nothing calls this at present: APFS allocation and checkpoint writes are
 * copy-on-write (fs/apfs/apfs.c).
 */

/*
 * Distinct metadata blocks one operation may touch: enough for a file
 * growing by a block (bitmap, chunk info, space manager, extent and inode
 * leaves) plus a B-tree split.  Exceeding it is a caller bug and fails the
 * operation.
 */
#define	FS_TXN_MAX_BLOCKS	12

struct fs_txn_slot {
	uint64_t	 ts_bno;
	uint8_t		*ts_buf;	/* one block, kmalloc'd on first touch */
	bool		 ts_dirty;
	/* No obj_phys: neither verified nor sealed (fs_txn_get_raw). */
	bool		 ts_raw;
};

struct fs_txn {
	struct fs_txn_slot	tx_slot[FS_TXN_MAX_BLOCKS];
	unsigned		tx_n;
	bool			tx_failed;	/* sticky: poisons the commit */
};

#define	FS_TXN_E_OK		0
#define	FS_TXN_E_IO		(-1)	/* a block would not read or write */
#define	FS_TXN_E_NOMEM		(-2)
#define	FS_TXN_E_FULL		(-3)	/* more blocks than FS_TXN_MAX_BLOCKS */

/* Start an empty transaction.  Cannot fail; allocates nothing yet. */
void	fs_txn_begin(struct fs_txn *t);

/*
 * Hand back block `bno' for modification, reading and checksum-verifying
 * it on first touch, so a block already corrupt is not resealed as if
 * sound.  The buffer belongs to the transaction until commit or abort.
 * Returns FS_TXN_E_OK with the buffer in *buf_out, or a negative
 * FS_TXN_E_*, after which the transaction is poisoned and will not commit.
 */
int	fs_txn_get(struct fs_txn *t, uint64_t bno, void **buf_out);

/*
 * The same for a block with no obj_phys header -- an APFS allocation
 * bitmap, whose first eight bytes are allocation bits where a metadata
 * block keeps its Fletcher-64.  Read without a checksum check, written
 * without sealing.  A block may be fetched raw or checked, never both;
 * the other way after the first poisons the transaction.
 */
int	fs_txn_get_raw(struct fs_txn *t, uint64_t bno, void **buf_out);

/* Mark a block obtained above as changed.  Untouched blocks are not written. */
void	fs_txn_dirty(struct fs_txn *t, uint64_t bno);

/*
 * Seal every dirty checked block with its Fletcher-64, write all dirty
 * blocks, and release the transaction.  Returns FS_TXN_E_OK, or a
 * negative FS_TXN_E_* (nothing written if the transaction was poisoned).
 * The transaction is finished either way.
 */
int	fs_txn_commit(struct fs_txn *t);

/* Throw the whole thing away, writing nothing.  Idempotent. */
void	fs_txn_abort(struct fs_txn *t);

/* Commits, blocks written and aborts; prints nothing if all are zero. */
void	fs_txn_stats(void);

#endif /* !_SYS_FS_TXN_H_ */
