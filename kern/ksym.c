/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "ksym.h"

struct ksym_entry {
	uint64_t	ks_addr;
	const char	*ks_name;
};

/*
 * Table populated by tools/gen_ksyms.sh.  Sorted by ks_addr; the
 * sentinel is an entry with ks_addr == 0 (and ks_name == NULL).
 *
 * If the build is in its first link pass the section is the
 * pre-linker stub with no real entries -- ksym_lookup just returns
 * NULL and the autopsy falls back to bare hex.
 */
extern const struct ksym_entry	__ksymtab_start[];
extern const struct ksym_entry	__ksymtab_end[];

const char *
ksym_lookup(uint64_t addr, uint64_t *offset_out)
{
	const struct ksym_entry	*e;
	const struct ksym_entry	*best;

	/*
	 * The sentinel is an entry with ks_name == NULL (gen_ksyms.sh
	 * emits a final .quad 0 / .quad 0 pair).  ks_addr == 0 alone is
	 * not a sentinel -- the post-link stub also has addr=0 entries
	 * that should be skipped, not terminate the walk.
	 */
	best = NULL;
	for (e = __ksymtab_start; e->ks_name != NULL; e++) {
		if (e->ks_addr == 0)
			continue;
		if (e->ks_addr > addr)
			continue;
		if (best == NULL || e->ks_addr > best->ks_addr)
			best = e;
	}

	if (best == NULL) {
		if (offset_out != NULL)
			*offset_out = 0;
		return (NULL);
	}

	if (offset_out != NULL)
		*offset_out = addr - best->ks_addr;
	return (best->ks_name);
}

void
ksym_print(uint64_t addr)
{
	const char	*name;
	uint64_t	 off;

	name = ksym_lookup(addr, &off);
	if (name == NULL) {
		kprintf("0x%016lx", (unsigned long)addr);
		return;
	}
	kprintf("0x%016lx <%s+0x%lx>", (unsigned long)addr,
	    name, (unsigned long)off);
}
