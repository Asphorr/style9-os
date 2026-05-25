/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kprintf.h"
#include "memmap.h"
#include "multiboot.h"
#include "panic.h"
#include "pvh.h"

struct memmap_entry	memmap_entries[MEMMAP_MAX_ENTRIES];
size_t			memmap_nentries;
uint64_t		memmap_total_bytes;
uint64_t		memmap_free_bytes;
uint64_t		memmap_max_pa;

static void	memmap_add(uint64_t base, uint64_t length, uint32_t type);
static void	memmap_sort(void);
static void	memmap_recount(void);
static uint32_t	e820_to_local(uint32_t e820_type);
static void	parse_mb1(uint32_t info);
static void	parse_mb2(uint32_t info);
static void	parse_pvh(uint32_t info);

void
memmap_init(uint32_t mb_magic, uint32_t mb_info)
{

	memmap_nentries    = 0;
	memmap_total_bytes = 0;
	memmap_free_bytes  = 0;
	memmap_max_pa      = 0;

	switch (mb_magic) {
	case MB1_BOOTLOADER_MAGIC:
		parse_mb1(mb_info);
		break;
	case MB2_BOOTLOADER_MAGIC:
		parse_mb2(mb_info);
		break;
	case 0:
		parse_pvh(mb_info);
		break;
	default:
		panic("memmap_init: unknown boot magic 0x%08x", mb_magic);
	}

	if (memmap_nentries == 0) {
		/*
		 * No firmware-supplied map.  Synthesise a conservative
		 * stub: low 1 MiB reserved, then 31 MiB usable (the
		 * absolute minimum any modern QEMU VM will hand us).
		 */
		memmap_add(0, 0x100000, MEMMAP_RESERVED);
		memmap_add(0x100000, 0x1F00000, MEMMAP_FREE);
	}

	memmap_sort();
	memmap_recount();
}

const char *
memmap_type_name(uint32_t type)
{

	switch (type) {
	case MEMMAP_FREE:	return ("free");
	case MEMMAP_RESERVED:	return ("reserved");
	case MEMMAP_ACPI:	return ("acpi");
	case MEMMAP_NVS:	return ("nvs");
	case MEMMAP_BADRAM:	return ("bad");
	default:		return ("?");
	}
}

void
memmap_print(void)
{
	size_t		i;
	uint64_t	end;

	kprintf("memory map (%zu entries, max_pa=0x%llx):\n",
	    memmap_nentries, (unsigned long long)memmap_max_pa);

	for (i = 0; i < memmap_nentries; i++) {
		end = memmap_entries[i].me_base + memmap_entries[i].me_length;
		kprintf("  [%2zu] 0x%016llx .. 0x%016llx  %-8s (%llu KiB)\n",
		    i,
		    (unsigned long long)memmap_entries[i].me_base,
		    (unsigned long long)end,
		    memmap_type_name(memmap_entries[i].me_type),
		    (unsigned long long)(memmap_entries[i].me_length >> 10));
	}

	kprintf("  total RAM (any type): %llu KiB\n",
	    (unsigned long long)(memmap_total_bytes >> 10));
	kprintf("  usable RAM:           %llu KiB\n",
	    (unsigned long long)(memmap_free_bytes >> 10));
}

static void
memmap_add(uint64_t base, uint64_t length, uint32_t type)
{
	uint64_t	end;

	if (length == 0)
		return;

	if (memmap_nentries >= MEMMAP_MAX_ENTRIES) {
		kprintf("memmap: dropping entry @ 0x%llx (table full)\n",
		    (unsigned long long)base);
		return;
	}

	memmap_entries[memmap_nentries].me_base     = base;
	memmap_entries[memmap_nentries].me_length   = length;
	memmap_entries[memmap_nentries].me_type     = type;
	memmap_entries[memmap_nentries].me_reserved = 0;
	memmap_nentries++;

	end = base + length;
	if (end > memmap_max_pa)
		memmap_max_pa = end;
}

/*
 * Insertion sort: input is small (under 64 entries), the only thing
 * that matters is producing a stable ordering for pmm.  Avoid pulling
 * in a generic sort.
 */
static void
memmap_sort(void)
{
	size_t			i, j;
	struct memmap_entry	tmp;

	for (i = 1; i < memmap_nentries; i++) {
		tmp = memmap_entries[i];
		j = i;
		while (j > 0 &&
		    memmap_entries[j - 1].me_base > tmp.me_base) {
			memmap_entries[j] = memmap_entries[j - 1];
			j--;
		}
		memmap_entries[j] = tmp;
	}
}

static void
memmap_recount(void)
{
	size_t	i;

	memmap_total_bytes = 0;
	memmap_free_bytes  = 0;

	for (i = 0; i < memmap_nentries; i++) {
		memmap_total_bytes += memmap_entries[i].me_length;
		if (memmap_entries[i].me_type == MEMMAP_FREE)
			memmap_free_bytes += memmap_entries[i].me_length;
	}
}

static uint32_t
e820_to_local(uint32_t e820_type)
{

	switch (e820_type) {
	case MB_E820_RAM:	return (MEMMAP_FREE);
	case MB_E820_RESERVED:	return (MEMMAP_RESERVED);
	case MB_E820_ACPI:	return (MEMMAP_ACPI);
	case MB_E820_NVS:	return (MEMMAP_NVS);
	case MB_E820_BADRAM:	return (MEMMAP_BADRAM);
	default:		return (MEMMAP_RESERVED);
	}
}

static void
parse_mb1(uint32_t info_pa)
{
	const struct mb1_info		*info;
	const struct mb1_mmap_entry	*e;
	uintptr_t			cur, end;

	info = (const struct mb1_info *)(uintptr_t)info_pa;

	if ((info->mb_flags & MB1_INFO_MEMMAP) != 0) {
		cur = info->mb_mmap_addr;
		end = (uintptr_t)info->mb_mmap_addr +
		    info->mb_mmap_length;

		while (cur < end) {
			e = (const struct mb1_mmap_entry *)cur;
			memmap_add(e->mme_base, e->mme_length,
			    e820_to_local(e->mme_type));
			/*
			 * mme_size is "size of this record minus this
			 * field"; the field itself is 4 bytes.  Step by
			 * that to hit the next record header.
			 */
			cur += e->mme_size + sizeof(uint32_t);
		}
	} else if ((info->mb_flags & MB1_INFO_MEMORY) != 0) {
		memmap_add(0, 0x100000, MEMMAP_RESERVED);
		memmap_add(0x100000,
		    (uint64_t)info->mb_mem_upper * 1024,
		    MEMMAP_FREE);
	}
}

static void
parse_mb2(uint32_t info_pa)
{
	const struct mb2_info		*info;
	const struct mb2_tag		*tag;
	const struct mb2_tag_mmap	*mm;
	const struct mb2_mmap_entry	*e;
	const uint8_t			*p, *end;
	size_t				n, i;

	info = (const struct mb2_info *)(uintptr_t)info_pa;
	p    = (const uint8_t *)(info + 1);
	end  = (const uint8_t *)info + info->mb_total_size;

	while (p + sizeof(*tag) <= end) {
		tag = (const struct mb2_tag *)p;

		if (tag->mt_type == MB2_TAG_END)
			break;

		if (tag->mt_type == MB2_TAG_MMAP) {
			mm = (const struct mb2_tag_mmap *)tag;
			if (mm->mt_entry_size == 0)
				break;
			n = (mm->mt_size - sizeof(*mm)) / mm->mt_entry_size;
			e = (const struct mb2_mmap_entry *)(mm + 1);

			for (i = 0; i < n; i++) {
				memmap_add(e->mme_base, e->mme_length,
				    e820_to_local(e->mme_type));
				e = (const struct mb2_mmap_entry *)
				    ((const uint8_t *)e + mm->mt_entry_size);
			}
		}

		/* Tags are 8-byte aligned. */
		p += (tag->mt_size + 7u) & ~7u;
	}
}

static void
parse_pvh(uint32_t info_pa)
{
	const struct pvh_start_info	*si;
	const struct pvh_memmap_entry	*e;
	uint32_t			i;

	si = (const struct pvh_start_info *)(uintptr_t)info_pa;
	if (si->psi_magic != PVH_START_MAGIC) {
		kprintf("memmap: PVH magic mismatch (0x%08x)\n",
		    si->psi_magic);
		return;
	}

	e = (const struct pvh_memmap_entry *)(uintptr_t)si->psi_memmap_paddr;
	for (i = 0; i < si->psi_memmap_entries; i++) {
		memmap_add(e[i].pme_base, e[i].pme_length,
		    e820_to_local(e[i].pme_type));
	}
}
