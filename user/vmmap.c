/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * vmmap -- dump the calling task's VM regions.
 *
 * Counterpart to Darwin's vmmap(1): one line per vm_map entry with VA
 * range, size, protection, backing kind and a guessed region label, from
 * SYS_TASK_GET_VM_REGIONS (an array of mach_vm_region_entry).  Like
 * lsmp(1) it can only inspect itself.  It shows the vm_map layer's intent
 * without walking the task's page tables.
 */

#include "style9.h"

/*
 * Coarse region label from the layout the loader and launcher build:
 * image text at the bottom of the user window, a single-page stack, and
 * vm_allocate memory in vm_map_find_space holes.  vm_map entries carry no
 * section tag, so the labels are guesses.  The kernel's USER_STACK_VA is
 * 0x40FFF000 (arch/amd64/usermode.h); the stack hint below does not
 * match it.
 */
#define	USER_VA_LO_HINT		0x40000000ULL
#define	USER_STACK_VA_HINT	0x4000F000ULL

static const char *
region_label(const struct mach_vm_region_entry *e)
{
	uint64_t	size;

	size = e->mvr_end - e->mvr_start;

	/* Single-page mapping at the stack VA. */
	if (e->mvr_start == USER_STACK_VA_HINT && size == 0x1000ULL)
		return ("STACK");

	/*
	 * Image: executable at USER_VA_LO.  elf_load copies segments into
	 * fresh frames (vm_map_image), so they show as anonymous.
	 */
	if (e->mvr_start == USER_VA_LO_HINT &&
	    (e->mvr_prot & VM_PROT_EXEC) != 0)
		return ("TEXT");

	if ((e->mvr_prot & VM_PROT_EXEC) != 0)
		return ("EXEC");

	if ((e->mvr_flags & VME_F_ANON) != 0) {
		if ((e->mvr_prot & VM_PROT_WRITE) != 0)
			return ("ANON_RW");
		return ("ANON_R");
	}
	return ("?");
}

static void
fmt_prot(char out[6], uint8_t prot)
{

	out[0] = (prot & VM_PROT_READ)  ? 'r' : '-';
	out[1] = (prot & VM_PROT_WRITE) ? 'w' : '-';
	out[2] = (prot & VM_PROT_EXEC)  ? 'x' : '-';
	out[3] = '/';
	out[4] = '-';	/* placeholder for max-prot once we track it */
	out[5] = '\0';
}

static void
fmt_flags(char out[4], uint8_t flags)
{

	out[0] = (flags & VME_F_ANON) ? 'A' : '-';
	out[1] = (flags & VME_F_COW)  ? 'C' : '-';
	out[2] = '-';
	out[3] = '\0';
}

static void
print_header(void)
{

	printf("  %-10s  %-17s %-17s  %8s  %5s  %3s  %s\n",
	    "region", "start", "end", "size", "prot", "flg", "details");
	printf("  ----------  ----------------- ----------------- "
	    " --------  -----  ---  -------\n");
}

static void
print_row(const struct mach_vm_region_entry *e)
{
	char	prot[6];
	char	flags[4];

	fmt_prot(prot, e->mvr_prot);
	fmt_flags(flags, e->mvr_flags);
	printf("  %-10s  %016llx- %016llx  %6llu K  %5s  %3s\n",
	    region_label(e),
	    (unsigned long long)e->mvr_start,
	    (unsigned long long)e->mvr_end,
	    (unsigned long long)((e->mvr_end - e->mvr_start) >> 10),
	    prot, flags);
}

/*
 * Add two anonymous regions so the table shows more than the image and
 * stack every task starts with.  Failure is harmless: the snapshot shows
 * whatever exists.
 */
static void
seed_demo_state(void)
{
	void	*p4;
	void	*p64;

	p4  = vm_allocate(0x1000,  VM_PROT_READ | VM_PROT_WRITE);
	p64 = vm_allocate(0x10000, VM_PROT_READ | VM_PROT_WRITE);
	(void)p4;
	(void)p64;
}

int
main(void)
{
	struct mach_vm_region_entry	entries[MACH_VM_REGION_MAX];
	long				n;
	long				i;
	uint64_t			total;

	seed_demo_state();

	n = task_get_vm_regions(0, entries, MACH_VM_REGION_MAX);
	if (n < 0) {
		printf("vmmap: SYS_TASK_GET_VM_REGIONS failed (rv=%ld)\n", n);
		return (1);
	}

	printf("vmmap: %ld live region%s in calling task's vm_map\n",
	    n, n == 1 ? "" : "s");
	print_header();
	total = 0;
	for (i = 0; i < n; i++) {
		print_row(&entries[i]);
		total += entries[i].mvr_end - entries[i].mvr_start;
	}
	printf("  ----------\n  TOTAL                                              "
	    " %6llu K\n", (unsigned long long)(total >> 10));
	return (0);
}
