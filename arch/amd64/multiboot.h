/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_MULTIBOOT_H_
#define	_MACHINE_MULTIBOOT_H_

#include <stdint.h>

/*
 * Wire formats for the boot-info structures handed to us in %ebx when
 * the bootloader transfers control.  We support multiboot1 (legacy
 * GRUB), multiboot2 (modern GRUB), and PVH (QEMU -kernel for ELF64).
 *
 * Only the fields the kernel actually consumes are decoded here -- the
 * structures are larger in the standards.  Fields are packed against
 * their on-the-wire layout; do not reorder.
 */

/* ---- Multiboot 1 -------------------------------------------------------- */

#define	MB1_BOOTLOADER_MAGIC	0x2BADB002U

#define	MB1_INFO_MEMORY		(1U << 0)
#define	MB1_INFO_MEMMAP		(1U << 6)

struct mb1_info {
	uint32_t	mb_flags;
	uint32_t	mb_mem_lower;		/* KiB below 1 MiB    */
	uint32_t	mb_mem_upper;		/* KiB above 1 MiB    */
	uint32_t	mb_boot_device;
	uint32_t	mb_cmdline;
	uint32_t	mb_mods_count;
	uint32_t	mb_mods_addr;
	uint32_t	mb_syms[4];
	uint32_t	mb_mmap_length;
	uint32_t	mb_mmap_addr;
	/* trailing fields elided */
} __attribute__((packed));

struct mb1_mmap_entry {
	uint32_t	mme_size;		/* size of THIS record minus mme_size itself */
	uint64_t	mme_base;
	uint64_t	mme_length;
	uint32_t	mme_type;
} __attribute__((packed));

/* ---- Multiboot 2 -------------------------------------------------------- */

#define	MB2_BOOTLOADER_MAGIC	0x36D76289U

#define	MB2_TAG_END		0
#define	MB2_TAG_MMAP		6

struct mb2_info {
	uint32_t	mb_total_size;
	uint32_t	mb_reserved;
	/* mb2_tag records follow, each 8-byte aligned */
} __attribute__((packed));

struct mb2_tag {
	uint32_t	mt_type;
	uint32_t	mt_size;
} __attribute__((packed));

struct mb2_tag_mmap {
	uint32_t	mt_type;
	uint32_t	mt_size;
	uint32_t	mt_entry_size;
	uint32_t	mt_entry_version;
	/* mb2_mmap_entry records follow */
} __attribute__((packed));

struct mb2_mmap_entry {
	uint64_t	mme_base;
	uint64_t	mme_length;
	uint32_t	mme_type;
	uint32_t	mme_reserved;
} __attribute__((packed));

/*
 * Both MB1 and MB2 encode E820 types: 1 = usable, 3 = ACPI reclaimable,
 * 4 = ACPI NVS, 5 = bad RAM, anything else = reserved.
 */
#define	MB_E820_RAM		1
#define	MB_E820_RESERVED	2
#define	MB_E820_ACPI		3
#define	MB_E820_NVS		4
#define	MB_E820_BADRAM		5

_Static_assert(sizeof(struct mb1_mmap_entry) == 24, "mb1 entry size");
_Static_assert(sizeof(struct mb2_mmap_entry) == 24, "mb2 entry size");

#endif /* !_MACHINE_MULTIBOOT_H_ */
