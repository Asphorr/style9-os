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
 * memmap_init() consumes the boot info pointer passed in via the boot
 * magic + struct address (mb1 / mb2 / PVH), normalises every entry
 * into the local enum, and stores them sorted by base address in the
 * fixed-size memmap_entries[] array.  Other subsystems (notably pmm)
 * iterate this view; they never look at the wire formats.
 *
 * The table is fixed-size on purpose: it is consumed early, before any
 * allocator exists, so heap-backed storage is not an option.  64
 * entries is plenty -- in practice firmware reports under a dozen.
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
