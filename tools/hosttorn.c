/*
 * THE POWER FAILS ON PURPOSE, AT EVERY POSSIBLE MOMENT
 *
 *	make torncheck
 *	obj/hosttorn obj/style9.apfs [workload] [-k N [-s S]]
 *
 * Every rung of the writer has leaned on one sentence: the checkpoint is the
 * only atom this volume has.  The clobbering rename leaned on it hardest --
 * "the takeover and the reap reach the platter as one published state" -- and
 * the sentence has been enforced by construction and never once measured.
 * This stand measures it, the way the sabotage ladder measured apfsck: not by
 * trusting the design but by doing the harm and watching the answer.
 *
 * THE HARM.  A workload runs against the image and the harness kills the
 * process INSIDE bio_write: after the Kth write of the measured edit has
 * landed, the (K+1)th either never starts (a clean cut) or lands its first S
 * sectors and no more (a torn write -- the disk's own failure mode, since a
 * platter has no obligation to finish a 4 KiB block it was half way through).
 * Then a fresh process mounts what is left and answers three questions: does
 * it mount at all, is the volume WHOLLY the old state or WHOLLY the new one,
 * and does apfsck agree.  K sweeps every write the edit makes, so every
 * moment the power could fail is a moment it does.
 *
 * WHAT MUST BE TRUE.  There is exactly one write that changes the answer --
 * the container superblock landing in the descriptor ring -- and the sweep
 * asserts it: old state at every cut before that write, new state at it and
 * after, never a third state, never an unmountable disk.  A torn write must
 * never flip the answer ON ITS OWN, because a torn block fails its
 * Fletcher-64 and a reader that adopts a block it cannot checksum has no
 * business calling itself a reader.  A tear CAN instead complete a write:
 * when every byte that differs sits inside the sectors that landed -- a
 * superblock's tail is padding, and padding does not change between
 * checkpoints -- the block on the platter is whole, and the sweep judges
 * the moment as the write having finished, because that is what it is.
 *
 * THE ONE-WRITE CRASH WINDOW, measured first and then proved irreducible.
 * A checkpoint ends with two copies of the same superblock: the ring slot,
 * which is the commit, and block zero, which is a courtesy for readers that
 * start there.  Two fixed locations cannot both change in one block write,
 * so between those two writes EVERY volume of this design is a volume whose
 * anchor lags its ring -- and apfsck says exactly that ("Block zero: the
 * filesystem was not unmounted cleanly", which after a power cut is a true
 * statement) and withholds its clean bill.  The order is still the right
 * order; committing the anchor first would advertise a checkpoint the ring
 * does not hold.  So the window's grid points answer to a two-tier oracle:
 * strict apfsck must refuse, and apfsck -u -- uncleanliness tolerated,
 * everything else still checked, which was verified by breaking one byte of
 * a catalog node and watching -u catch it -- must call the volume whole.
 * Everywhere else, strict apfsck must simply be happy.
 *
 * WHAT THE MODEL IS, honestly.  Writes land in the order they were issued
 * and a cut keeps a prefix.  A real disk with a write cache can reorder
 * across that prefix unless someone tells it not to, and nothing in this
 * stack issues a flush yet -- so what is proved here is that the LOGICAL
 * order is crash-consistent, which is the property a flush discipline would
 * then pin to the platter.  Reorder-under-cache is that later rung's
 * question, and pretending this sweep answers it would be the lie the essay
 * above the acceptance script warns about.
 *
 * HOW THE IMAGE COMES BACK.  Every write is journaled with its pre-image
 * before it lands (one pread, one append), and between grid points the
 * parent replays the journal BACKWARDS.  That restores exactly what the
 * dying child touched, with no assumption that two runs of the workload
 * write the same blocks -- the undo log is per-run truth, not a bet on
 * determinism.  The determinism that IS relied on -- the same start state
 * driving the same write COUNT, so K means the same moment every time -- is
 * asserted rather than assumed: a child that survives a cut below W, or
 * refuses an operation the dry run completed, fails the stand out loud.
 *
 * The single-point form (-k, and -s for the torn variant) runs one grid
 * point, does NOT restore, and leaves the corpse for apfspoke and apfsck:
 * the autopsy mode, for when the sweep names a K and a human wants to look.
 */
#define	_POSIX_C_SOURCE	200809L

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "apfs.h"

#define	SECTOR		512
#define	BLKSZ		4096
#define	TORN_EXIT	42	/* the cut child died where it was told to */
#define	BUG_EXIT	99	/* a workload op refused on an honest disk */

/*
 * One wall-clock value for every run of every child.  Fixed rather than read,
 * because the sweep's whole vocabulary -- "the Kth write" -- depends on two
 * runs of the same workload issuing the same sequence, and a timestamp is the
 * one input that would differ.
 */
#define	NOW		1756000000000000000ULL

static const char	*img_path;
static int		 img  = -1;
static int		 undo = -1;	/* pre-image journal, O_APPEND */
static FILE		*sink;		/* where kprintf goes; NULL = stdout */

/* The cut, armed only in a cut child and only past the setup phase. */
static bool		 counting;	/* the measured edit has begun */
static long		 seen;		/* measured writes fully landed */
static long		 cut = -1;	/* die on write cut+1; -1 = never */
static int		 tear;		/* sectors of the fatal write to land */
static int		 wpipe = -1;	/* dry run reports W through this */
static long		 odd_writes;	/* writes that were not one block */

/* ---- the five ----------------------------------------------------------- */

void *
kmalloc(size_t size)
{

	return (malloc(size));
}

void
kfree(void *p)
{

	free(p);
}

int
kprintf(const char *fmt, ...)
{
	va_list	ap;
	int	n;

	va_start(ap, fmt);
	n = vfprintf(sink != NULL ? sink : stdout, fmt, ap);
	va_end(ap);
	return (n);
}

int
bio_read(unsigned drive, uint64_t lba, uint32_t nsec, void *buf)
{
	ssize_t	n;

	(void)drive;
	n = pread(img, buf, (size_t)nsec * SECTOR, (off_t)lba * SECTOR);
	return (n == (ssize_t)((size_t)nsec * SECTOR) ? 0 : -1);
}

/*
 * The block autopsy's appetite, fed the same way hostapfs feeds it (see the
 * essay there): no driver and no platter on a host, so the counters are zero
 * and the straight-off-the-device read is the same pread as every other.
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

/*
 * The seam the whole stand hangs on.  Journal the pre-image, then either
 * land the write whole, or land S sectors of it and die on the spot --
 * _exit, not exit, because a power failure does not run atexit handlers
 * either.  The journal entry goes down BEFORE the torn sectors do, so even
 * the block this child mutilates on its way out is restorable.
 */
int
bio_write(unsigned drive, uint64_t lba, uint32_t nsec, const void *buf)
{
	uint8_t		 pre[BLKSZ];
	uint8_t		 hdr[12];
	size_t		 len;
	ssize_t		 n;
	uint32_t	 i;

	(void)drive;
	len = (size_t)nsec * SECTOR;
	if (len > sizeof(pre)) {
		fprintf(stderr, "hosttorn: %u-sector write cannot be "
		    "journaled\n", nsec);
		return (-1);
	}
	if (nsec != BLKSZ / SECTOR)
		odd_writes++;

	if (undo >= 0) {
		n = pread(img, pre, len, (off_t)lba * SECTOR);
		if (n != (ssize_t)len) {
			fprintf(stderr, "hosttorn: pre-image of %llu "
			    "unreadable\n", (unsigned long long)lba);
			return (-1);
		}
		for (i = 0; i < 8; i++)
			hdr[i] = (uint8_t)(lba >> (i * 8));
		for (i = 0; i < 4; i++)
			hdr[8 + i] = (uint8_t)(nsec >> (i * 8));
		if (write(undo, hdr, sizeof(hdr)) != (ssize_t)sizeof(hdr) ||
		    write(undo, pre, len) != (ssize_t)len) {
			fprintf(stderr, "hosttorn: the undo journal would "
			    "not take an entry\n");
			return (-1);
		}
	}

	if (counting && cut >= 0 && seen == cut) {
		if (tear > 0 && (uint32_t)tear < nsec)
			(void)!pwrite(img, buf, (size_t)tear * SECTOR,
			    (off_t)lba * SECTOR);
		if (sink != NULL)
			fflush(sink);
		_exit(TORN_EXIT);
	}

	n = pwrite(img, buf, len, (off_t)lba * SECTOR);
	if (n != (ssize_t)len)
		return (-1);
	if (counting)
		seen++;
	return (0);
}

/* ---- the workloads ------------------------------------------------------ */

/*
 * Four edits, chosen for what they make the checkpoint carry: a creation
 * (records and fresh extents), the clobbering rename (the takeover, the
 * orphan machinery and a reap in one edit -- the sentence this stand exists
 * to test), an unlink (records leaving, blocks to the free queue), and a
 * growth (the extent reference tree's new two-level paths under power loss).
 * Each is a setup phase that completes -- its own checkpoint and all --
 * and a measured phase that ends in exactly one fs_apfs_checkpoint(); only
 * the measured phase's writes are counted and cut.
 */

static void
must(int rv, const char *what)
{

	if (rv != FS_APFS_E_OK) {
		fprintf(stderr, "hosttorn: %s answered %d on an honest "
		    "disk\n", what, rv);
		_exit(BUG_EXIT);
	}
}

static uint64_t
etc_ino(void)
{
	uint64_t	oid;
	int		isdir;

	must(fs_apfs_lookup("/etc", &oid, &isdir), "lookup of /etc");
	if (!isdir)
		_exit(BUG_EXIT);
	return (oid);
}

static void
make_file(uint64_t dir, const char *name, uint32_t blocks, uint8_t fill)
{
	uint8_t		 buf[BLKSZ];
	uint64_t	 ino;
	uint64_t	 size;
	uint32_t	 b;
	uint32_t	 put;

	must(fs_apfs_create(dir, name, NOW, 0644, &ino), "create");
	size = (uint64_t)blocks * BLKSZ;
	must(fs_apfs_grow(ino, ino, size), "grow");
	memset(buf, fill, sizeof(buf));
	for (b = 0; b < blocks; b++) {
		must(fs_apfs_pwrite(ino, size, (uint64_t)b * BLKSZ, buf,
		    BLKSZ, &put), "pwrite");
		if (put != BLKSZ)
			_exit(BUG_EXIT);
	}
}

/*
 * Classifiers answer with the verify child's exit code.  OLD and NEW are
 * checked to the byte: a state that merely LOOKS old is exactly the kind of
 * half-published edit the sweep exists to catch, so "the name is there" is
 * never enough -- the size and every byte have to be the state's own.
 */
#define	CLS_OLD		10
#define	CLS_NEW		11
#define	CLS_MIX		12
#define	CLS_NOMOUNT	13

/* 1 = absent, 0 = present, -1 = the question itself failed */
static int
absent(const char *path)
{
	uint64_t	oid;
	int		isdir;
	int		rv;

	rv = fs_apfs_lookup(path, &oid, &isdir);
	if (rv == FS_APFS_E_NOTFOUND)
		return (1);
	return (rv == FS_APFS_E_OK ? 0 : -1);
}

/* 1 = the file is exactly this shape, 0 = it is not, -1 = unreadable */
static int
shaped(const char *path, uint32_t size, uint32_t split, uint8_t head,
    uint8_t tail)
{
	uint8_t		*buf;
	uint32_t	 got;
	uint32_t	 i;
	int		 ok;

	if (fs_apfs_slurp(path, &buf, &got) != FS_APFS_E_OK)
		return (-1);
	ok = (got == size);
	for (i = 0; ok && i < got; i++)
		ok = (buf[i] == (i < split ? head : tail));
	kfree(buf);
	return (ok);
}

/* -- creation: /etc/torn.txt, two blocks of 'T', from nothing ------------- */

static void
w_create_setup(void)
{
}

static void
w_create_measured(void)
{

	make_file(etc_ino(), "torn.txt", 2, 'T');
	must(fs_apfs_checkpoint(), "checkpoint");
}

static int
w_create_classify(void)
{
	int	a;

	a = absent("/etc/torn.txt");
	if (a < 0)
		return (CLS_MIX);
	if (a == 1)
		return (CLS_OLD);
	return (shaped("/etc/torn.txt", 2 * BLKSZ, 2 * BLKSZ, 'T', 0) == 1 ?
	    CLS_NEW : CLS_MIX);
}

/* -- the takeover: usurper.txt over usurped.txt, and the victim reaped ---- */

static void
w_clobber_setup(void)
{
	uint64_t	etc;

	etc = etc_ino();
	make_file(etc, "usurped.txt", 1, 'O');
	make_file(etc, "usurper.txt", 2, 'N');
	must(fs_apfs_checkpoint(), "setup checkpoint");
}

static void
w_clobber_measured(void)
{
	uint64_t	etc;
	uint64_t	victim;

	etc = etc_ino();
	must(fs_apfs_rename(etc, "usurper.txt", etc, "usurped.txt", NOW,
	    &victim), "clobbering rename");
	if (victim == 0)
		_exit(BUG_EXIT);
	must(fs_apfs_reap(victim, NOW), "reap");
	must(fs_apfs_checkpoint(), "checkpoint");
}

static int
w_clobber_classify(void)
{
	int	a;

	a = absent("/etc/usurper.txt");
	if (a < 0)
		return (CLS_MIX);
	if (a == 0) {
		if (shaped("/etc/usurper.txt", 2 * BLKSZ, 2 * BLKSZ,
		    'N', 0) == 1 &&
		    shaped("/etc/usurped.txt", 1 * BLKSZ, 1 * BLKSZ,
		    'O', 0) == 1)
			return (CLS_OLD);
		return (CLS_MIX);
	}
	return (shaped("/etc/usurped.txt", 2 * BLKSZ, 2 * BLKSZ, 'N', 0) == 1 ?
	    CLS_NEW : CLS_MIX);
}

/* -- an unlink: three blocks of 'D' leave the volume ---------------------- */

static void
w_unlink_setup(void)
{

	make_file(etc_ino(), "doomed.txt", 3, 'D');
	must(fs_apfs_checkpoint(), "setup checkpoint");
}

static void
w_unlink_measured(void)
{

	must(fs_apfs_unlink(etc_ino(), "doomed.txt", NOW), "unlink");
	must(fs_apfs_checkpoint(), "checkpoint");
}

static int
w_unlink_classify(void)
{
	int	a;

	a = absent("/etc/doomed.txt");
	if (a < 0)
		return (CLS_MIX);
	if (a == 1)
		return (CLS_NEW);
	return (shaped("/etc/doomed.txt", 3 * BLKSZ, 3 * BLKSZ, 'D', 0) == 1 ?
	    CLS_OLD : CLS_MIX);
}

/* -- a growth: one block becomes four, through the extent reference tree -- */

static void
w_grow_setup(void)
{

	make_file(etc_ino(), "stretch.txt", 1, 'G');
	must(fs_apfs_checkpoint(), "setup checkpoint");
}

static void
w_grow_measured(void)
{
	uint8_t		 buf[BLKSZ];
	uint64_t	 id;
	uint64_t	 ino;
	uint64_t	 size;
	uint32_t	 b;
	uint32_t	 put;

	must(fs_apfs_open("/etc/stretch.txt", &id, &size, &ino), "open");
	must(fs_apfs_grow(ino, id, 4 * BLKSZ), "grow");
	memset(buf, 'H', sizeof(buf));
	for (b = 1; b < 4; b++) {
		must(fs_apfs_pwrite(id, 4 * BLKSZ, (uint64_t)b * BLKSZ, buf,
		    BLKSZ, &put), "pwrite");
		if (put != BLKSZ)
			_exit(BUG_EXIT);
	}
	must(fs_apfs_checkpoint(), "checkpoint");
}

static int
w_grow_classify(void)
{
	int	old;
	int	new;

	old = shaped("/etc/stretch.txt", 1 * BLKSZ, 1 * BLKSZ, 'G', 0);
	if (old == 1)
		return (CLS_OLD);
	new = shaped("/etc/stretch.txt", 4 * BLKSZ, 1 * BLKSZ, 'G', 'H');
	if (new == 1)
		return (CLS_NEW);
	return (CLS_MIX);
}

struct workload {
	const char	*w_name;
	void		(*w_setup)(void);
	void		(*w_measured)(void);
	int		(*w_classify)(void);
};

static const struct workload	workloads[] = {
	{ "create",  w_create_setup,  w_create_measured,  w_create_classify  },
	{ "clobber", w_clobber_setup, w_clobber_measured, w_clobber_classify },
	{ "unlink",  w_unlink_setup,  w_unlink_measured,  w_unlink_classify  },
	{ "grow",    w_grow_setup,    w_grow_measured,    w_grow_classify    },
};

/* ---- the harness -------------------------------------------------------- */

static char	logpath[512];
static char	undopath[512];

/*
 * Run the workload in a child that will die mid-write (or complete, when K
 * is at or past W -- the dry run is exactly that with K = -1).  The child's
 * chatter goes to the iteration log; a completed run reports its measured
 * write count back through the pipe, which is how the parent learns W
 * without parsing prose.
 */
static int
run_cut(const struct workload *wl, long k, int s, bool to_stdout)
{
	pid_t	pid;
	int	fds[2];
	int	status;
	long	w;

	if (pipe(fds) != 0) {
		perror("pipe");
		exit(2);
	}
	pid = fork();
	if (pid == 0) {
		close(fds[0]);
		wpipe = fds[1];
		if (!to_stdout) {
			sink = fopen(logpath, "a");
			if (sink == NULL)
				_exit(BUG_EXIT);
		}
		fs_apfs_init();
		if (!fs_apfs_ready())
			_exit(CLS_NOMOUNT);
		wl->w_setup();
		counting = true;
		cut = k;
		tear = s;
		wl->w_measured();
		w = seen;
		(void)!write(wpipe, &w, sizeof(w));
		if (sink != NULL)
			fflush(sink);
		_exit(0);
	}
	close(fds[1]);
	w = -1;
	(void)!read(fds[0], &w, sizeof(w));
	close(fds[0]);
	waitpid(pid, &status, 0);
	if (WIFEXITED(status) && WEXITSTATUS(status) == 0)
		return ((int)w);	/* completed; w = W */
	if (WIFEXITED(status) && WEXITSTATUS(status) == TORN_EXIT)
		return (-1);		/* died at the cut, as told */
	return (-2);			/* something the model forbids */
}

/* Mount what is left and say which state it is.  Reads only. */
static int
run_verify(const struct workload *wl, bool to_stdout)
{
	pid_t	pid;
	int	status;

	pid = fork();
	if (pid == 0) {
		if (!to_stdout) {
			sink = fopen(logpath, "a");
			if (sink == NULL)
				_exit(BUG_EXIT);
		}
		fs_apfs_init();
		if (!fs_apfs_ready())
			_exit(CLS_NOMOUNT);
		_exit(wl->w_classify());
	}
	waitpid(pid, &status, 0);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : CLS_MIX);
}

static int
run_apfsck(const char *flag, bool to_stdout)
{
	pid_t	pid;
	int	fd;
	int	status;

	pid = fork();
	if (pid == 0) {
		if (!to_stdout) {
			fd = open(logpath, O_WRONLY | O_APPEND | O_CREAT,
			    0644);
			if (fd >= 0) {
				dup2(fd, 1);
				dup2(fd, 2);
			}
		}
		execlp("apfsck", "apfsck", flag, img_path, (char *)NULL);
		_exit(127);
	}
	waitpid(pid, &status, 0);
	return (WIFEXITED(status) ? WEXITSTATUS(status) : -1);
}

/*
 * The checker's three possible words about an image, in the two-tier scheme
 * the header essay lays out: strict (-c) happy; strict refusing while -u
 * calls the volume whole (a crash, honestly reported); or damage that even
 * uncleanliness-tolerance will not excuse.
 */
#define	FSCK_CLEAN	0
#define	FSCK_UNCLEAN	1
#define	FSCK_DAMAGED	2

static int
fsck_verdict(bool to_stdout)
{

	if (run_apfsck("-c", to_stdout) == 0)
		return (FSCK_CLEAN);
	return (run_apfsck("-u", to_stdout) == 0 ? FSCK_UNCLEAN :
	    FSCK_DAMAGED);
}

static const char *
cls_name(int cls)
{

	return (cls == CLS_OLD ? "old" : cls == CLS_NEW ? "new" :
	    cls == CLS_NOMOUNT ? "NO MOUNT" : "MIXED");
}

static const char *
fsck_name(int verdict)
{

	return (verdict == FSCK_CLEAN ? "clean" :
	    verdict == FSCK_UNCLEAN ? "unclean" : "DAMAGED");
}

/*
 * Put every block back, newest change first.  Replaying the journal
 * backwards undoes overlapping writes correctly by construction: the last
 * entry to touch a block is undone first, and the first entry -- whose
 * pre-image is the pristine content -- is undone last and therefore wins.
 */
static void
restore(void)
{
	static uint8_t	*jrn;
	static size_t	 cap;
	uint8_t		*p;
	off_t		 sz;
	size_t		 n;
	size_t		 count;
	uint64_t	 lba;
	uint32_t	 nsec;
	uint32_t	 i;
	size_t		*offs;

	sz = lseek(undo, 0, SEEK_END);
	if (sz <= 0) {
		if (sz < 0) {
			perror("hosttorn: undo lseek");
			exit(2);
		}
		return;
	}
	if ((size_t)sz > cap) {
		free(jrn);
		cap = (size_t)sz;
		jrn = malloc(cap);
		if (jrn == NULL) {
			fprintf(stderr, "hosttorn: no memory for the undo "
			    "journal\n");
			exit(2);
		}
	}
	if (pread(undo, jrn, (size_t)sz, 0) != (ssize_t)sz) {
		fprintf(stderr, "hosttorn: the undo journal would not read "
		    "back\n");
		exit(2);
	}

	/* index the entries, then walk the index backwards */
	count = 0;
	for (n = 0; n + 12 <= (size_t)sz; ) {
		nsec = 0;
		for (i = 0; i < 4; i++)
			nsec |= (uint32_t)jrn[n + 8 + i] << (i * 8);
		n += 12 + (size_t)nsec * SECTOR;
		count++;
	}
	if (n != (size_t)sz) {
		fprintf(stderr, "hosttorn: the undo journal is ragged "
		    "(%zu of %lld bytes)\n", n, (long long)sz);
		exit(2);
	}
	offs = malloc(count * sizeof(size_t));
	if (offs == NULL)
		exit(2);
	count = 0;
	for (n = 0; n + 12 <= (size_t)sz; ) {
		offs[count++] = n;
		nsec = 0;
		for (i = 0; i < 4; i++)
			nsec |= (uint32_t)jrn[n + 8 + i] << (i * 8);
		n += 12 + (size_t)nsec * SECTOR;
	}
	while (count > 0) {
		p = jrn + offs[--count];
		lba = 0;
		for (i = 0; i < 8; i++)
			lba |= (uint64_t)p[i] << (i * 8);
		nsec = 0;
		for (i = 0; i < 4; i++)
			nsec |= (uint32_t)p[8 + i] << (i * 8);
		if (pwrite(img, p + 12, (size_t)nsec * SECTOR,
		    (off_t)lba * SECTOR) != (ssize_t)((size_t)nsec * SECTOR)) {
			fprintf(stderr, "hosttorn: restore of %llu failed\n",
			    (unsigned long long)lba);
			exit(2);
		}
	}
	free(offs);
	if (ftruncate(undo, 0) != 0) {
		perror("hosttorn: undo truncate");
		exit(2);
	}
}

static void
show_log(void)
{
	FILE	*f;
	int	 c;

	f = fopen(logpath, "r");
	if (f == NULL)
		return;
	while ((c = fgetc(f)) != EOF)
		fputc(c, stdout);
	fclose(f);
}

/*
 * One grid point: cut at K (torn at S sectors, 0 = clean), verify, apfsck,
 * restore.  Everything the parent decides comes back in one struct so the
 * sweep below reads as the model it asserts.
 */
struct point {
	int	p_cut;		/* -1 died as told, W completed, -2 forbidden */
	int	p_cls;		/* CLS_* from the verify child */
	int	p_fsck;		/* FSCK_* from the two-tier oracle */
};

static struct point
grid_point(const struct workload *wl, long k, int s, bool keep)
{
	struct point	pt;
	off_t		before;

	if (truncate(logpath, 0) != 0 && errno != ENOENT) {
		perror("hosttorn: log truncate");
		exit(2);
	}
	pt.p_cut = run_cut(wl, k, s, false);
	before = lseek(undo, 0, SEEK_END);
	pt.p_cls = run_verify(wl, false);
	if (lseek(undo, 0, SEEK_END) != before) {
		fprintf(stderr, "hosttorn: the VERIFY mount wrote to the "
		    "disk -- a reader that writes is its own finding\n");
		show_log();
		exit(2);
	}
	pt.p_fsck = fsck_verdict(false);
	if (!keep)
		restore();
	return (pt);
}

/*
 * The measured model, position by position.  Writes are numbered 1..W; a
 * point (k, s) holds k whole writes and s sectors of write k+1.  The ring
 * superblock is write W-1 and the anchor is write W -- asserted rather than
 * assumed, because the flip discovered on the clean row must land exactly
 * there.  The crash window of the header essay is the set of states where
 * the ring superblock's content is whole and the anchor's is not: reached
 * by cutting after W-1, or by a tear of W-1 that completed it.  A torn
 * anchor completes on this superblock layout (every differing byte is in
 * the first sector); if a future layout moves a differing byte past the
 * torn prefix, the point stops matching and the sweep says so out loud --
 * which is the moment to decide what a mount without a readable block zero
 * should do, not before.
 */
static bool
point_allowed(long k, int s, long w, const struct point *pt)
{

	if (k == w)
		return (pt->p_cls == CLS_NEW && pt->p_fsck == FSCK_CLEAN);
	if (k == w - 1) {
		if (s == 0)
			return (pt->p_cls == CLS_NEW &&
			    pt->p_fsck == FSCK_UNCLEAN);
		return (pt->p_cls == CLS_NEW && pt->p_fsck == FSCK_CLEAN);
	}
	if (k == w - 2 && s > 0)
		return ((pt->p_cls == CLS_OLD && pt->p_fsck == FSCK_CLEAN) ||
		    (pt->p_cls == CLS_NEW && pt->p_fsck == FSCK_UNCLEAN));
	return (pt->p_cls == CLS_OLD && pt->p_fsck == FSCK_CLEAN);
}

/* ---- the sweep ---------------------------------------------------------- */

static const int	tears[] = { 0, 1, 4, 7 };

/*
 * Three tear positions, not seven: any partial landing breaks a sealed
 * block's Fletcher-64 the same way, so early, middle and late probe the
 * distinct outcomes (an unsealed DATA block has no checksum to break, and
 * for those what matters is only that no committed state references the
 * block -- which the state assertion already answers).  What earns clean
 * cuts AND tears at every K is that they fail differently: a clean cut
 * tests the ORDER of writes, a torn write tests the reader's refusal to
 * believe a half-written block.
 */
static int
sweep(const struct workload *wl)
{
	struct point	pt;
	long		W;
	long		flip;
	long		window;
	long		k;
	unsigned	t;
	int		devs;
	int		fsck0;

	/*
	 * No pristine pre-check: a workload's OLD state exists only after its
	 * setup phase has run, so the pristine image classifies as nothing at
	 * all (or, for an unlink, accidentally as NEW).  The k=0 grid point
	 * carries that duty properly -- setup complete, not one measured
	 * write landed, and the verdict must be OLD.
	 */

	/* the dry run: complete the workload once, learn W, prove NEW */
	if (truncate(logpath, 0) != 0 && errno != ENOENT)
		exit(2);
	W = run_cut(wl, -1, 0, false);
	if (W <= 0) {
		printf("hosttorn: %s: FAIL -- the dry run did not complete "
		    "(%ld)\n", wl->w_name, W);
		show_log();
		restore();
		return (1);
	}
	if (run_verify(wl, false) != CLS_NEW) {
		printf("hosttorn: %s: FAIL -- the completed workload does "
		    "not classify as the new state\n", wl->w_name);
		show_log();
		restore();
		return (1);
	}
	fsck0 = fsck_verdict(false);
	if (fsck0 != FSCK_CLEAN) {
		printf("hosttorn: %s: FAIL -- apfsck calls the completed "
		    "workload %s\n", wl->w_name, fsck_name(fsck0));
		show_log();
		restore();
		return (1);
	}
	restore();

	/*
	 * The grid.  The flip is still DISCOVERED -- the first clean cut
	 * that answers NEW -- and then asserted to sit exactly at write W-1,
	 * the ring superblock, one before the anchor.  Every point is judged
	 * by position against the measured model in point_allowed, and the
	 * crash-window points are counted rather than excused silently.
	 */
	devs = 0;
	window = 0;
	flip = -1;
	for (k = 0; k <= W; k++) {
		for (t = 0; t < sizeof(tears) / sizeof(tears[0]); t++) {
			if (k == W && tears[t] != 0)
				continue;	/* nothing left to tear */
			pt = grid_point(wl, k, tears[t], false);
			if (k < W ? (pt.p_cut != -1) : (pt.p_cut != W)) {
				printf("hosttorn: %s: FAIL -- k=%ld s=%d: "
				    "the cut child did not die at the cut "
				    "(%d)\n", wl->w_name, k, tears[t],
				    pt.p_cut);
				devs++;
				continue;
			}
			if (tears[t] == 0 && flip < 0 && pt.p_cls == CLS_NEW)
				flip = k;
			if (pt.p_cls == CLS_NEW &&
			    pt.p_fsck == FSCK_UNCLEAN)
				window++;
			if (!point_allowed(k, tears[t], W, &pt)) {
				printf("hosttorn: %s: DEVIATION k=%ld s=%d "
				    "-- state=%s fsck=%s (reproduce: "
				    "obj/hosttorn %s %s -k %ld -s %d)\n",
				    wl->w_name, k, tears[t],
				    cls_name(pt.p_cls),
				    fsck_name(pt.p_fsck), img_path,
				    wl->w_name, k, tears[t]);
				devs++;
			}
		}
	}
	if (flip != W - 1) {
		printf("hosttorn: %s: FAIL -- the flip sits at write %ld of "
		    "%ld; the ring superblock is expected second-to-last, "
		    "just before the anchor\n", wl->w_name, flip, W);
		return (devs + 1);
	}

	printf("hosttorn: %s -- %ld write(s) in the measured edit, %ld "
	    "power failure(s) staged, old until write %ld and new from it, "
	    "the anchor lagged at %ld point(s) (whole but unclean), %d "
	    "deviation(s)\n", wl->w_name, W,
	    (long)(W * (sizeof(tears) / sizeof(tears[0])) + 1), flip,
	    window, devs);
	return (devs);
}

int
main(int argc, char **argv)
{
	const struct workload	*only;
	long			 k;
	unsigned		 i;
	int			 arg;
	int			 s;
	int			 devs;

	if (argc < 2) {
		fprintf(stderr, "usage: %s IMAGE [workload] [-k N [-s S]]\n",
		    argv[0]);
		fprintf(stderr, "workloads:");
		for (i = 0; i < sizeof(workloads) / sizeof(workloads[0]); i++)
			fprintf(stderr, " %s", workloads[i].w_name);
		fprintf(stderr, "\n");
		return (2);
	}
	img_path = argv[1];
	img = open(img_path, O_RDWR);
	if (img < 0) {
		perror(img_path);
		return (2);
	}
	snprintf(logpath, sizeof(logpath), "%s.tornlog", img_path);
	snprintf(undopath, sizeof(undopath), "%s.undo", img_path);
	undo = open(undopath, O_RDWR | O_CREAT | O_APPEND | O_TRUNC, 0644);
	if (undo < 0) {
		perror(undopath);
		return (2);
	}

	only = NULL;
	k = -1;
	s = 0;
	for (arg = 2; arg < argc; arg++) {
		if (strcmp(argv[arg], "-k") == 0 && arg + 1 < argc) {
			k = atol(argv[++arg]);
		} else if (strcmp(argv[arg], "-s") == 0 && arg + 1 < argc) {
			s = atoi(argv[++arg]);
		} else {
			for (i = 0; i < sizeof(workloads) /
			    sizeof(workloads[0]); i++)
				if (strcmp(argv[arg],
				    workloads[i].w_name) == 0)
					only = &workloads[i];
			if (only == NULL) {
				fprintf(stderr, "hosttorn: no workload "
				    "called \"%s\"\n", argv[arg]);
				return (2);
			}
		}
	}

	/*
	 * The autopsy mode: one grid point, chatter to the terminal, and NO
	 * restore -- the image is left exactly as the power failure left it,
	 * for apfsck and apfspoke to examine at leisure.
	 */
	if (k >= 0) {
		struct point	pt;

		if (only == NULL) {
			fprintf(stderr, "hosttorn: -k needs a workload\n");
			return (2);
		}
		pt.p_cut = run_cut(only, k, s, true);
		pt.p_cls = run_verify(only, true);
		pt.p_fsck = fsck_verdict(true);
		printf("hosttorn: %s k=%ld s=%d -- cut=%d state=%s fsck=%s; "
		    "the image is left as the failure left it\n",
		    only->w_name, k, s, pt.p_cut, cls_name(pt.p_cls),
		    fsck_name(pt.p_fsck));
		return (0);
	}

	devs = 0;
	for (i = 0; i < sizeof(workloads) / sizeof(workloads[0]); i++) {
		if (only != NULL && only != &workloads[i])
			continue;
		devs += sweep(&workloads[i]);
	}
	if (odd_writes != 0)
		printf("hosttorn: %ld write(s) were not one block -- the "
		    "tear grid assumed 8 sectors\n", odd_writes);
	(void)unlink(undopath);
	printf("hosttorn: %d deviation(s) from the model in all\n", devs);
	return (devs != 0 ? 1 : 0);
}
