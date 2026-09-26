/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * dirlist -- a self-authored Darwin-ABI probe for directory enumeration:
 * not an Apple binary, but a small one built by the real toolchain to
 * prove opendir/readdir/stat before a genuine binary (tree(1)) relies on
 * them.  Built by clang/ld64.lld for macOS and bound by our dyld against
 * our libSystem, it imports the same $INODE64 names a real binary would,
 * so a clean run proves the export naming and the struct dirent layout.
 *
 * Freestanding (-fno-builtin, no SDK headers): the macOS struct dirent and
 * the prototypes are declared here as <dirent.h> would alias them.  Entry
 * is _entry (ld -e), so no crt; relinked low like dyldhello.
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

/*
 * stat() the first real subdirectory of the root, whichever volume is
 * mounted.  Taking the name from the listing, not a fixed path, proves
 * readdir and stat agree about the same directory.
 */
static void
stat_first_subdir(void)
{
	unsigned char	 sb[144];
	char		 path[512];
	struct dirent	*e;
	DIR		*d;
	int		 i;

	d = opendir("/");
	if (d == NULL)
		return;
	while ((e = readdir(d)) != NULL) {
		if (e->d_type != DT_DIR ||
		    streq(e->d_name, ".") || streq(e->d_name, ".."))
			continue;
		path[0] = '/';
		for (i = 0; i < 500 && e->d_name[i] != '\0'; i++)
			path[i + 1] = e->d_name[i];
		path[i + 1] = '\0';
		if (stat(path, sb) == 0)
			printf("dirlist: stat(%s) -> mode=0%o size=%lld "
			    "ino=%llu\n", path,
			    (unsigned)*(uint16_t *)(sb + 4),
			    (long long)*(int64_t *)(sb + 96),
			    (unsigned long long)*(uint64_t *)(sb + 8));
		break;
	}
	closedir(d);
}

int
entry(void)
{

	printf("dirlist: walking the volume via opendir/readdir + stat\n");
	list("/", 0);
	stat_first_subdir();
	printf("dirlist: done\n");
	exit(0);
	return (0);
}
