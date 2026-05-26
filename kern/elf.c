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
 * Bring one PT_LOAD into the target task's address space:
 *	- round [p_vaddr, p_vaddr+p_memsz) out to whole pages,
 *	- allocate + map each page U=1 with R / W / X from p_flags,
 *	- zero the freshly-allocated frame (covers BSS),
 *	- copy p_filesz bytes from `image + p_offset` to the segment
 *	  start via the kernel-VA alias of the underlying frame
 *	  (works even when p_flags & PF_W is 0 -- the alias is RW
 *	  through the boot identity map, the user leaf stays RO).
 */
static int
load_segment(struct task *target, const uint8_t *image, size_t image_size,
    const struct elf64_phdr *ph)
{
	uint64_t	va, va_start, va_end;
	uint64_t	pa;
	uint64_t	src_off;
	uint64_t	remaining;
	uint64_t	cur_va;
	uint64_t	page_off;
	uint64_t	chunk;
	uint8_t		*kva;
	uint32_t	prot;
	size_t		i;

	if (ph->p_memsz < ph->p_filesz)
		return (ELF_E_BADSEG);
	if (ph->p_offset + ph->p_filesz > image_size)
		return (ELF_E_TRUNCATED);
	if (ph->p_vaddr + ph->p_memsz < ph->p_vaddr)
		return (ELF_E_BADSEG);

	prot = VM_PROT_READ | VM_PROT_USER;
	if (ph->p_flags & PF_W)
		prot |= VM_PROT_WRITE;
	if (ph->p_flags & PF_X)
		prot |= VM_PROT_EXEC;

	va_start = ph->p_vaddr & ~(uint64_t)PAGE_MASK;
	va_end   = (ph->p_vaddr + ph->p_memsz + PAGE_MASK) &
	    ~(uint64_t)PAGE_MASK;

	for (va = va_start; va < va_end; va += PAGE_SIZE) {
		pa = pmm_alloc_page();
		if (pa == PA_INVALID)
			return (ELF_E_NOMEM);

		kva = (uint8_t *)pmm_kva_from_pa(pa);
		for (i = 0; i < PAGE_SIZE; i++)
			kva[i] = 0;

		if (!pmap_enter(target->t_pmap, va, pa, prot))
			return (ELF_E_MAP);
	}

	/*
	 * Record the whole segment as one entry in the task's vm_map.
	 * vme_prot carries pmap-style bits (R/W/X plus USER); vme_flags
	 * only tracks backing semantics (ANON, future COW), since "user
	 * accessibility" is already in the prot byte and a second flag
	 * would just drift.
	 */
	if (!vm_map_enter(target->t_map, va_start, va_end - va_start,
	    (uint8_t)prot, VME_F_ANON))
		return (ELF_E_MAP);

	src_off   = ph->p_offset;
	remaining = ph->p_filesz;
	cur_va    = ph->p_vaddr;

	while (remaining > 0) {
		page_off = cur_va & PAGE_MASK;
		chunk    = PAGE_SIZE - page_off;
		if (chunk > remaining)
			chunk = remaining;

		pa = pmap_extract(target->t_pmap, cur_va & ~(uint64_t)PAGE_MASK);
		if (pa == PA_INVALID)
			return (ELF_E_MAP);
		kva = (uint8_t *)pmm_kva_from_pa(pa & ~(uint64_t)PAGE_MASK);

		for (i = 0; i < chunk; i++)
			kva[page_off + i] = image[src_off + i];

		src_off   += chunk;
		cur_va    += chunk;
		remaining -= chunk;
	}

	return (ELF_E_OK);
}
