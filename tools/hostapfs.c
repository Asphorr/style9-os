/*
 * The APFS writer, on the host.
 *
 *	make hostapfs
 *	obj/hostapfs obj/style9.apfs [test ...]
 *
 * fs/apfs is ordinary C over a block device.  What it needs from the kernel
 * (bio_read, bio_write, bio_sync, kmalloc, kfree, kprintf, and three ATA
 * hooks for the block autopsy) is stubbed here in a line or two each, so the
 * subsystem and its self-tests run against an image file with no kernel,
 * QEMU or boot: a second, not the four minutes of a boot, and apfsck right
 * after.
 *
 * It does not replace the QEMU pass: ring 3, interrupts, the ATA driver and
 * the cache under bio are absent.  This is for the arithmetic -- paddings,
 * key order, node splits, footer counts.
 *
 * Every test leaves the volume as it found it, so a defect that exists only
 * while the tests run is gone before apfsck looks; a footer high-water mark
 * left unraised was reintroduced on purpose and went unseen here.  Catching
 * that class needs an invariant checked after each test.  A defect that
 * survives does fail here: emptying the data stream in inode_renamed fails
 * apfs-move, with a nonzero exit.
 */
/*
 * pread and pwrite are POSIX, not ISO C, and -std=c11 hides them; implicitly
 * declared they would return int, and a truncated ssize_t could make a short
 * read look like success.
 */
#define	_POSIX_C_SOURCE	200809L

#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "apfs.h"

#define	SECTOR		512
#define	FAIL_MARK	"FAIL"

static int	 img = -1;
static long	 alloc_live;		/* kmalloc that has not been freed */
static long	 alloc_total;
static int	 fails;			/* lines a test called a failure */

/* ---- what fs/apfs needs from the kernel --------------------------------- */

void *
kmalloc(size_t size)
{
	void	*p;

	p = malloc(size);
	if (p != NULL) {
		alloc_live++;
		alloc_total++;
	}
	return (p);
}

void
kfree(void *p)
{

	if (p != NULL)
		alloc_live--;
	free(p);
}

/*
 * Every line the writer prints, counting the ones that say a test failed,
 * so the exit status is the answer and no script has to parse the output.
 */
int
kprintf(const char *fmt, ...)
{
	va_list	ap;
	int	n;

	if (strstr(fmt, FAIL_MARK) != NULL)
		fails++;
	va_start(ap, fmt);
	n = vprintf(fmt, ap);
	va_end(ap);
	fflush(stdout);
	return (n);
}

int
bio_read(unsigned drive, uint64_t lba, uint32_t nsec, void *buf)
{
	ssize_t	n;

	(void)drive;
	n = pread(img, buf, (size_t)nsec * SECTOR, (off_t)lba * SECTOR);
	if (n != (ssize_t)((size_t)nsec * SECTOR)) {
		fprintf(stderr, "hostapfs: read of %u sector(s) at %llu gave "
		    "%zd\n", nsec, (unsigned long long)lba, n);
		return (-1);
	}
	return (0);
}

/*
 * For block_autopsy in apfs.c: on a checksum failure it asks the ATA driver
 * for its interrupt counters and a read straight off the device, to tell a
 * cache fault from a device fault.  Here the image is the device, so the
 * counters are zero, the direct read is the same pread, and an autopsy
 * reports a corrupt image.
 */
uint32_t
ata_lost_intrs(void)
{

	return (0);
}

uint32_t
ata_overlaps(void)
{

	return (0);
}

int
ata_kread(unsigned drive_idx, uint64_t lba, uint32_t count, void *buf)
{

	return (bio_read(drive_idx, lba, count, buf));
}

int
bio_write(unsigned drive, uint64_t lba, uint32_t nsec, const void *buf)
{
	ssize_t	n;

	(void)drive;
	n = pwrite(img, buf, (size_t)nsec * SECTOR, (off_t)lba * SECTOR);
	if (n != (ssize_t)((size_t)nsec * SECTOR)) {
		fprintf(stderr, "hostapfs: write of %u sector(s) at %llu gave "
		    "%zd\n", nsec, (unsigned long long)lba, n);
		return (-1);
	}
	return (0);
}

/* A pwrite has no drive cache to lose; ordering is hosttorn's business. */
int
bio_sync(unsigned drive)
{

	(void)drive;
	return (0);
}

/* ---- the tests ---------------------------------------------------------- */

struct hosttest {
	const char	*ht_name;
	void		(*ht_run)(uint64_t now);
	bool		 ht_timed;	/* takes `now` rather than nothing */
};

static void	run_alloc(uint64_t now) { (void)now; fs_apfs_alloc_selftest(); }
static void	run_split(uint64_t now) { (void)now; fs_apfs_split_selftest(); }
static void	run_seek(uint64_t now)  { (void)now; fs_apfs_seek_selftest(); }
static void	run_ckpt(uint64_t now)  { (void)now; fs_apfs_ckpt_selftest(); }

/*
 * In kmain's order, except that ckpt runs last here.  The order matters: the
 * index and drop tests leave a deeper tree than the pristine volume's,
 * apfs-room fills a leaf in it, and apfs-seek comes after them so that the
 * descent it checks has had to choose.
 */
static const struct hosttest	tests[] = {
	{ "alloc",  run_alloc,		     false },
	{ "split",  run_split,		     false },
	{ "index",  fs_apfs_index_selftest,  true  },
	{ "drop",   fs_apfs_drop_selftest,   true  },
	{ "stream", fs_apfs_stream_selftest, true  },
	{ "extref", fs_apfs_extref_selftest, true  },
	{ "room",   fs_apfs_room_selftest,   true  },
	{ "move",   fs_apfs_move_selftest,   true  },
	{ "orphan", fs_apfs_orphan_selftest, true  },
	{ "clobber", fs_apfs_clobber_selftest, true },
	{ "view",   fs_apfs_view_selftest,   true  },
	{ "seek",   run_seek,		     false },
	{ "ckpt",   run_ckpt,		     false },
};

static bool
wanted(int argc, char **argv, const char *name)
{
	int	i;

	if (argc < 3)
		return (true);		/* no names given: run them all */
	for (i = 2; i < argc; i++)
		if (strcmp(argv[i], name) == 0)
			return (true);
	return (false);
}

int
main(int argc, char **argv)
{
	uint64_t	now;
	long		held[2];
	unsigned	i;
	int		pass;
	int		ran;

	if (argc < 2) {
		fprintf(stderr, "usage: %s IMAGE [test ...]\n", argv[0]);
		fprintf(stderr, "tests:");
		for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++)
			fprintf(stderr, " %s", tests[i].ht_name);
		fprintf(stderr, "\n");
		return (2);
	}
	img = open(argv[1], O_RDWR);
	if (img < 0) {
		perror(argv[1]);
		return (2);
	}

	fs_apfs_init();
	if (!fs_apfs_ready()) {
		fprintf(stderr, "hostapfs: %s did not mount\n", argv[1]);
		return (1);
	}

	/*
	 * One wall-clock reading in nanoseconds for the whole run, as fs.c
	 * passes one down; the tests only write it into records.
	 */
	now = (uint64_t)time(NULL) * 1000000000ULL;

	/*
	 * The list runs twice.  A mounted volume legitimately holds buffers
	 * (space manager, bitmaps, chunk-info block, free queues) that are
	 * never freed, since the kernel never unmounts, so a leak shows as the
	 * second pass holding more than the first, not as a nonzero total.
	 * The second pass also fails any test that only works on a pristine
	 * tree.
	 */
	ran = 0;
	for (pass = 0; pass < 2; pass++) {
		for (i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
			if (!wanted(argc, argv, tests[i].ht_name))
				continue;
			tests[i].ht_run(tests[i].ht_timed ? now : 0);
			if (pass == 0)
				ran++;
		}
		if (fs_apfs_checkpoint() != FS_APFS_E_OK) {
			fprintf(stderr, "hostapfs: the checkpoint closing pass "
			    "%d was refused\n", pass + 1);
			fails++;
		}
		held[pass] = alloc_live;
	}
	(void)close(img);

	printf("hostapfs: %d test(s) twice, %d failure(s), %ld kmalloc(s) held "
	    "after each pass", ran, fails, held[1]);
	if (held[0] != held[1])
		printf(" -- %ld MORE than after the first, which is a leak per "
		    "run", held[1] - held[0]);
	printf(" (%ld allocated in all)\n", alloc_total);
	return (fails != 0 || held[0] != held[1] ? 1 : 0);
}
