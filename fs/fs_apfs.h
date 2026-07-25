/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_FS_APFS_H_
#define	_SYS_FS_APFS_H_

#include <stdint.h>

/*
 * Read-only APFS -- the filesystem a Darwin personality ought to be reading.
 *
 * Everything else on this rung is already Apple-shaped (Mach-O images, a
 * clean-room dyld, libSystem, launchd, Mach IPC), and FAT was only ever the
 * placeholder that let a binary find a data file at all.  APFS is the real
 * thing, and unlike the rest of the Apple surface here it does not have to be
 * reverse engineered: Apple published the on-disk format.
 *
 * Three ideas carry the whole format, and the code below is organised around
 * them:
 *
 *	1. EVERY metadata block starts with an obj_phys header carrying a
 *	   Fletcher-64 checksum of the rest of the block, plus the object's id
 *	   (oid) and the transaction that wrote it (xid).  Nothing is trusted
 *	   without checking that checksum -- a half-written block is exactly
 *	   what a copy-on-write filesystem expects to find after a crash.
 *
 *	2. There is no single fixed superblock.  Block 0 is only an anchor;
 *	   the live container superblock is whichever copy in the CHECKPOINT
 *	   DESCRIPTOR ring carries the highest xid and still checksums.  That
 *	   ring is how APFS commits atomically: write the new state elsewhere,
 *	   then land one new superblock.  Mounting means finding the newest
 *	   valid one, which is what makes an interrupted write a no-op rather
 *	   than corruption.
 *
 *	3. Objects are addressed indirectly.  A PHYSICAL oid is a block
 *	   number, but a VIRTUAL oid has to be translated through an object
 *	   map (omap) B-tree first, because copy-on-write moves objects
 *	   without changing their identity.
 *
 * Read-only, and that is the whole of it: no allocator, no checkpoint writer,
 * no space manager.  Mounting a copy-on-write filesystem for reading is a much
 * smaller claim than writing one, and it is the claim a Darwin personality
 * needs -- a binary opens its data files, it does not reformat the disk.
 */

/* 'NXSB' as it appears little-endian at offset 32 of the container block. */
#define	APFS_NX_MAGIC		0x4253584EU

/* Only block size we accept; the one every real container uses. */
#define	APFS_BLOCK_SIZE		4096

#define	APFS_NX_MAX_FILE_SYSTEMS	100

/*
 * obj_phys.o_type packs a type in the low 16 bits with storage-class and
 * flag bits above it.
 */
#define	APFS_OBJ_TYPE_MASK	0x0000FFFFU
#define	APFS_OBJ_STORAGE_MASK	0xC0000000U
#define	APFS_OBJ_VIRTUAL	0x00000000U
#define	APFS_OBJ_EPHEMERAL	0x80000000U
#define	APFS_OBJ_PHYSICAL	0x40000000U

#define	APFS_OBJ_NX_SUPERBLOCK	0x00000001U
#define	APFS_OBJ_BTREE_ROOT	0x00000002U
#define	APFS_OBJ_BTREE_NODE	0x00000003U
#define	APFS_OBJ_OMAP		0x0000000BU
#define	APFS_OBJ_FS		0x0000000DU	/* volume superblock */

/*
 * Header every metadata block begins with.  o_cksum covers the block from
 * o_oid onward, so it is deliberately NOT part of what it protects.
 */
struct apfs_obj_phys {
	uint64_t	o_cksum;
	uint64_t	o_oid;
	uint64_t	o_xid;
	uint32_t	o_type;
	uint32_t	o_subtype;
};

/*
 * Container superblock.  Field offsets are dictated by the published format;
 * the kernel is amd64-only and the format is little-endian, so the struct is
 * read straight out of the block with no byte swapping.  The static asserts
 * below pin the offsets that were verified against a real container.
 */
struct apfs_nx_superblock {
	struct apfs_obj_phys	nx_o;
	uint32_t		nx_magic;
	uint32_t		nx_block_size;
	uint64_t		nx_block_count;
	uint64_t		nx_features;
	uint64_t		nx_readonly_compat;
	uint64_t		nx_incompat;
	uint8_t			nx_uuid[16];
	uint64_t		nx_next_oid;
	uint64_t		nx_next_xid;
	uint32_t		nx_xp_desc_blocks;
	uint32_t		nx_xp_data_blocks;
	uint64_t		nx_xp_desc_base;
	uint64_t		nx_xp_data_base;
	uint32_t		nx_xp_desc_next;
	uint32_t		nx_xp_data_next;
	uint32_t		nx_xp_desc_index;
	uint32_t		nx_xp_desc_len;
	uint32_t		nx_xp_data_index;
	uint32_t		nx_xp_data_len;
	uint64_t		nx_spaceman_oid;
	uint64_t		nx_omap_oid;
	uint64_t		nx_reaper_oid;
	uint32_t		nx_test_type;
	uint32_t		nx_max_file_systems;
	uint64_t		nx_fs_oid[APFS_NX_MAX_FILE_SYSTEMS];
};

_Static_assert(sizeof(struct apfs_obj_phys) == 32,
    "obj_phys is a 32-byte on-disk header");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_magic) == 32,
    "nx_magic sits at +32 -- verified against a real container");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_xp_desc_base)
    == 112, "nx_xp_desc_base sits at +112");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_omap_oid)
    == 160, "nx_omap_oid sits at +160");
_Static_assert(__builtin_offsetof(struct apfs_nx_superblock, nx_fs_oid)
    == 184, "nx_fs_oid[] starts at +184");

/*
 * Object map.  A PHYSICAL oid is already a block number, but a VIRTUAL one
 * is not: copy-on-write moves an object without changing its identity, so
 * the mapping oid -> block lives in this B-tree and is looked up by
 * (oid, xid).  Keying on the transaction too is what lets several versions
 * of the same object coexist -- that is how snapshots work.
 */
struct apfs_omap_phys {
	struct apfs_obj_phys	om_o;
	uint32_t		om_flags;
	uint32_t		om_snap_count;
	uint32_t		om_tree_type;
	uint32_t		om_snapshot_tree_type;
	uint64_t		om_tree_oid;
	uint64_t		om_snapshot_tree_oid;
	uint64_t		om_most_recent_snap;
	uint64_t		om_pending_revert_min;
	uint64_t		om_pending_revert_max;
};

struct apfs_omap_key {
	uint64_t	ok_oid;
	uint64_t	ok_xid;
};

struct apfs_omap_val {
	uint32_t	ov_flags;
	uint32_t	ov_size;
	uint64_t	ov_paddr;
};

/*
 * B-tree node.  One shape serves every tree in the format; only the key and
 * value types differ.  The body after this 56-byte header is laid out as:
 *
 *	[ table of contents ]	at btn_table_space.off, one entry per key
 *	[ keys ]		growing UP from the end of the table space
 *	[ free space ]
 *	[ values ]		growing DOWN from the end of the NODE
 *	[ btree_info, 40 B ]	present only in a root node
 *
 * So a value offset is subtracted from the end of the node -- minus the
 * trailing info struct when the node is also the root.  Key and value
 * offsets are 16-bit and relative, which is why a node cannot exceed 64 KiB.
 */
#define	APFS_BTNODE_ROOT		0x0001
#define	APFS_BTNODE_LEAF		0x0002
#define	APFS_BTNODE_FIXED_KV_SIZE	0x0004

#define	APFS_BTNODE_HDR_SIZE		56
#define	APFS_BTREE_INFO_SIZE		40

struct apfs_nloc {
	uint16_t	nl_off;
	uint16_t	nl_len;
};

struct apfs_btree_node_phys {
	struct apfs_obj_phys	btn_o;
	uint16_t		btn_flags;
	uint16_t		btn_level;
	uint32_t		btn_nkeys;
	struct apfs_nloc	btn_table_space;
	struct apfs_nloc	btn_free_space;
	struct apfs_nloc	btn_key_free_list;
	struct apfs_nloc	btn_val_free_list;
};

/* Fixed-size-KV table entry; the variable-size form is a pair of nlocs. */
struct apfs_kvoff {
	uint16_t	k;
	uint16_t	v;
};

struct apfs_kvloc {
	struct apfs_nloc	k;
	struct apfs_nloc	v;
};

/* 'APSB' little-endian at offset 32 of a volume superblock. */
#define	APFS_APSB_MAGIC		0x42535041U
#define	APFS_VOLNAME_LEN	256

struct apfs_wrapped_meta_crypto_state {
	uint16_t	wmcs_major_version;
	uint16_t	wmcs_minor_version;
	uint32_t	wmcs_cpflags;
	uint32_t	wmcs_persistent_class;
	uint32_t	wmcs_key_os_version;
	uint16_t	wmcs_key_revision;
	uint16_t	wmcs_unused;
};

struct apfs_modified_by {
	uint8_t		am_id[32];
	uint64_t	am_timestamp;
	uint64_t	am_last_xid;
};

/*
 * Volume superblock.  A container holds up to nx_max_file_systems of these,
 * each an independent filesystem sharing the container's free space -- the
 * "space sharing" that lets macOS ship System and Data as separate volumes
 * without partitioning.  We mount volume 0.
 */
struct apfs_superblock {
	struct apfs_obj_phys	apfs_o;
	uint32_t		apfs_magic;
	uint32_t		apfs_fs_index;
	uint64_t		apfs_features;
	uint64_t		apfs_readonly_compat;
	uint64_t		apfs_incompat;
	uint64_t		apfs_unmount_time;
	uint64_t		apfs_fs_reserve_block_count;
	uint64_t		apfs_fs_quota_block_count;
	uint64_t		apfs_fs_alloc_count;
	struct apfs_wrapped_meta_crypto_state	apfs_meta_crypto;
	uint32_t		apfs_root_tree_type;
	uint32_t		apfs_extentref_tree_type;
	uint32_t		apfs_snap_meta_tree_type;
	uint64_t		apfs_omap_oid;
	uint64_t		apfs_root_tree_oid;
	uint64_t		apfs_extentref_tree_oid;
	uint64_t		apfs_snap_meta_tree_oid;
	uint64_t		apfs_revert_to_xid;
	uint64_t		apfs_revert_to_sblock_oid;
	uint64_t		apfs_next_obj_id;
	uint64_t		apfs_num_files;
	uint64_t		apfs_num_directories;
	uint64_t		apfs_num_symlinks;
	uint64_t		apfs_num_other_fsobjects;
	uint64_t		apfs_num_snapshots;
	uint64_t		apfs_total_blocks_alloced;
	uint64_t		apfs_total_blocks_freed;
	uint8_t			apfs_vol_uuid[16];
	uint64_t		apfs_last_mod_time;
	uint64_t		apfs_fs_flags;
	struct apfs_modified_by	apfs_formatted_by;
	struct apfs_modified_by	apfs_modified_by[8];
	uint8_t			apfs_volname[APFS_VOLNAME_LEN];
};

_Static_assert(sizeof(struct apfs_omap_key) == 16, "omap key is 16 bytes");
_Static_assert(sizeof(struct apfs_omap_val) == 16, "omap value is 16 bytes");
_Static_assert(__builtin_offsetof(struct apfs_omap_phys, om_tree_oid) == 48,
    "om_tree_oid sits at +48");
_Static_assert(sizeof(struct apfs_btree_node_phys) == APFS_BTNODE_HDR_SIZE,
    "a B-tree node header is 56 bytes");
_Static_assert(sizeof(struct apfs_modified_by) == 48,
    "apfs_modified_by is 48 bytes");
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_omap_oid) == 128,
    "apfs_omap_oid sits at +128");
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_root_tree_oid)
    == 136, "apfs_root_tree_oid sits at +136");
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_num_files)
    == 184, "apfs_num_files sits at +184");
/*
 * The offset that proves the whole struct: on a container formatted with
 * -L style9, reading a string here yields exactly "style9".
 */
_Static_assert(__builtin_offsetof(struct apfs_superblock, apfs_volname) == 704,
    "apfs_volname sits at +704");

/*
 * File-system records.  A volume keeps inodes, directory entries, extents
 * and extended attributes in ONE B-tree, told apart by the record type
 * packed into the top 4 bits of the key's first word -- the low 60 bits are
 * the object id.  Records sort by object id FIRST and type second, which is
 * the opposite of what a raw 64-bit compare of that word would do; any
 * search has to unpack before comparing.
 */
#define	APFS_J_OBJ_ID_MASK	0x0FFFFFFFFFFFFFFFULL
#define	APFS_J_OBJ_TYPE_SHIFT	60

#define	APFS_TYPE_INODE		3
#define	APFS_TYPE_XATTR		4
#define	APFS_TYPE_DSTREAM_ID	6
#define	APFS_TYPE_FILE_EXTENT	8
#define	APFS_TYPE_DIR_REC	9

/* The root directory's object id is fixed by the format. */
#define	APFS_ROOT_DIR_INO	2

/*
 * Directory-entry keys come in two shapes and the volume's incompatible
 * feature flags pick which.  A case- or normalization-insensitive volume
 * stores a 22-bit hash of the name alongside its length, and orders entries
 * within a directory BY THAT HASH; a plain volume stores just the length and
 * orders by name.  Computing the hash means reproducing Apple's Unicode
 * normalization, so this reader does not: it descends on the object id --
 * the primary sort key, and hash-independent -- and compares names directly
 * once there.  Costlier for a huge directory, exact for any of them.
 */
#define	APFS_INCOMPAT_CASE_INSENSITIVE		0x00000001ULL
#define	APFS_INCOMPAT_NORM_INSENSITIVE		0x00000008ULL
#define	APFS_DREC_LEN_MASK			0x000003FFU

/* j_drec_val_t.flags low bits: the dirent type, BSD DT_* numbering. */
#define	APFS_DT_DIR		4
#define	APFS_DT_REG		8

/*
 * Packed, and it matters: the on-disk record is 18 bytes, but the natural
 * alignment of a struct ending in a uint16 after two uint64s would round
 * sizeof up to 24 -- and a length check against that silently rejects every
 * real directory entry.
 */
struct apfs_drec_val {
	uint64_t	dv_file_id;
	uint64_t	dv_date_added;
	uint16_t	dv_flags;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_drec_val) == 18,
    "a directory-entry record is 18 bytes before its extended fields");

/*
 * Inode record.  Note what is NOT here: the file's size.  That lives in a
 * dstream extended field appended after this struct, because a plain
 * directory has no need of one -- so the fixed part stops at 92 bytes and
 * the extended fields follow.  The struct is packed: uncompressed_size sits
 * at offset 84, which is not 8-byte aligned.
 */
struct apfs_inode_val {
	uint64_t	ai_parent_id;
	uint64_t	ai_private_id;
	uint64_t	ai_create_time;
	uint64_t	ai_mod_time;
	uint64_t	ai_change_time;
	uint64_t	ai_access_time;
	uint64_t	ai_internal_flags;
	int32_t		ai_nchildren_or_nlink;
	uint32_t	ai_default_protection_class;
	uint32_t	ai_write_generation_counter;
	uint32_t	ai_bsd_flags;
	uint32_t	ai_owner;
	uint32_t	ai_group;
	uint16_t	ai_mode;
	uint16_t	ai_pad1;
	uint64_t	ai_uncompressed_size;
} __attribute__((packed));

/*
 * ai_mode is an ordinary POSIX mode_t written straight to disk, so its type
 * field is spelled the way POSIX froze it.  The reader needs exactly one of
 * these bits: ai_nchildren_or_nlink means CHILDREN on a directory and LINKS
 * on anything else, and the mode is what tells the two apart.
 */
#define	APFS_S_IFMT	0170000
#define	APFS_S_IFDIR	0040000

_Static_assert(sizeof(struct apfs_inode_val) == 92,
    "the fixed part of an inode record is 92 bytes, extended fields follow");
_Static_assert(__builtin_offsetof(struct apfs_inode_val, ai_mode) == 80,
    "ai_mode sits at +80");

/*
 * Extended fields.  A record that needs more than its fixed part appends a
 * small blob of them: a count, then that many descriptors, then the data they
 * describe, each datum padded up to a multiple of 8.  This is how APFS keeps a
 * directory's inode from carrying a file's worth of empty fields -- and it is
 * why a file's LENGTH is not where one would look for it.  Length lives in the
 * DSTREAM field, so an inode without one names something with no bytes.
 */
#define	APFS_INO_EXT_TYPE_NAME		4
#define	APFS_INO_EXT_TYPE_DSTREAM	8

struct apfs_xf_blob {
	uint16_t	xb_num_exts;
	uint16_t	xb_used_data;
};

struct apfs_x_field {
	uint8_t		xf_type;
	uint8_t		xf_flags;
	uint16_t	xf_size;
};

struct apfs_dstream {
	uint64_t	ds_size;		/* the file's real length */
	uint64_t	ds_alloced_size;
	uint64_t	ds_default_crypto_id;
	uint64_t	ds_total_bytes_written;
	uint64_t	ds_total_bytes_read;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_xf_blob) == 4, "xf_blob is 4 bytes");
_Static_assert(sizeof(struct apfs_x_field) == 4, "an x_field is 4 bytes");
_Static_assert(sizeof(struct apfs_dstream) == 40, "a dstream is 40 bytes");

/*
 * File extent.  The key is the record header followed by the byte offset
 * within the file; the value gives that run's length and the block it starts
 * at.  Two things a reader has to respect: the run is ALLOCATED length and may
 * overshoot the file's real size (the tail of the last block is garbage), and
 * a physical block of zero means a HOLE -- a sparse region that was never
 * written and reads back as zeroes rather than as anything on disk.
 *
 * Packed for the same reason the records above are: these sit at whatever
 * offset the node's value area put them, which is not 8-byte aligned.
 */
#define	APFS_FILE_EXTENT_LEN_MASK	0x00FFFFFFFFFFFFFFULL

struct apfs_file_extent_val {
	uint64_t	fe_len_and_flags;
	uint64_t	fe_phys_block_num;
	uint64_t	fe_crypto_id;
} __attribute__((packed));

_Static_assert(sizeof(struct apfs_file_extent_val) == 24,
    "a file-extent record is 24 bytes");

/* Longest name fs_apfs_readdir reports. */
#define	FS_APFS_NAME_MAX	255

/* Largest file the slurp path will read (bounds the kmalloc). */
#define	FS_APFS_MAX_FILE	(4u * 1024u * 1024u)

/*
 * One directory entry as fs_apfs_readdir reports it.  Deliberately the same
 * shape fs_fat_dirent has, so the Darwin readdir path can be pointed at
 * either filesystem without changing its wire format.
 */
struct fs_apfs_dirent {
	uint64_t	ade_ino;
	uint64_t	ade_size;
	uint8_t		ade_is_dir;
	char		ade_name[FS_APFS_NAME_MAX + 1];
};

/*
 * A file's metadata, as fs_apfs_stat reports it.  Sizes are 64-bit because
 * APFS's are; the Darwin syscall layer narrows them where its own wire format
 * is 32-bit, which is the right place for that decision to be visible.
 *
 * Everything below afs_mode comes out of the inode record's FIXED part, which
 * the tree walk was already reading and this struct was already throwing away
 * -- the timestamps are APFS's own nanoseconds since the Unix epoch, and the
 * link count is nchildren for a directory (Apple stores both in one field and
 * tells them apart by the mode).  afs_alloced is the dstream's allocated size,
 * which is what st_blocks means: what the volume spent, not what the file
 * says it is.
 */
struct fs_apfs_statbuf {
	uint64_t	afs_size;	/* byte length (0 for a directory) */
	uint64_t	afs_ino;	/* object id                       */
	uint64_t	afs_alloced;	/* bytes on disk (dstream)         */
	uint64_t	afs_mtime_ns;
	uint64_t	afs_atime_ns;
	uint64_t	afs_ctime_ns;
	uint64_t	afs_btime_ns;
	uint32_t	afs_nlink;
	uint32_t	afs_uid;
	uint32_t	afs_gid;
	uint16_t	afs_mode;	/* BSD mode bits from the inode    */
	uint8_t		afs_is_dir;
};

#define	FS_APFS_E_OK		0
#define	FS_APFS_E_NOMOUNT	(-1)	/* no APFS container mounted    */
#define	FS_APFS_E_IO		(-2)	/* block read failed            */
#define	FS_APFS_E_NOMEM		(-3)	/* kmalloc failed               */
#define	FS_APFS_E_INVAL		(-4)	/* not APFS / unsupported shape */
#define	FS_APFS_E_CKSUM		(-5)	/* Fletcher-64 mismatch         */
#define	FS_APFS_E_NOTFOUND	(-6)	/* name absent / not a dir      */
#define	FS_APFS_E_TOOBIG	(-7)	/* file exceeds FS_APFS_MAX_FILE */
#define	FS_APFS_E_NOALLOC	(-8)	/* would need a block allocator  */

/*
 * Probe the first ATA drive for an APFS container and adopt the newest valid
 * checkpoint superblock.  Called once at boot, after ata_drv_init and
 * kmem_init.  Logs the container geometry on success and a one-line reason on
 * failure; a failed probe simply leaves APFS unavailable.
 */
void	fs_apfs_init(void);

/* Non-zero once a container is mounted. */
int	fs_apfs_ready(void);

/*
 * What the reader's whole-tree walk has cost so far: walks started, B-tree
 * nodes read, leaf records handed to a callback.  The walk is O(tree) per
 * question by construction, so these say whether that is a real cost on this
 * volume or a theoretical one.
 */
void	fs_apfs_stats(void);

/*
 * Translate a virtual object id to its block number through the object-map
 * B-tree rooted at `tree_bno`, taking the newest version no later than `xid`.
 * Returns FS_APFS_E_OK and stores the block in *paddr_out, or a negative
 * FS_APFS_E_*.  Both the container omap (which finds volumes) and each
 * volume's own omap (which finds its trees) are read with this.
 */
int	fs_apfs_omap_lookup(uint64_t tree_bno, uint64_t oid, uint64_t xid,
	    uint64_t *paddr_out);

/*
 * Resolve an absolute path to its object id, reporting whether it names a
 * directory.  A leading '/' is optional and repeated separators are ignored;
 * "" and "/" both name the root.  Returns FS_APFS_E_OK or a negative
 * FS_APFS_E_* (NOTFOUND for a missing component, or for descending through
 * something that is not a directory).
 */
int	fs_apfs_lookup(const char *path, uint64_t *oid_out, int *is_dir_out);

/*
 * Fill *out with the `index`-th entry of the directory named by `path`.
 * Returns 1 when an entry was written, 0 at end-of-directory, or a negative
 * FS_APFS_E_*.  Enumeration is stateless -- each call re-resolves and
 * re-scans -- matching how fs_fat_readdir behaves and keeping no per-fd
 * cursor in the kernel.
 */
int	fs_apfs_readdir(const char *path, uint32_t index,
	    struct fs_apfs_dirent *out);

/*
 * Report a file-or-directory's metadata without reading its bytes.  Returns
 * FS_APFS_E_OK and fills *out, or a negative FS_APFS_E_*.
 */
int	fs_apfs_stat(const char *path, struct fs_apfs_statbuf *out);

/*
 * Read the whole file named by `path` into a freshly kmalloc'd buffer.  On
 * success returns FS_APFS_E_OK, stores the buffer in *out_buf (the caller
 * kfree's it) and its byte length in *out_size.  Holes read back as zeroes.
 * Returns FS_APFS_E_TOOBIG rather than attempting an unbounded allocation.
 */
int	fs_apfs_slurp(const char *path, uint8_t **out_buf, uint32_t *out_size);

/*
 * Resolve `path` to the three things reading and writing it need: the dstream
 * id its extents are keyed on, its byte length, and its inode object id.  The
 * expensive half of reading, paid once instead of per call.  Directories are
 * refused.  Returns FS_APFS_E_OK or a negative FS_APFS_E_*.
 *
 * The dstream id and the inode id are equal on a freshly written volume and
 * diverge as soon as anything is hard-linked, so both are reported: one finds
 * the bytes, the other finds the record that describes them.
 */
int	fs_apfs_open(const char *path, uint64_t *id_out, uint64_t *size_out,
	    uint64_t *ino_out);

/*
 * Read at most `len` bytes of the resolved file (`id`, `size`) starting at
 * file offset `off` into the caller's buffer, reporting the count delivered
 * through *out_got.  Reads that begin at or past end-of-file return
 * FS_APFS_E_OK with zero bytes; reads that run off the end come back short.
 * Holes read back as zeroes.
 */
int	fs_apfs_pread(uint64_t id, uint64_t size, uint64_t off, uint8_t *buf,
	    uint32_t len, uint32_t *out_got);

/*
 * Read APFS block `bno` into `buf` (which must hold a whole block) and verify
 * its Fletcher-64.  Returns FS_APFS_E_OK, or a negative FS_APFS_E_*.  Exposed
 * because every layer above -- omap, B-trees, volume superblocks -- reads
 * blocks exactly this way.
 */
int	fs_apfs_read_block(uint64_t bno, void *buf);

/*
 * APFS Fletcher-64 over `len` bytes at `p` (`len` a multiple of 4).  Callers
 * pass block+8 / blocksize-8: the stored checksum is not part of its own sum.
 */
uint64_t	fs_apfs_fletcher64(const void *p, uint32_t len);

/*
 * Write a METADATA block back, sealing it first: the Fletcher-64 is computed
 * over the block from offset 8 and stored in the header, so the result cannot
 * be part of its own input.  Exposed because fs/fs_txn.c writes the blocks a
 * transaction collected and sealing is not knowledge worth having twice.
 *
 * Not for file data, which carries no header and needs no seal -- writing
 * data through here would overwrite its first eight bytes with a checksum.
 */
int	fs_apfs_write_block(uint64_t bno, void *buf);

/* ---- writing ------------------------------------------------------- */

/*
 * WHAT THIS WRITER IS, AND WHAT IT DELIBERATELY IS NOT.
 *
 * APFS is a copy-on-write filesystem: Apple's implementation writes a changed
 * metadata block to a NEW location, updates the object map to point at it,
 * and publishes the result by writing a fresh checkpoint superblock with a
 * higher transaction id.  Nothing is overwritten, so an interrupted write
 * leaves the previous checkpoint intact and the volume mounts as it was.
 *
 * This writer does none of that.  It mutates IN PLACE: a changed block is
 * written back where it already lived.  That forfeits exactly one property --
 * crash safety, since a power loss mid-write leaves a block half-updated with
 * no older version to fall back to -- and buys the absence of the two hardest
 * pieces of APFS, the space manager (allocation bitmap chunks plus free-queue
 * B-trees keyed by xid) and the recursive object-map rewrite that copy-on-
 * write forces.  A volume this writer has touched is still a VALID APFS
 * volume at rest: checksums are recomputed, the tree shape is untouched, and
 * an independent implementation reads it back.  It is simply not a volume
 * that was written the way Apple writes one.
 *
 * The consequence is a hard boundary, and the API states it rather than
 * papering over it: nothing here can allocate a block.  A write may only land
 * on blocks the file already owns.  Growing a file, filling a hole, creating
 * a file, and adding a directory entry all need an allocator and all return
 * FS_APFS_E_NOALLOC.  Overwriting bytes that are already there is the whole
 * of what works, and it works completely.
 */

/*
 * Overwrite `len` bytes of the resolved file (`id`, `size`) at file offset
 * `off`, reporting the count written through *out_put.
 *
 * Refuses rather than truncates: a write that would extend the file past
 * `size`, or that lands anywhere the file has no block (a hole, or a range no
 * extent record covers), returns FS_APFS_E_NOALLOC having written nothing it
 * cannot account for.  Coverage is verified by counting bytes actually
 * written and comparing against the request -- an absent extent record is
 * silence, not an error, and silence must not read as success.
 *
 * Partial blocks are read-modify-written, so the bytes around the request
 * survive it.
 */
int	fs_apfs_pwrite(uint64_t id, uint64_t size, uint64_t off,
	    const uint8_t *buf, uint32_t len, uint32_t *out_put);

/*
 * Stamp an inode's modification and change times, in nanoseconds since the
 * Unix epoch, which is how APFS stores them.
 *
 * This is the first thing here to rewrite METADATA, and therefore the first
 * to recompute a Fletcher-64 rather than merely check one: the B-tree leaf
 * holding the record is re-read, patched, re-sealed and written back.  Note
 * the transaction id is left alone -- bumping it would oblige us to publish a
 * new checkpoint, which is the next rung and not this one.
 */
int	fs_apfs_touch(uint64_t oid, uint64_t mtime_ns);

#endif /* !_SYS_FS_APFS_H_ */
