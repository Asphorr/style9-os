/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * mmaptest -- a self-authored Darwin-ABI probe for mmap(2), in the same role
 * timeprobe played for the wall clock and pipefork for fork/exec: prove the
 * syscall from ring 3 with a small binary built by the real toolchain, before
 * anything larger leans on it.
 *
 * The interesting checks are the ones that are about demand paging rather than
 * about mmap's return value:
 *
 *	- a mapping far larger than the memory in the machine still succeeds,
 *	  because nothing is allocated until it is touched;
 *
 *	- a page the KERNEL writes first (read(2) into a fresh mapping) is
 *	  filled just like one the program writes.  That fault arrives from
 *	  ring 0 rather than ring 3, on the same page, and before demand
 *	  paging it could only have been a bug;
 *
 *	- a file mapping's bytes are the file's bytes -- compared against what
 *	  read(2) returns for the same offsets, including an offset far enough
 *	  in to need a page the first fault did not bring;
 *
 *	- the tail of the last page of a file mapping reads as zero, which is
 *	  the one promise mmap makes that a file cannot keep by itself.
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
 * Anonymous memory: it must arrive zeroed, hold what is written to it, and be
 * returnable.  The 64 KiB here is deliberately more than one page, so a write
 * at the far end proves a second fault was serviced and not just the first.
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
 * The point of laziness: ask for far more than the machine has.  This map is
 * 64 MiB on a machine with 127 MiB of RAM, most of it already spoken for, and
 * it succeeds because none of it exists yet.  Touching three pages should cost
 * three frames -- the kernel's fault counters are where that is visible; from
 * here all that can be checked is that it works at all.
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
 * A page whose first toucher is the kernel.  read(2) copies into a mapping
 * this program has never written, so the fault comes from ring 0 in the middle
 * of a syscall rather than from the instruction stream.  Nothing else in the
 * system exercises that path, and it is the one that turns "mmap works" into
 * "mmap'd memory is memory".
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
 * The file mapping.  Compared against read(2) at three offsets: the very
 * start, a point deep enough to need a page no earlier fault brought in, and
 * the last bytes of the file.  Then the tail of the final page, which belongs
 * to no part of the file and has to read as zero.
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
	 * One page more than the file needs, deliberately.  A mapping longer
	 * than the thing it maps is legal, and every byte past the end has to
	 * read as zero -- including a whole page that no part of the file
	 * reaches, which is the case a file whose length happens to be a
	 * multiple of the page size would otherwise never test.
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

	/* Past end-of-file, inside the last page: zero, by promise. */
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
	 * Half a mapping.  The kernel cannot split an entry, so this must be
	 * refused outright -- freeing the frames and leaving the entry behind
	 * would hand the page allocator memory the map still promises.
	 */
	p = mmap(NULL, 8192, PROT_READ | PROT_WRITE, MAP_ANON | MAP_PRIVATE,
	    -1, 0);
	if (p == MAP_FAILED)
		bad("mmap for the partial-munmap test failed");
	else {
		if (munmap(p, 4096) == 0)
			bad("munmap of half a mapping succeeded");
		if (munmap(p, 8192) != 0)
			bad("munmap of the whole mapping failed");
	}
	printf("mmaptest: MAP_FIXED, zero length, stray and partial munmap "
	    "refused\n");
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
	} else
		printf("mmaptest: no test file on this volume -- "
		    "file mapping not checked\n");

	printf("mmaptest: %s\n", fail ? "FAILED" : "all checks passed");
	exit(fail);
	return (0);
}
