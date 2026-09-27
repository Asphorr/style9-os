/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * filewrite -- a self-authored Darwin-ABI probe for writing the volume from
 * ring 3, run before a genuine Apple binary (dash redirecting with `>')
 * depends on it.  Not an Apple binary, but built by the real toolchain,
 * bound by our dyld against our libSystem and importing the same symbols,
 * so a clean run proves the syscall path and not our own glue.
 *
 *	1. O_CREAT makes a file that was not there, with a usable fd.
 *	2. The bytes read back through a second open are the bytes written;
 *	   reading through the writing fd would only check a cache.
 *	3. A write into the middle overwrites without changing the length,
 *	   which a writer that always appended would fail.
 *	4. O_APPEND lands at the end wherever the cursor was.
 *	5. O_TRUNC empties the file at open time.
 *	6. unlink removes the name.
 *	7. A built-in (/bin/gcat, in the kernel's own text) still refuses a
 *	   write open.
 *	8. mkdir makes a directory and refuses to make it twice.
 *	9. A name can be made inside it, the only proof from out here that
 *	   a directory reached the disk; rmdir refuses while it is there and
 *	   succeeds once it is gone.
 *	10. rmdir refuses a file, unlink refuses a directory.
 *	11. rename moves a name and its bytes (the length lives in the
 *	    record the rename rewrites).
 *	12. rename onto a taken name: new opens get the newcomer while a
 *	    descriptor held on the old file still reads it to the end.
 *	13. An open file outlives its last name; afterwards the name is free
 *	    to an O_EXCL create.
 *	14. fsync(2) answers 0 on a written file and fails on a closed fd.
 *	15. The checkpoint fsync published can be read back by number under
 *	    /.xid, and nothing there can be written.
 *	16. A second descriptor sees the file grow through the first: read,
 *	    pread, lseek(SEEK_END) and O_APPEND all reach the new bytes.
 *	17. utimes(2) sets both times to the microsecond and stat reads
 *	    them back; futimes(2) with no times sets both to now; a missing
 *	    name is ENOENT.
 *	18. lseek past the end reads nothing there, and a write there
 *	    leaves a gap of zeros, across a block edge; cut inside a block
 *	    by ftruncate and grown back, a file reads zeros where the cut
 *	    bytes were.
 *
 * Freestanding: no SDK headers, prototypes declared as <fcntl.h>/<unistd.h>
 * would alias them, entry at _entry (ld -e), relinked low like dyldhello.
 */

typedef __UINT8_TYPE__	uint8_t;
typedef __UINT16_TYPE__	uint16_t;
typedef __UINT32_TYPE__	uint32_t;
typedef __UINT64_TYPE__	uint64_t;
typedef __INT32_TYPE__	int32_t;
typedef __INT64_TYPE__	int64_t;
typedef __SIZE_TYPE__	size_t;

#define	NULL		((void *)0)

#define	O_RDONLY	0x0000
#define	O_WRONLY	0x0001
#define	O_RDWR		0x0002
#define	O_APPEND	0x0008
#define	O_CREAT		0x0200
#define	O_TRUNC		0x0400
#define	O_EXCL		0x0800

#define	SEEK_SET	0
#define	SEEK_END	2

#define	ENOENT		2
#define	EEXIST		17
#define	ENOTDIR		20
#define	EISDIR		21
#define	ENOSPC		28
#define	EROFS		30
#define	ENOTEMPTY	66
#define	ESTALE		70

/*
 * The $INODE64 dirent, as an Apple binary sees it.  DIR stays opaque, as in
 * Apple's headers.
 */
struct dirent {
	uint64_t	d_ino;
	uint64_t	d_seekoff;
	uint16_t	d_reclen;
	uint16_t	d_namlen;
	uint8_t		d_type;
	char		d_name[1024];
};

extern void	*opendir(const char *path) __asm__("_opendir$INODE64");
extern struct dirent *readdir(void *dp) __asm__("_readdir$INODE64");
extern int	 closedir(void *dp);

/* The macOS $INODE64 struct stat, 144 bytes; only the times are read. */
struct stat {
	int32_t		st_dev;
	uint16_t	st_mode;
	uint16_t	st_nlink;
	uint64_t	st_ino;
	uint32_t	st_uid;
	uint32_t	st_gid;
	int32_t		st_rdev;
	int32_t		st_pad;
	int64_t		st_atime;
	int64_t		st_atimensec;
	int64_t		st_mtime;
	int64_t		st_mtimensec;
	int64_t		st_rest[10];	/* ctime .. qspare */
};
_Static_assert(sizeof(struct stat) == 144, "the $INODE64 struct stat");

struct timeval {
	int64_t		tv_sec;
	int32_t		tv_usec;
	int32_t		tv_pad;
};

extern int	 stat(const char *path, struct stat *sb)
		    __asm__("_stat$INODE64");
extern int	 utimes(const char *path, const struct timeval tv[2]);
extern int	 futimes(int fd, const struct timeval tv[2]);
extern int	 gettimeofday(struct timeval *tv, void *tz);

extern int	*__error(void);		/* Apple's <errno.h>: errno == *__error() */
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern long	 write(int fd, const void *buf, unsigned long n);
extern int	 close(int fd);
extern int	 fsync(int fd);
extern long	 lseek(int fd, long off, int whence);
extern long	 pread(int fd, void *buf, unsigned long n, long off);
extern int	 ftruncate(int fd, long len);
extern int	 unlink(const char *path);
extern int	 mkdir(const char *path, unsigned short mode);
extern int	 rmdir(const char *path);
extern int	 rename(const char *from, const char *to);
extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);

#define	PATH		"/etc/filewrite.txt"
#define	DIR		"/etc/ring3dir"
#define	DIRFILE		"/etc/ring3dir/inside.txt"
#define	HELD		"/etc/hello.txt"	/* off the image, never made */
#define	MOVED		"/var/db/moved.txt"	/* and in another directory */
#define	FIRST		"style9: written from ring 3 by filewrite\n"
#define	SECOND		"style9: and appended to, later\n"

static int	fails;

static size_t
slen(const char *s)
{
	size_t	n;

	for (n = 0; s[n] != '\0'; n++)
		continue;
	return (n);
}

static int
same(const char *a, const char *b, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		if (a[i] != b[i])
			return (0);
	return (1);
}

static int
zeros(const char *a, size_t n)
{
	size_t	i;

	for (i = 0; i < n; i++)
		if (a[i] != 0)
			return (0);
	return (1);
}

static void
fail(const char *what)
{

	printf("filewrite: FAIL %s\n", what);
	fails++;
}

/*
 * Read a whole file through a fresh descriptor, so the check is on the
 * file and not on the writing descriptor's state.
 */
static long
slurp(const char *path, char *buf, size_t cap)
{
	long	got;
	int	fd;

	fd = open(path, O_RDONLY);
	if (fd < 0)
		return (-1);
	got = read(fd, buf, cap);
	(void)close(fd);
	return (got);
}

/* The decimal digits of `v` into `dst`, no terminator; how many there were. */
static size_t
putnum(char *dst, unsigned long v)
{
	char	tmp[24];
	size_t	n;
	size_t	i;

	n = 0;
	do {
		tmp[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	for (i = 0; i < n; i++)
		dst[i] = tmp[n - 1 - i];
	return (n);
}

int
entry(void)
{
	char		 buf[512];
	char		 vpath[96];
	struct dirent	*ent;
	void		*dp;
	unsigned long	 best;
	unsigned long	 xid;
	size_t		 i;
	size_t		 n;
	long		 got;
	long		 put;
	int		 fd;
	int		 held;

	printf("filewrite: the volume, from ring 3\n");

	/* Whatever an earlier run left, so this starts from nothing. */
	(void)unlink(PATH);

	/*
	 * 1. a file that was not there.  EROFS is not a failure: the kernel
	 * also boots from FAT, which has no writer, so the run just stops.
	 */
	fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0666);
	if (fd < 0 && *__error() == EROFS) {
		printf("filewrite: this volume cannot be written -- there is "
		    "nothing here for ring 3 to change; skipped\n");
		exit(0);
	}
	if (fd < 0) {
		fail("open(O_CREAT) would not make " PATH);
		exit(1);
	}
	put = write(fd, FIRST, slen(FIRST));
	if (put != (long)slen(FIRST))
		fail("the first write was short");
	(void)close(fd);

	/* 2. the bytes, through a different descriptor. */
	got = slurp(PATH, buf, sizeof(buf));
	if (got != (long)slen(FIRST) || !same(buf, FIRST, slen(FIRST)))
		fail("what came back is not what went in");
	else
		printf("filewrite: PASS %ld bytes made it to %s and back\n",
		    got, PATH);

	/* 3. a write into the middle: same length, different bytes. */
	fd = open(PATH, O_RDWR);
	if (fd < 0)
		fail("cannot reopen for writing");
	else {
		if (lseek(fd, 8, SEEK_SET) != 8)
			fail("lseek would not move");
		if (write(fd, "RING3", 5) != 5)
			fail("the middle write was short");
		(void)close(fd);
		got = slurp(PATH, buf, sizeof(buf));
		if (got != (long)slen(FIRST))
			fail("an overwrite changed the length");
		else if (!same(buf + 8, "RING3", 5))
			fail("the overwritten bytes are not there");
		else if (!same(buf, FIRST, 8))
			fail("an overwrite disturbed the bytes before it");
		else
			printf("filewrite: PASS an overwrite at offset 8 "
			    "changed 5 bytes and nothing else\n");
	}

	/* 4. append: at the end, wherever the cursor was. */
	fd = open(PATH, O_WRONLY | O_APPEND);
	if (fd < 0)
		fail("cannot reopen for appending");
	else {
		(void)lseek(fd, 0, SEEK_SET);	/* O_APPEND must ignore this */
		if (write(fd, SECOND, slen(SECOND)) != (long)slen(SECOND))
			fail("the append was short");
		(void)close(fd);
		got = slurp(PATH, buf, sizeof(buf));
		if (got != (long)(slen(FIRST) + slen(SECOND)))
			fail("the appended file is the wrong length");
		else if (!same(buf + slen(FIRST), SECOND, slen(SECOND)))
			fail("the appended bytes are not at the end");
		else
			printf("filewrite: PASS an append landed at the end, "
			    "%ld bytes now\n", got);
	}

	/* 5. O_TRUNC, at open time and not at first write. */
	fd = open(PATH, O_WRONLY | O_TRUNC);
	if (fd < 0)
		fail("cannot reopen to truncate");
	else {
		(void)close(fd);
		got = slurp(PATH, buf, sizeof(buf));
		if (got != 0)
			fail("O_TRUNC left bytes behind");
		else
			printf("filewrite: PASS O_TRUNC emptied the file "
			    "without anything being written\n");
	}

	/* 6. and the name goes. */
	if (unlink(PATH) != 0)
		fail("unlink refused");
	else if (open(PATH, O_RDONLY) >= 0)
		fail("the file is still there after unlink");
	else
		printf("filewrite: PASS unlink removed %s\n", PATH);

	/* 7. A built-in lives in the kernel's text, not on the volume. */
	fd = open("/bin/gcat", O_WRONLY);
	if (fd >= 0) {
		fail("a built-in accepted an open for writing");
		(void)close(fd);
	} else
		printf("filewrite: PASS /bin/gcat is still read-only\n");

	/*
	 * 8. A directory.  Clear an earlier run's leftovers first: the name
	 * inside, then the directory.
	 */
	(void)unlink(DIRFILE);
	(void)rmdir(DIR);
	if (mkdir(DIR, 0755) != 0) {
		fail("mkdir would not make " DIR);
	} else if (mkdir(DIR, 0755) == 0 || *__error() != EEXIST) {
		fail("mkdir made " DIR " a second time");
	} else {
		printf("filewrite: PASS mkdir made %s, and would not make it "
		    "again\n", DIR);

		/*
		 * 9. A name inside it: open(O_CREAT) can only put one there
		 * if what reached the disk is a directory.
		 */
		fd = open(DIRFILE, O_WRONLY | O_CREAT | O_TRUNC, 0666);
		if (fd < 0) {
			fail("nothing can be made inside a directory ring 3 "
			    "just made");
		} else {
			(void)write(fd, FIRST, slen(FIRST));
			(void)close(fd);
			if (rmdir(DIR) == 0 || *__error() != ENOTEMPTY)
				fail("rmdir removed a directory that was "
				    "still holding a name");
			else if (unlink(DIRFILE) != 0)
				fail("cannot unlink the file inside " DIR);
			else if (rmdir(DIR) != 0)
				fail("rmdir would not remove an emptied " DIR);
			else if (open(DIRFILE, O_RDONLY) >= 0)
				fail("the directory is gone and a path "
				    "through it still opens");
			else
				printf("filewrite: PASS %s took a name, "
				    "refused to go while it held one, and "
				    "went when it did not\n", DIR);
		}
	}

	/* 10. and the two removals are not each other. */
	if (rmdir(HELD) == 0 || *__error() != ENOTDIR)
		fail("rmdir accepted " HELD ", which is a file");
	else if (unlink("/etc") == 0 || *__error() != EISDIR)
		fail("unlink accepted /etc, which is a directory");
	else
		printf("filewrite: PASS rmdir refuses a file and unlink "
		    "refuses a directory\n");

	/*
	 * 11. A name that moves, and its bytes with it.  The file's length
	 * lives in the same packed record as its name, so a rename to a name
	 * of another length rewrites the record; reading the file afterwards
	 * shows the length survived.
	 */
	(void)unlink(MOVED);
	fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		fail("cannot make a file to move");
	else {
		put = write(fd, FIRST, slen(FIRST));
		(void)close(fd);
		if (put != (long)slen(FIRST))
			fail("the write before the move was short");
		else if (rename(PATH, MOVED) != 0)
			fail("rename refused " PATH " -> " MOVED);
		else if (open(PATH, O_RDONLY) >= 0)
			fail(PATH " still opens after being renamed away");
		else {
			got = slurp(MOVED, buf, sizeof(buf));
			if (got != (long)slen(FIRST) ||
			    !same(buf, FIRST, slen(FIRST)))
				fail("the bytes did not cross the rename");
			else
				printf("filewrite: PASS rename moved %s to %s "
				    "with all %u of its bytes\n", PATH, MOVED,
				    (unsigned)slen(FIRST));
		}
	}

	/*
	 * 12. Onto a name that is taken: write a new file beside the old one
	 * and rename it over.  Every open of the name must then get the
	 * newcomer, while a descriptor held on the old file across the move
	 * still reads it to the last byte.
	 */
	held = open(MOVED, O_RDONLY);
	if (held < 0)
		fail("cannot hold the occupant open before the takeover");
	else {
		fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
		put = fd < 0 ? -1 : write(fd, SECOND, slen(SECOND));
		if (fd >= 0)
			(void)close(fd);
		if (put != (long)slen(SECOND))
			fail("cannot make the newcomer for the takeover");
		else if (rename(PATH, MOVED) != 0)
			fail("rename refused a destination that is taken");
		else if (open(PATH, O_RDONLY) >= 0)
			fail(PATH " still opens after moving onto " MOVED);
		else {
			got = slurp(MOVED, buf, sizeof(buf));
			if (got != (long)slen(SECOND) ||
			    !same(buf, SECOND, slen(SECOND)))
				fail("the taken name did not answer with the "
				    "newcomer's bytes");
			else {
				got = read(held, buf, sizeof(buf));
				if (got != (long)slen(FIRST) ||
				    !same(buf, FIRST, slen(FIRST)))
					fail("the holder did not keep the "
					    "file it opened");
				else
					printf("filewrite: PASS rename took "
					    "%s while a holder read the old "
					    "file's %u byte(s) to the end\n",
					    MOVED, (unsigned)slen(FIRST));
			}
		}
		(void)close(held);
	}

	/* and the refusals that stay refusals. */
	if (rename("/etc/nothing-of-that-name", MOVED) == 0 ||
	    *__error() != ENOENT)
		fail("rename moved a name that is not there");
	else if (rename(MOVED, "/etc") == 0 || *__error() != EISDIR)
		fail("rename put a file over a directory");
	else
		printf("filewrite: PASS rename refuses a source that is not "
		    "there and a directory destination\n");
	(void)unlink(MOVED);

	/*
	 * 13. A file that outlives its name: it lives until its last name and
	 * its last descriptor are gone.  Read after the unlink, from a
	 * non-zero offset, so bytes buffered at open time cannot pass for the
	 * volume still answering.
	 */
	fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		fail("cannot make the file to unlink while open");
	else {
		put = write(fd, FIRST, slen(FIRST));
		(void)close(fd);
		if (put != (long)slen(FIRST))
			fail("the write before the unlink was short");
	}
	fd = open(PATH, O_RDONLY);
	if (fd < 0)
		fail("cannot open the file to unlink");
	else {
		if (unlink(PATH) != 0)
			fail("unlink refused a file that is open");
		else if (open(PATH, O_RDONLY) >= 0)
			fail(PATH " still opens after being unlinked");
		else if (lseek(fd, 8, SEEK_SET) != 8)
			fail("lseek would not move in an unlinked file");
		else {
			got = read(fd, buf, sizeof(buf));
			if (got != (long)slen(FIRST) - 8 ||
			    !same(buf, &FIRST[8], (size_t)got))
				fail("the bytes of an unlinked file are gone "
				    "while it is still open");
			else
				printf("filewrite: PASS %s read %ld byte(s) "
				    "through a descriptor after its only name "
				    "was unlinked\n", PATH, got);
		}
		(void)close(fd);
		/*
		 * Now it is gone.  An O_EXCL create of the name is the check:
		 * a file left unreaped after the last close could still hold
		 * the name, which neither open nor stat would show.
		 */
		fd = open(PATH, O_WRONLY | O_CREAT | O_EXCL, 0644);
		if (fd < 0)
			fail("the name did not come free after the last close");
		else {
			(void)close(fd);
			(void)unlink(PATH);
			printf("filewrite: PASS the name came free once the "
			    "last descriptor on it closed\n");
		}
	}

	/*
	 * 14. fsync.  The kernel batches checkpoints: write(2) returning means
	 * the edit is complete and ordered, fsync(2) returning 0 means it is
	 * published on disk rather than parked in the open batch.  Only the
	 * answer is checkable from here (the kernel's torn-write stand checks
	 * the disk), plus the refusal of a closed descriptor.
	 */
	fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		fail("cannot make a file to fsync");
	else {
		put = write(fd, FIRST, slen(FIRST));
		if (put != (long)slen(FIRST))
			fail("the write before the fsync was short");
		else if (fsync(fd) != 0)
			fail("fsync refused a file descriptor");
		else
			printf("filewrite: PASS fsync answered 0 -- the "
			    "write is published, not parked\n");
		(void)close(fd);
		if (fsync(fd) == 0)
			fail("fsync accepted a closed descriptor");
		(void)unlink(PATH);
	}

	/*
	 * 15. The published past, by path.  The kernel lists every checkpoint
	 * it can still read under /.xid (fs/fs.h), so the newest entry is the
	 * one fsync just made, and the file under it must read as written,
	 * from a directory that refuses every write.
	 *
	 * The window slides with every checkpoint, and the syncer may publish
	 * one between the listing and the read: ESTALE then is reported as
	 * skipped, not failed.
	 */
	fd = open(PATH, O_WRONLY | O_CREAT | O_TRUNC, 0644);
	if (fd < 0)
		fail("cannot make a file to publish");
	else {
		put = write(fd, FIRST, slen(FIRST));
		if (put != (long)slen(FIRST) || fsync(fd) != 0)
			fail("the write or the fsync before the view failed");
		(void)close(fd);
		best = 0;
		dp = opendir("/.xid");
		if (dp == NULL)
			fail("/.xid would not open as a directory");
		else {
			while ((ent = readdir(dp)) != NULL) {
				xid = 0;
				for (i = 0; ent->d_name[i] >= '0' &&
				    ent->d_name[i] <= '9'; i++)
					xid = xid * 10 + (unsigned long)
					    (ent->d_name[i] - '0');
				if (i == 0 || ent->d_name[i] != '\0' ||
				    ent->d_type != 4)
					fail("/.xid lists something that is "
					    "not a numbered directory");
				else if (xid > best)
					best = xid;
			}
			(void)closedir(dp);
		}
		if (best == 0)
			fail("/.xid lists no checkpoint at all");
		else {
			n = 0;
			for (i = 0; "/.xid/"[i] != '\0'; i++)
				vpath[n++] = "/.xid/"[i];
			n += putnum(vpath + n, best);
			for (i = 0; PATH[i] != '\0'; i++)
				vpath[n++] = PATH[i];
			vpath[n] = '\0';

			got = slurp(vpath, buf, sizeof(buf));
			if (got < 0 && *__error() == ESTALE)
				printf("filewrite: checkpoint %lu slid out of "
				    "the window before it could be read -- "
				    "the window being honest; skipped\n", best);
			else if (got != (long)slen(FIRST) ||
			    !same(buf, FIRST, (size_t)got))
				fail("the file under the newest /.xid entry "
				    "does not read as what fsync published");
			else
				printf("filewrite: PASS %s read back through "
				    "%s -- the checkpoint fsync published, "
				    "by number\n", PATH, vpath);

			fd = open(vpath, O_WRONLY);
			if (fd >= 0) {
				(void)close(fd);
				fail("open(O_WRONLY) under /.xid handed out a "
				    "descriptor");
			} else if (*__error() != EROFS)
				fail("open(O_WRONLY) under /.xid refused with "
				    "something other than EROFS");
			else if (unlink(vpath) == 0 || *__error() != EROFS)
				fail("unlink under /.xid did not answer EROFS");
			else if (mkdir("/.xid/ring3", 0755) == 0 ||
			    *__error() != EROFS)
				fail("mkdir under /.xid did not answer EROFS");
			else
				printf("filewrite: PASS the past is read-only "
				    "-- open for writing, unlink and mkdir "
				    "under /.xid all answered EROFS\n");
		}
		(void)unlink(PATH);
	}

	/*
	 * 16. Two descriptors, one file.  What one writes past the end the
	 * other must see: read(2) and pread(2) reach the new bytes,
	 * lseek(SEEK_END) lands on the new end, and O_APPEND appends after
	 * them, not over them.  A descriptor that knew only the length it
	 * was opened at would stop, or write, at the old end.
	 */
	fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
	held = open(PATH, O_RDWR | O_APPEND);
	if (fd < 0 || held < 0)
		fail("cannot open one file twice");
	else if (write(fd, "0123456789", 10) != 10)
		fail("the first descriptor's write was short");
	else if (lseek(held, 0, SEEK_END) != 10)
		fail("lseek(SEEK_END) through a second descriptor missed "
		    "what the first wrote");
	else if (lseek(held, 0, SEEK_SET) != 0 ||
	    read(held, buf, sizeof(buf)) != 10)
		fail("read(2) through a second descriptor stopped at the "
		    "old end");
	else if (pread(held, buf, 4, 8) != 2 || !same(buf, "89", 2))
		fail("pread(2) through a second descriptor stopped at the "
		    "old end");
	else if (write(held, "ab", 2) != 2 ||
	    slurp(PATH, buf, sizeof(buf)) != 12 ||
	    !same(buf, "0123456789ab", 12))
		fail("O_APPEND through a second descriptor did not land "
		    "after the first's bytes");
	else
		printf("filewrite: PASS a second descriptor sees the file "
		    "grow -- read, pread, lseek(SEEK_END) and O_APPEND all "
		    "reach the first's bytes\n");
	if (fd >= 0)
		(void)close(fd);
	if (held >= 0)
		(void)close(held);
	(void)unlink(PATH);

	/* 17. Setting the times. */
	{
		struct timeval	tv[2];
		struct timeval	now;
		struct stat	st;

		tv[0].tv_sec  = 1000000000;
		tv[0].tv_usec = 500000;
		tv[0].tv_pad  = 0;
		tv[1].tv_sec  = 1234567890;
		tv[1].tv_usec = 250000;
		tv[1].tv_pad  = 0;
		fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
		if (fd < 0 || write(fd, "x", 1) != 1)
			fail("cannot make a file to set the times of");
		else if (utimes(PATH, tv) != 0 || stat(PATH, &st) != 0)
			fail("utimes(2), or the stat after it, was refused");
		else if (st.st_atime != 1000000000 ||
		    st.st_atimensec != 500000000 ||
		    st.st_mtime != 1234567890 ||
		    st.st_mtimensec != 250000000)
			fail("stat did not read back the times utimes set");
		else if (gettimeofday(&now, NULL) != 0 ||
		    futimes(fd, NULL) != 0 || stat(PATH, &st) != 0)
			fail("futimes(2) with no times was refused");
		else if (st.st_mtime < now.tv_sec - 5 ||
		    st.st_mtime > now.tv_sec + 5 ||
		    st.st_atime != st.st_mtime ||
		    st.st_atimensec != st.st_mtimensec)
			fail("futimes(2) with no times did not set both to "
			    "now");
		else if (utimes("/etc/no-such-file", tv) != -1 ||
		    *__error() != ENOENT)
			fail("utimes(2) on a missing name was not ENOENT");
		else
			printf("filewrite: PASS utimes sets both times to the "
			    "microsecond and stat reads them back; futimes "
			    "with none sets both to now\n");
		if (fd >= 0)
			(void)close(fd);
		(void)unlink(PATH);
	}

	/*
	 * 18. Past the end.  The gap runs from byte 10 across the block edge
	 * at 4096 to 5000.  Then the file is cut to 4, inside its first block,
	 * and grown back to 10: those six bytes were "EFGHIJ" and are still in
	 * the block's slack, but what reads back must be zeros.
	 */
	fd = open(PATH, O_RDWR | O_CREAT | O_TRUNC, 0644);
	if (fd < 0 || write(fd, "ABCDEFGHIJ", 10) != 10)
		fail("cannot make a file to write past the end of");
	else if (lseek(fd, 5000, SEEK_SET) != 5000 ||
	    read(fd, buf, sizeof(buf)) != 0)
		fail("lseek past the end was refused, or a read there found "
		    "bytes");
	else if (write(fd, "xy", 2) != 2 || lseek(fd, 0, SEEK_END) != 5002)
		fail("a write past the end did not land there");
	else if (pread(fd, buf, sizeof(buf), 10) != (long)sizeof(buf) ||
	    !zeros(buf, sizeof(buf)) ||
	    pread(fd, buf, sizeof(buf), 3900) != (long)sizeof(buf) ||
	    !zeros(buf, sizeof(buf)) ||
	    pread(fd, buf, 16, 4990) != 12 || !zeros(buf, 10) ||
	    !same(buf + 10, "xy", 2))
		fail("the gap a write past the end left is not zeros");
	else if (ftruncate(fd, 4) != 0 || ftruncate(fd, 10) != 0 ||
	    pread(fd, buf, 16, 0) != 10 || !same(buf, "ABCD", 4) ||
	    !zeros(buf + 4, 6))
		fail("bytes cut off by ftruncate came back when the file grew");
	else
		printf("filewrite: PASS a write past the end leaves zeros "
		    "behind it, across a block edge, and cut bytes grown back "
		    "over read as zeros\n");
	if (fd >= 0)
		(void)close(fd);
	(void)unlink(PATH);

	if (fails != 0) {
		printf("filewrite: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("filewrite: done -- every check passed\n");
	exit(0);
	return (0);
}
