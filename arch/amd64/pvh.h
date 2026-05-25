/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_PVH_H_
#define	_MACHINE_PVH_H_

#include <stdint.h>

/*
 * Xen PVH boot ABI -- used by QEMU's ELF64 -kernel loader when the
 * image carries an XEN_ELFNOTE_PHYS32_ENTRY note (see boot.S).
 *
 * On entry from the loader %ebx points at a hvm_start_info structure
 * placed somewhere below 4 GiB.  All embedded pointers are physical
 * and also below 4 GiB; we read them via the identity map.
 */

#define	PVH_START_MAGIC		0x336EC578U	/* HVM_START_MAGIC_VALUE */

struct pvh_start_info {
	uint32_t	psi_magic;
	uint32_t	psi_version;
	uint32_t	psi_flags;
	uint32_t	psi_nr_modules;
	uint64_t	psi_modlist_paddr;
	uint64_t	psi_cmdline_paddr;
	uint64_t	psi_rsdp_paddr;
	uint64_t	psi_memmap_paddr;
	uint32_t	psi_memmap_entries;
	uint32_t	psi_reserved;
} __attribute__((packed));

struct pvh_memmap_entry {
	uint64_t	pme_base;
	uint64_t	pme_length;
	uint32_t	pme_type;		/* E820 codes; see multiboot.h */
	uint32_t	pme_reserved;
} __attribute__((packed));

_Static_assert(sizeof(struct pvh_start_info) == 56, "pvh start_info size");
_Static_assert(sizeof(struct pvh_memmap_entry) == 24, "pvh mmap entry size");

#endif /* !_MACHINE_PVH_H_ */
