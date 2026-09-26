/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_MEMMAP_H_
#define	_SYS_MEMMAP_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Bootloader-independent view of physical memory.
 *
 * memmap_init() parses the boot info (mb1 / mb2 / PVH, chosen by the boot
 * magic), normalises every entry to the local types, and stores them
 * sorted by base in memmap_entries[].  pmm and others use only this view.
 *
 * Fixed-size because it is filled before any allocator exists;
 * MEMMAP_MAX_ENTRIES is plenty (firmware reports under a dozen).
 */

enum {
	MEMMAP_FREE		= 1,	/* RAM available to the kernel    */
	MEMMAP_RESERVED		= 2,	/* firmware / hole / unusable     */
	MEMMAP_ACPI		= 3,	/* ACPI reclaimable               */
	MEMMAP_NVS		= 4,	/* ACPI non-volatile storage      */
	MEMMAP_BADRAM		= 5,	/* known-bad RAM                  */
};

struct memmap_entry {
	uint64_t	me_base;
	uint64_t	me_length;
	uint32_t	me_type;
	uint32_t	me_reserved;
};

#define	MEMMAP_MAX_ENTRIES	64

extern struct memmap_entry	memmap_entries[];
extern size_t			memmap_nentries;
extern uint64_t			memmap_total_bytes;
extern uint64_t			memmap_free_bytes;
extern uint64_t			memmap_max_pa;

void		 memmap_init(uint32_t mb_magic, uint32_t mb_info);
void		 memmap_print(void);
const char	*memmap_type_name(uint32_t type);

#endif /* !_SYS_MEMMAP_H_ */
