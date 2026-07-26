/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * filewrite -- a self-authored Darwin-ABI probe for the rung where userspace
 * can CHANGE the volume.
 *
 * Everything below this program has been provable for a while: the APFS writer
 * makes files, grows them, shortens them and removes them, and apfsck accepts
 * what it leaves.  None of it was reachable from ring 3 -- open(2) answered
 * EROFS to every write mode and write(2) on a file descriptor answered EBADF --
 * so the only thing that had ever written to the disk was the kernel's own
 * self-tests.  This is the probe that de-risks the syscalls before a genuine
 * Apple binary (dash, redirecting with `>`) depends on them.
 *
 * It is NOT a real Apple binary.  It is built by the same real toolchain, bound
 * by our dyld against our libSystem, and imports the same symbols a real one
 * would -- so a clean run proves the syscall path and not our own glue.
 *
 * WHAT IT CHECKS, and why each one is here rather than assumed:
 *
 *	1. O_CREAT makes a file that was not there, and the fd it returns is
 *	   usable.  A create that "succeeded" but left nothing behind would
 *	   still let the write below appear to work, against a handle pointing
 *	   at nothing.
 *	2. The bytes read back are the bytes written -- through a SECOND open,
 *	   not the write fd.  Reading back through the thing that wrote is how
 *	   a cache proves itself right.
 *	3. lseek + a write into the MIDDLE overwrites without changing the
 *	   length.  A writer that always appended would pass every other check
 *	   here.
 *	4. O_APPEND lands at the end no matter where the cursor was.
 *	5. O_TRUNC empties a file that had contents, at open time.
 *	6. unlink removes the name, and opening it afterwards fails.
 *	7. Writing to a built-in (/bin/gcat, which lives in the kernel's own
 *	   text) is still refused: the volume being writable does not make
 *	   everything writable.
 *
 * Freestanding: no SDK headers, prototypes declared as <fcntl.h>/<unistd.h>
 * would alias them, entry at _entry (ld -e), relinked low like dyldhello.
 */

typedef __UINT8_TYPE__	uint8_t;
typedef __UINT32_TYPE__	uint32_t;
typedef __UINT64_TYPE__	uint64_t;
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

#define	EROFS		30

extern int	*__error(void);		/* Apple's <errno.h>: errno == *__error() */
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern long	 write(int fd, const void *buf, unsigned long n);
extern int	 close(int fd);
extern long	 lseek(int fd, long off, int whence);
extern int	 unlink(const char *path);
extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);

#define	PATH		"/etc/filewrite.txt"
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

static void
fail(const char *what)
{

	printf("filewrite: FAIL %s\n", what);
	fails++;
}

/*
 * Read a whole file through a fresh descriptor.  Fresh because the point of
 * every check here is that the bytes reached the FILE, and a cursor left over
 * from the write would be answering a question about this program's memory.
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

int
entry(void)
{
	char	buf[512];
	long	got;
	long	put;
	int	fd;

	printf("filewrite: the volume, from ring 3\n");

	/* Whatever an earlier run left, so this starts from nothing. */
	(void)unlink(PATH);

	/*
	 * 1. a file that was not there.
	 *
	 * EROFS here is not a failure: this kernel boots from a FAT volume as
	 * readily as from an APFS one, and only one of them has a writer.  The
	 * answer is the volume's, so it is reported as one and the run stops.
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

	/*
	 * 7. A built-in is not on the volume: it is an image in the kernel's
	 * own text, and there is nowhere for a write to it to go.
	 */
	fd = open("/bin/gcat", O_WRONLY);
	if (fd >= 0) {
		fail("a built-in accepted an open for writing");
		(void)close(fd);
	} else
		printf("filewrite: PASS /bin/gcat is still read-only\n");

	if (fails != 0) {
		printf("filewrite: %d check(s) FAILED\n", fails);
		exit(1);
	}
	printf("filewrite: done -- every check passed\n");
	exit(0);
	return (0);
}
