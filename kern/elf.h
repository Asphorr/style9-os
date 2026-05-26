/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_ELF_H_
#define	_SYS_ELF_H_

#include <stddef.h>
#include <stdint.h>

/*
 * Minimal static-ELF64 loader for ring-3 programs.
 *
 * The ABI is the subset real ld emits for a "freestanding -static" link:
 *	ELFCLASS64, ELFDATA2LSB, ET_EXEC, EM_X86_64,
 *	one or more PT_LOAD segments giving file -> VA mappings,
 *	e_entry naming the start address.
 *
 * elf_load() parses an image already resident in kernel memory, drops
 * the PT_LOAD segments into the live page-table at their requested VAs
 * (all U=1, with R/W/X mirroring p_flags), and returns the entry RIP.
 *
 * Single-task only for now; per-task pmaps land alongside SMP.
 */

#define	ELF_MAG0		0x7Fu
#define	ELF_MAG1		'E'
#define	ELF_MAG2		'L'
#define	ELF_MAG3		'F'

#define	ELFCLASS64		2
#define	ELFDATA2LSB		1
#define	EM_X86_64		62
#define	ET_EXEC			2

#define	PT_LOAD			1

#define	PF_X			(1u << 0)
#define	PF_W			(1u << 1)
#define	PF_R			(1u << 2)

struct elf64_ehdr {
	uint8_t		e_ident[16];
	uint16_t	e_type;
	uint16_t	e_machine;
	uint32_t	e_version;
	uint64_t	e_entry;
	uint64_t	e_phoff;
	uint64_t	e_shoff;
	uint32_t	e_flags;
	uint16_t	e_ehsize;
	uint16_t	e_phentsize;
	uint16_t	e_phnum;
	uint16_t	e_shentsize;
	uint16_t	e_shnum;
	uint16_t	e_shstrndx;
};

struct elf64_phdr {
	uint32_t	p_type;
	uint32_t	p_flags;
	uint64_t	p_offset;
	uint64_t	p_vaddr;
	uint64_t	p_paddr;
	uint64_t	p_filesz;
	uint64_t	p_memsz;
	uint64_t	p_align;
};

_Static_assert(sizeof(struct elf64_ehdr) == 64, "ehdr must be 64 bytes");
_Static_assert(sizeof(struct elf64_phdr) == 56, "phdr must be 56 bytes");

#define	ELF_E_OK		0
#define	ELF_E_TRUNCATED		(-1)
#define	ELF_E_BADMAG		(-2)
#define	ELF_E_BADCLASS		(-3)
#define	ELF_E_BADTYPE		(-4)
#define	ELF_E_BADMACH		(-5)
#define	ELF_E_NOMEM		(-6)
#define	ELF_E_MAP		(-7)
#define	ELF_E_BADSEG		(-8)

int	elf_load(const void *image, size_t image_size, uint64_t *entry_out);

#endif /* !_SYS_ELF_H_ */
