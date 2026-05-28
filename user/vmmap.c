/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * vmmap -- dump the calling task's VM regions.
 *
 * Userspace counterpart to Darwin's `vmmap(1)`: prints one line per
 * live entry in the task's vm_map showing VA range, size, protection
 * bits, backing kind, and a guessed "region type" label.  The kernel
 * surface is the SYS_TASK_GET_VM_REGIONS syscall, which copies an
 * array of mach_vm_region_entry records back to the caller.
 *
 * Like lsmp(1), v1 only introspects self.  Useful as a debugger probe
 * + a demo of the layered (vm_map -> per-page pmap) memory model: the
 * tool shows the higher-level intent (region kinds, protections)
 * without having to walk the per-task PML4.
 */

#include "style9.h"

/*
 * Map a VA range to a coarse region label by checking against the
 * well-known layout the loader + launcher build for a fresh task:
 *	USER_VA_LO    = 0x40000000  -- ELF .text starts here
 *	USER_STACK_VA = 0x4000F000  -- single-page user stack the launcher maps
 *	heap / vm_allocate-anon lands in vm_map_find_space holes above
 * The classifier is heuristic (no formal section tagging in the
 * vm_map entry yet), so labels are guidance, not authoritative.
 */
#define	USER_VA_LO_HINT		0x40000000ULL
#define	USER_STACK_VA_HINT	0x4000F000ULL

static const char *
region_label(const struct mach_vm_region_entry *e)
{
	uint64_t	size;

	size = e->mvr_end - e->mvr_start;

	/*
	 * Single-page mapping at the well-known stack VA.
	 */
	if (e->mvr_start == USER_STACK_VA_HINT && size == 0x1000ULL)
		return ("STACK");

	/*
	 * Image: starts at USER_VA_LO, executable, anonymous in our
	 * loader (elf_load currently copies segments into freshly
	 * pmm_alloc'd frames marked VME_F_ANON).
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
 * Stand up a couple of extra anonymous regions before the snapshot so
 * the table demonstrates more than just the fixed TEXT + STACK pair
 * every task gets at launch.  Failures are non-fatal: snapshot still
 * shows whatever state was actually reached.
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
