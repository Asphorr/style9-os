/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * dirlist -- a self-authored Darwin-ABI probe for the directory-enumeration
 * rung of the VFS.  It is NOT a real Apple binary; it is the "small probe
 * compiled with the real toolchain" that de-risks opendir/readdir/stat before
 * a genuine binary (tree(1)) depends on them.  Built by clang/ld64.lld for the
 * macOS target and bound by our dyld against our libSystem, it imports the
 * SAME $INODE64 symbol names a real binary would -- so a clean run proves the
 * export naming and the struct-dirent round-trip, not just our own glue.
 *
 * Freestanding (-fno-builtin, no SDK headers): the macOS struct dirent layout
 * and the imported prototypes are declared here exactly as <dirent.h> would
 * alias them, and resolved from /usr/lib/libSystem.B.dylib at link.  Entry is
 * _entry (ld -e), so no crt is required; relinked low like dyldhello.
 */

typedef __UINT8_TYPE__	uint8_t;
typedef __UINT16_TYPE__	uint16_t;
typedef __UINT32_TYPE__	uint32_t;
typedef __UINT64_TYPE__	uint64_t;
typedef __INT64_TYPE__	int64_t;

#define	NULL	((void *)0)
#define	DT_DIR	4

struct dirent {
	uint64_t	d_ino;
	uint64_t	d_seekoff;
	uint16_t	d_reclen;
	uint16_t	d_namlen;
	uint8_t		d_type;
	char		d_name[1024];
};

typedef struct __dirstream	DIR;

extern DIR		*opendir(const char *path) __asm__("_opendir$INODE64");
extern struct dirent	*readdir(DIR *dp) __asm__("_readdir$INODE64");
extern int		 closedir(DIR *dp);
extern int		 stat(const char *path, void *buf) __asm__("_stat$INODE64");
extern int		 printf(const char *fmt, ...);
extern void		 exit(int code);

static int
streq(const char *a, const char *b)
{
	int	i;

	for (i = 0; ; i++) {
		if (a[i] != b[i])
			return (0);
		if (a[i] == '\0')
			return (1);
	}
}

/* List one directory, recursing into subdirectories up to two levels deep. */
static void
list(const char *path, int depth)
{
	DIR		*d;
	struct dirent	*e;
	int		 i;

	d = opendir(path);
	if (d == NULL) {
		printf("  opendir(%s) -> NULL\n", path);
		return;
	}
	while ((e = readdir(d)) != NULL) {
		for (i = 0; i < depth; i++)
			printf("  ");
		printf("    %s%s\n", e->d_name,
		    e->d_type == DT_DIR ? "/" : "");
		if (e->d_type == DT_DIR && depth < 2 &&
		    !streq(e->d_name, ".") && !streq(e->d_name, "..")) {
			char	child[512];
			int	a, b;

			for (a = 0; a < 500 && path[a] != '\0'; a++)
				child[a] = path[a];
			if (a > 0 && child[a - 1] != '/')
				child[a++] = '/';
			for (b = 0; a < 511 && e->d_name[b] != '\0'; b++)
				child[a++] = e->d_name[b];
			child[a] = '\0';
			list(child, depth + 1);
		}
	}
	closedir(d);
}

int
entry(void)
{
	unsigned char	sb[144];

	printf("dirlist: walking the FAT volume via opendir/readdir + stat\n");
	list("/", 0);
	if (stat("/fonts", sb) == 0)
		printf("dirlist: stat(/fonts) -> mode=0%o size=%lld ino=%llu\n",
		    (unsigned)*(uint16_t *)(sb + 4),
		    (long long)*(int64_t *)(sb + 96),
		    (unsigned long long)*(uint64_t *)(sb + 8));
	printf("dirlist: done\n");
	exit(0);
	return (0);
}
