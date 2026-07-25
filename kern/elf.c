/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "elf.h"
#include "kprintf.h"
#include "pmap.h"
#include "pmm.h"
#include "task.h"
#include "vm.h"

static int	load_segment(struct task *target, const uint8_t *image,
		    size_t image_size, const struct elf64_phdr *ph);

int
elf_load(struct task *target, const void *image, size_t image_size,
    uint64_t *entry_out)
{
	const struct elf64_ehdr		*eh;
	const struct elf64_phdr		*ph;
	const uint8_t			*bytes;
	uint16_t			 i;
	int				 rv;

	if (target == NULL || image == NULL || entry_out == NULL)
		return (ELF_E_TRUNCATED);
	if (image_size < sizeof(*eh))
		return (ELF_E_TRUNCATED);

	bytes = (const uint8_t *)image;
	eh    = (const struct elf64_ehdr *)image;

	if (eh->e_ident[0] != ELF_MAG0 || eh->e_ident[1] != ELF_MAG1 ||
	    eh->e_ident[2] != ELF_MAG2 || eh->e_ident[3] != ELF_MAG3)
		return (ELF_E_BADMAG);
	if (eh->e_ident[4] != ELFCLASS64)
		return (ELF_E_BADCLASS);
	if (eh->e_ident[5] != ELFDATA2LSB)
		return (ELF_E_BADCLASS);
	if (eh->e_type != ET_EXEC)
		return (ELF_E_BADTYPE);
	if (eh->e_machine != EM_X86_64)
		return (ELF_E_BADMACH);
	if (eh->e_phentsize != sizeof(struct elf64_phdr))
		return (ELF_E_BADSEG);
	if (eh->e_phoff + (uint64_t)eh->e_phnum *
	    sizeof(struct elf64_phdr) > image_size)
		return (ELF_E_TRUNCATED);

	for (i = 0; i < eh->e_phnum; i++) {
		ph = (const struct elf64_phdr *)
		    (bytes + eh->e_phoff + i * sizeof(struct elf64_phdr));
		if (ph->p_type != PT_LOAD)
			continue;

		rv = load_segment(target, bytes, image_size, ph);
		if (rv != ELF_E_OK)
			return (rv);
	}

	*entry_out = eh->e_entry;
	return (ELF_E_OK);
}

/*
 * Bring one PT_LOAD into the target task's address space.  Validation and
 * the p_flags -> VM_PROT_* translation happen here, because they are what
 * this container format says; the mapping itself is vm_map_image, which is
 * the same code kern/macho.c reaches for an LC_SEGMENT_64.  The two formats
 * disagree about how a segment is described and agree completely about what
 * one is, so that is where the line is drawn.
 */
static int
load_segment(struct task *target, const uint8_t *image, size_t image_size,
    const struct elf64_phdr *ph)
{
	uint32_t	prot;

	if (ph->p_memsz < ph->p_filesz)
		return (ELF_E_BADSEG);
	if (ph->p_offset + ph->p_filesz > image_size)
		return (ELF_E_TRUNCATED);
	if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr)
		return (ELF_E_BADSEG);

	/*
	 * vme_prot carries pmap-style bits (R/W/X plus USER); vme_flags only
	 * tracks backing semantics (ANON, COW), since "user accessibility" is
	 * already in the prot byte and a second flag would just drift.
	 */
	prot = VM_PROT_READ | VM_PROT_USER;
	if (ph->p_flags & PF_W)
		prot |= VM_PROT_WRITE;
	if (ph->p_flags & PF_X)
		prot |= VM_PROT_EXEC;

	switch (vm_map_image(target->t_map, target->t_pmap, ph->p_vaddr,
	    ph->p_memsz, ph->p_filesz, image + ph->p_offset, (uint8_t)prot)) {
	case VM_IMAGE_OK:
		return (ELF_E_OK);
	case VM_IMAGE_NOMEM:
		return (ELF_E_NOMEM);
	default:
		return (ELF_E_MAP);
	}
}
