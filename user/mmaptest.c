/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * mmaptest -- a self-authored Darwin-ABI probe for mmap(2), as timeprobe
 * is for the clock and pipefork for fork/exec: a small binary built by the
 * real toolchain proves the syscall before anything larger leans on it.
 *
 * The checks that matter are about demand paging:
 *
 *	- a mapping far larger than the machine's memory succeeds, since
 *	  nothing is allocated until touched;
 *	- a page the kernel writes first (read(2) into a fresh mapping) is
 *	  filled like one the program writes; that fault comes from ring 0;
 *	- a file mapping's bytes equal what read(2) returns at the same
 *	  offsets, including one that needs a page the first fault did not
 *	  bring;
 *	- the tail of a file mapping's last page reads as zero;
 *	- unmapping a page from the middle of a file mapping leaves both
 *	  sides mapped and showing their own part of the file, which tests
 *	  splitting a map entry and the offset arithmetic of the split.
 */

typedef __UINT8_TYPE__		uint8_t;
typedef __UINT32_TYPE__		uint32_t;
typedef __UINT64_TYPE__		uint64_t;
typedef __SIZE_TYPE__		size_t;

#define	NULL			((void *)0)

#define	PROT_READ		0x01
#define	PROT_WRITE		0x02
#define	MAP_PRIVATE		0x0002
#define	MAP_FIXED		0x0010
#define	MAP_ANON		0x1000
#define	MAP_FAILED		((void *)-1)

#define	O_RDONLY		0

extern void	*mmap(void *addr, size_t len, int prot, int flags, int fd,
		    long off);
extern int	 munmap(void *addr, size_t len);
extern int	 open(const char *path, int flags, ...);
extern long	 read(int fd, void *buf, unsigned long n);
extern long	 lseek(int fd, long off, int whence);
extern int	 close(int fd);
extern int	 printf(const char *fmt, ...);
extern void	 exit(int code);

/* Files to look for, in order.  The first list entry is the APFS volume's. */
static const char *const candidates[] = {
	"/var/db/big.txt",
	"/STANDARD.FLF",
	"/standard.flf",
	NULL
};

static int	fail;

static void
bad(const char *what)
{

	printf("mmaptest: FAIL %s\n", what);
	fail = 1;
}

/*
 * Anonymous memory arrives zeroed, holds what is written, and can be
 * returned.  64 KiB spans many pages, so more than the first fault is
 * serviced.
 */
static void
test_anon(void)
{
	uint8_t		*p;
	size_t		 len;
	size_t		 i;

	len = 64u * 1024u;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (p == MAP_FAILED) {
		bad("anonymous mmap returned MAP_FAILED");
		return;
	}
	for (i = 0; i < len; i++) {
		if (p[i] != 0) {
			bad("anonymous memory did not arrive zeroed");
			break;
		}
	}
	for (i = 0; i < len; i++)
		p[i] = (uint8_t)(i * 7u + 3u);
	for (i = 0; i < len; i++) {
		if (p[i] != (uint8_t)(i * 7u + 3u)) {
			bad("anonymous memory did not keep what was written");
			break;
		}
	}
	if (munmap(p, len) != 0)
		bad("munmap of an anonymous mapping failed");
	printf("mmaptest: anonymous 64 KiB -- zeroed, writable, returned\n");
}

/*
 * Ask for more than is free: 64 MiB on a 128 MiB guest with most of it in
 * use succeeds because none of it exists yet.  Touching three pages should
 * cost three frames; only the kernel's fault counters can show that, so
 * here we check that it works.
 */
static void
test_lazy(void)
{
	uint8_t		*p;
	size_t		 len;

	len = 64u * 1024u * 1024u;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (p == MAP_FAILED) {
		bad("64 MiB lazy mapping was refused");
		return;
	}
	p[0]                 = 0x11;
	p[4096u * 1000u]     = 0x22;
	p[len - 1u]          = 0x33;
	if (p[0] != 0x11 || p[4096u * 1000u] != 0x22 || p[len - 1u] != 0x33)
		bad("a touched page of the big mapping did not hold its byte");
	if (munmap(p, len) != 0)
		bad("munmap of the big mapping failed");
	printf("mmaptest: 64 MiB reserved, 3 pages touched, released\n");
}

/*
 * A page first touched by the kernel: read(2) into an unwritten mapping
 * faults from ring 0 in the middle of a syscall, a path nothing else here
 * exercises.
 */
static void
test_kernel_writes(const char *path)
{
	uint8_t		*p;
	uint8_t		 direct[512];
	size_t		 len;
	size_t		 i;
	long		 n;
	int		 fd;

	len = 32u * 1024u;
	p = mmap(NULL, len, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (p == MAP_FAILED) {
		bad("mmap for the kernel-write test failed");
		return;
	}

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		bad("could not open the test file for the kernel-write test");
		(void)munmap(p, len);
		return;
	}
	/* Read straight into untouched pages, well past the first. */
	n = read(fd, p + 8192, 512);
	if (n != 512) {
		bad("read into an untouched mapping came up short");
		(void)close(fd);
		(void)munmap(p, len);
		return;
	}
	if (lseek(fd, 0, 0) != 0)
		bad("lseek back to the start of the test file failed");
	if (read(fd, direct, sizeof(direct)) != 512)
		bad("re-reading the test file into the stack failed");
	(void)close(fd);

	for (i = 0; i < sizeof(direct); i++) {
		if (p[8192 + i] != direct[i]) {
			bad("the kernel's write landed in the wrong bytes");
			break;
		}
	}
	if (munmap(p, len) != 0)
		bad("munmap after the kernel-write test failed");
	printf("mmaptest: read(2) filled a page the program never touched\n");
}

/*
 * The file mapping, compared with read(2) at the start, at a page no
 * earlier fault brought in, and at the last byte; then everything past
 * EOF must read as zero.
 */
static void
test_file(const char *path)
{
	uint8_t		*p;
	uint8_t		 direct[256];
	uint64_t	 size;
	uint64_t	 span;
	uint64_t	 deep;
	size_t		 i;
	int		 fd;

	fd = open(path, O_RDONLY);
	if (fd < 0) {
		bad("could not open the test file");
		return;
	}
	size = (uint64_t)lseek(fd, 0, 2);		/* SEEK_END */
	if ((long)size <= 0) {
		bad("the test file has no length");
		(void)close(fd);
		return;
	}

	/*
	 * One page more than the file needs, so a whole page lies past EOF
	 * even when the file length is a page multiple; it must read as zero.
	 */
	span = ((size + 0xFFFu) & ~(uint64_t)0xFFFu) + 4096u;
	p = mmap(NULL, span, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		bad("file mmap returned MAP_FAILED");
		(void)close(fd);
		return;
	}
	printf("mmaptest: mapped %s (%llu bytes) at %p\n", path,
	    (unsigned long long)size, (void *)p);

	/* Start of the file. */
	if (lseek(fd, 0, 0) != 0 || read(fd, direct, sizeof(direct)) !=
	    (long)sizeof(direct))
		bad("could not read the head of the test file");
	else {
		for (i = 0; i < sizeof(direct); i++) {
			if (p[i] != direct[i]) {
				bad("the mapping's first page is not the file");
				break;
			}
		}
	}

	/* A page the first fault did not bring in. */
	deep = (size > 3u * 4096u) ? (size / 2u) & ~(uint64_t)0xFFu : 0;
	if (deep + sizeof(direct) <= size) {
		if (lseek(fd, (long)deep, 0) != (long)deep ||
		    read(fd, direct, sizeof(direct)) != (long)sizeof(direct))
			bad("could not read the middle of the test file");
		else {
			for (i = 0; i < sizeof(direct); i++) {
				if (p[deep + i] != direct[i]) {
					bad("a later page of the mapping is "
					    "not the file");
					break;
				}
			}
		}
		printf("mmaptest: byte %llu matches through a second fault\n",
		    (unsigned long long)deep);
	}

	/* The very last byte of the file. */
	if (lseek(fd, (long)(size - 1u), 0) == (long)(size - 1u) &&
	    read(fd, direct, 1) == 1) {
		if (p[size - 1u] != direct[0])
			bad("the mapping's last file byte is wrong");
	} else
		bad("could not read the tail of the test file");

	/* Past end-of-file: zero. */
	if (span > size) {
		for (i = (size_t)size; i < (size_t)span; i++) {
			if (p[i] != 0) {
				bad("the tail past end-of-file is not zero");
				break;
			}
		}
		printf("mmaptest: %llu bytes past EOF read as zero\n",
		    (unsigned long long)(span - size));
	}

	if (munmap(p, span) != 0)
		bad("munmap of the file mapping failed");
	(void)close(fd);
}

/* The refusals.  A mapping this kernel cannot honour must say so. */
static void
test_refusals(void)
{
	void	*p;

	p = mmap((void *)0x50000000UL, 4096, PROT_READ | PROT_WRITE,
	    MAP_ANON | MAP_PRIVATE | MAP_FIXED, -1, 0);
	if (p != MAP_FAILED) {
		bad("MAP_FIXED was accepted");
		(void)munmap(p, 4096);
	}
	if (mmap(NULL, 0, PROT_READ, MAP_ANON | MAP_PRIVATE, -1, 0) !=
	    MAP_FAILED)
		bad("a zero-length mapping was accepted");
	if (munmap((void *)0x41234000UL, 4096) == 0)
		bad("munmap of a range nobody mapped succeeded");

	/*
	 * Unmapping part of a mapping is allowed; a range with a hole is
	 * not.  Unmap the front page, then ask for both.
	 */
	p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (p == MAP_FAILED)
		bad("mmap for the partial-munmap test failed");
	else {
		if (munmap(p, 4096) != 0)
			bad("munmap of half a mapping failed");
		if (munmap(p, 8192) == 0)
			bad("munmap across a hole succeeded");
		if (munmap((char *)p + 4096, 4096) != 0)
			bad("munmap of the surviving half failed");
	}
	printf("mmaptest: MAP_FIXED, zero length, stray munmap and munmap "
	    "across a hole refused\n");
}

/*
 * Cutting a file mapping in two.  The half above the cut starts further
 * into the file by the distance the cut moved; a kernel that drops that
 * offset shows the wrong part of the file.  Only a page not faulted in
 * before the cut can reveal it, so: map, touch nothing, cut, then look.
 */
static void
test_cut(const char *path)
{
	uint8_t		*p;
	uint8_t		 head[64];
	uint8_t		 direct[64];
	uint64_t	 size;
	uint64_t	 span;
	uint64_t	 above;
	uint64_t	 below;
	size_t		 i;
	int		 fd;
	int		 differs;
	int		 before;

	before = fail;
	fd = open(path, O_RDONLY);
	if (fd < 0) {
		bad("could not open the test file for the cut test");
		return;
	}
	size = (uint64_t)lseek(fd, 0, 2);		/* SEEK_END */
	span = size & ~(uint64_t)0xFFFu;		/* whole pages only */
	if (span < 5u * 4096u) {
		printf("mmaptest: test file too small to cut, skipped\n");
		(void)close(fd);
		return;
	}

	/* What page 0 holds -- which is what a lost offset would serve up. */
	if (lseek(fd, 0, 0) != 0 ||
	    read(fd, head, sizeof(head)) != (long)sizeof(head)) {
		bad("could not read the head of the test file");
		(void)close(fd);
		return;
	}

	below = 1u * 4096u;
	above = 3u * 4096u;

	/* What page 3 holds, read straight from the file for comparison. */
	if (lseek(fd, (long)above, 0) != (long)above ||
	    read(fd, direct, sizeof(direct)) != (long)sizeof(direct)) {
		bad("could not read page three of the test file");
		(void)close(fd);
		return;
	}

	/*
	 * A lost offset is only visible if the file differs at the two
	 * places; otherwise skip rather than pass vacuously.
	 */
	differs = 0;
	for (i = 0; i < sizeof(head); i++) {
		if (head[i] != direct[i]) {
			differs = 1;
			break;
		}
	}
	if (!differs) {
		printf("mmaptest: the test file repeats itself; the cut test "
		    "would prove nothing, skipped\n");
		(void)close(fd);
		return;
	}

	p = mmap(NULL, span, PROT_READ, MAP_PRIVATE, fd, 0);
	if (p == MAP_FAILED) {
		bad("mmap for the cut test returned MAP_FAILED");
		(void)close(fd);
		return;
	}

	/* Page 2, out of the middle.  Nothing in this mapping has faulted. */
	if (munmap((char *)p + 2u * 4096u, 4096) != 0) {
		bad("munmap of a page in the middle of a mapping failed");
		(void)munmap(p, span);
		(void)close(fd);
		return;
	}

	/* The half above the cut, faulted here for the first time. */
	for (i = 0; i < sizeof(direct); i++) {
		if (p[above + i] != direct[i]) {
			bad("the half above the cut is showing the wrong "
			    "part of the file");
			break;
		}
	}

	/* The half below it, which never moved. */
	if (lseek(fd, (long)below, 0) != (long)below ||
	    read(fd, direct, sizeof(direct)) != (long)sizeof(direct))
		bad("could not read page one of the test file");
	else {
		for (i = 0; i < sizeof(direct); i++) {
			if (p[below + i] != direct[i]) {
				bad("the half below the cut is wrong");
				break;
			}
		}
	}

	/* The cut really took the middle out: there is nothing there now. */
	if (munmap((char *)p + 2u * 4096u, 4096) == 0)
		bad("munmap of the hole the cut left succeeded");

	/* Each half is a mapping in its own right and goes on its own. */
	if (munmap(p, 2u * 4096u) != 0)
		bad("munmap of the half below the cut failed");
	if (munmap((char *)p + above, span - above) != 0)
		bad("munmap of the half above the cut failed");

	(void)close(fd);
	/* Claim success only if no check above failed. */
	if (fail == before)
		printf("mmaptest: cut a page out of the middle -- both halves "
		    "still hold their own part of the file\n");
}

int
entry(void)
{
	const char	*path;
	int		 fd;
	int		 i;

	path = NULL;
	for (i = 0; candidates[i] != NULL; i++) {
		fd = open(candidates[i], O_RDONLY);
		if (fd >= 0) {
			(void)close(fd);
			path = candidates[i];
			break;
		}
	}

	test_anon();
	test_lazy();
	test_refusals();
	if (path != NULL) {
		test_kernel_writes(path);
		test_file(path);
		test_cut(path);
	} else
		printf("mmaptest: no test file on this volume -- "
		    "file mapping not checked\n");

	printf("mmaptest: %s\n", fail ? "FAILED" : "all checks passed");
	exit(fail);
	return (0);
}
