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
 * A set of metadata blocks changed together, written together.
 *
 * WHAT PROBLEM.  One filesystem operation touches several blocks.  Growing a
 * file by a block changes the allocation bitmap, the space manager's free
 * counts, the B-tree leaf that gains an extent record, and the inode's
 * recorded length -- four blocks, and a volume where two of them landed is a
 * volume that lies about itself.  Today's writer already has a small version
 * of this: fs_apfs_pwrite puts bytes down and then stamps the inode, and a
 * failure between them leaves contents that changed and a timestamp that did
 * not.
 *
 * WHAT THIS GUARANTEES, EXACTLY.  Nothing reaches the disk until every change
 * has been computed successfully.  Any failure while building -- out of
 * memory, a block that fails its own checksum, a record that will not fit --
 * aborts having written nothing at all.
 *
 * WHAT IT DOES NOT GUARANTEE.  Once the flush begins it is not atomic: the
 * drive takes the blocks one at a time and a power cut in the middle leaves
 * some written.  This is not a journal and does not claim to be one.  It
 * narrows the window from "the whole operation" to "the flush", which is the
 * honest improvement available without copy-on-write -- and it is the shape
 * the real fix takes later, because a checkpoint writer is this same set of
 * blocks published by one final superblock write instead of applied in place.
 *
 * METADATA ONLY, and that is a design statement rather than a limit.  In APFS
 * only metadata blocks carry an obj_phys header and therefore a checksum;
 * file data is unchecked bytes.  Metadata is also bounded -- a handful of
 * blocks per operation -- while a write's data can be megabytes, so data is
 * streamed straight through and never buffered here.  Real filesystems draw
 * the line in the same place and for the same reasons.
 *
 * Blocks are coalesced by number: asking twice for the same block yields the
 * same buffer, so two records changed in one B-tree leaf produce one read and
 * one write rather than a lost update.
 */

/*
 * How many distinct metadata blocks one operation may touch.  Sized for the
 * largest operation planned -- a file growing by one block: allocation
 * bitmap, chunk info, space manager, the extent leaf, the inode leaf, and
 * headroom for a B-tree split touching a node and its parent.  Exceeding it
 * is a bug in the caller rather than a condition to handle gracefully, so it
 * fails the operation loudly instead of growing.
 */
#define	FS_TXN_MAX_BLOCKS	12

struct fs_txn_slot {
	uint64_t	 ts_bno;
	uint8_t		*ts_buf;	/* one block, kmalloc'd on first touch */
	bool		 ts_dirty;
	/*
	 * This block carries no obj_phys, so it is neither verified on the way
	 * in nor sealed on the way out.  See fs_txn_get_raw.
	 */
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
 * Hand back block `bno` for modification, reading and CHECKSUM-VERIFYING it
 * on first touch.  Verifying matters: writing over a block that already fails
 * its own checksum would turn someone else's corruption into ours, and hand
 * it back freshly sealed so nothing downstream could tell.
 *
 * The buffer belongs to the transaction and stays valid until commit or
 * abort.  Returns FS_TXN_E_OK and stores the buffer in *buf_out, or a
 * negative FS_TXN_E_* -- after which the transaction is poisoned and will
 * refuse to commit, so a caller that ignores one error cannot write a
 * half-built change.
 */
int	fs_txn_get(struct fs_txn *t, uint64_t bno, void **buf_out);

/*
 * The same, for a block that has no obj_phys header: read it without checking
 * a checksum it does not have, and write it back without sealing one over its
 * contents.
 *
 * There is exactly one such block in an operation of this kind, and it is the
 * reason the list at the top of this file names it first: an APFS allocation
 * bitmap is bits and nothing else.  Its first eight bytes are the allocation
 * state of sixty-four blocks, in the place where a metadata block keeps its
 * Fletcher-64 -- so the ordinary path rejects every bitmap in the container on
 * the way in, and would overwrite sixty-four blocks' worth of state on the way
 * out.  The distinction is the one write_block_raw already draws for file
 * data; this is the same line, drawn for a block that is neither data nor
 * checksummed metadata.
 *
 * A block may be fetched raw or checked, never both: asking for it the other
 * way after the first fetch poisons the transaction rather than quietly
 * handing back a buffer that will be written under the wrong rules.
 */
int	fs_txn_get_raw(struct fs_txn *t, uint64_t bno, void **buf_out);

/* Mark a block obtained above as changed.  Untouched blocks are not written. */
void	fs_txn_dirty(struct fs_txn *t, uint64_t bno);

/*
 * Seal every dirty block with its Fletcher-64 and write them all, then
 * release the transaction.  Returns FS_TXN_E_OK, or a negative FS_TXN_E_* --
 * including when an earlier fs_txn_get failed, in which case nothing is
 * written.  The transaction is finished either way; do not reuse it.
 */
int	fs_txn_commit(struct fs_txn *t);

/* Throw the whole thing away, writing nothing.  Idempotent. */
void	fs_txn_abort(struct fs_txn *t);

/* Blocks written and transactions committed, for the boot banner. */
void	fs_txn_stats(void);

#endif /* !_SYS_FS_TXN_H_ */
