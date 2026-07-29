/*
 * THE APFS WRITER, ON THE HOST
 *
 *	make hostapfs
 *	obj/hostapfs obj/style9.apfs [test ...]
 *
 * fs/apfs is ordinary C over a block device.  It reaches outside itself for
 * exactly five things -- bio_read, bio_write, kmalloc, kfree, kprintf -- which
 * was measured with nm rather than assumed, and every one of them has a
 * one-line answer on a host.  So the whole subsystem, self-tests and all, runs
 * against an image file with no kernel, no QEMU and no boot.
 *
 * WHY THIS EXISTS.  Until now the only way to learn whether a change to the
 * writer was right was to build a kernel, boot QEMU, run every self-test the
 * system has, and then apfsck the image: about four minutes for one bit of
 * information, which is longer than writing the change usually takes.  A loop
 * that slow does not get run per-edit, so it gets run per-BATCH, and a batch
 * that fails tells you less than four separate runs would have.  Here the same
 * tests take under a second and apfsck can be called in the same breath.
 *
 * WHAT IT DOES NOT REPLACE.  Ring 3, interrupts, a real ATA driver and the
 * page cache under bio are all absent, so the QEMU pass stays exactly as
 * necessary as it was for anything above the filesystem.  This is for the
 * arithmetic: paddings, key order, node splits, footer counts.
 *
 * AND WHAT IT CANNOT SEE, measured rather than guessed by putting a bug back
 * and watching this miss it.  Every test here leaves the volume as it found
 * it, so a defect that exists only WHILE the tests run is gone before apfsck
 * is called: the footer bug this rung fixed -- a key longer than the tree had
 * ever held, which nothing raised the high-water mark for -- was reintroduced
 * on purpose and this reported a clean run, correctly, because by the end the
 * long key had been taken back out and a footer of 25 was honest again.  An
 * end-state checker cannot answer a mid-run question.  What would catch that
 * class is an invariant asserted after each test rather than at the end, which
 * is a self-test to be written and not a property of this harness.
 *
 * What it DOES catch was measured the same way: emptying the data stream in
 * inode_renamed -- exactly the mistake a rename that rebuilt the record like a
 * create would make -- fails apfs-move here in seconds, twice, with a non-zero
 * exit for a script to act on.
 */
/*
 * pread and pwrite are POSIX, not ISO C, and -std=c11 alone hides them --
 * which matters rather than being tidiness: implicitly declared they would
 * return int, and a truncated ssize_t is a short read this code would then
 * report as success.
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

/* ---- the five ----------------------------------------------------------- */

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
 * Every line the writer prints, and a tally of the ones that say a test
 * failed.  Counted HERE rather than by grepping afterwards, so that the exit
 * status of this program is the answer and a script does not have to parse
 * prose to find out whether anything went wrong.
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
 * In the order kmain runs them, because the order is load-bearing: the index
 * and drop tests leave a deeper tree than the pristine volume has, apfs-room
 * fills a leaf in it, and apfs-seek goes last on purpose so that the descent
 * it checks has had to choose.
 */
static const struct hosttest	tests[] = {
	{ "alloc",  run_alloc,		     false },
	{ "split",  run_split,		     false },
	{ "index",  fs_apfs_index_selftest,  true  },
	{ "drop",   fs_apfs_drop_selftest,   true  },
	{ "stream", fs_apfs_stream_selftest, true  },
	{ "room",   fs_apfs_room_selftest,   true  },
	{ "move",   fs_apfs_move_selftest,   true  },
	{ "orphan", fs_apfs_orphan_selftest, true  },
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
	 * One wall-clock reading for the whole run, in nanoseconds, exactly as
	 * the kernel passes one down from fs.c.  The tests only ever write it
	 * into records, so a single value keeps two runs of this comparable.
	 */
	now = (uint64_t)time(NULL) * 1000000000ULL;

	/*
	 * THE LIST IS RUN TWICE, and the reason is what a leak looks like here.
	 * "Nothing still held at the end" is the wrong question: a mounted
	 * volume legitimately keeps buffers for its space manager, its bitmap,
	 * its chunk-info block and each free queue, and the kernel never frees
	 * those either because it never unmounts.  What a leak DOES look like
	 * is the same work costing more the second time, so the two passes are
	 * compared against each other rather than against zero.
	 *
	 * It buys a second thing for nothing: these tests are all written to
	 * leave the volume as they found it, and a pass that only works on a
	 * pristine tree fails here rather than in some later boot.
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
