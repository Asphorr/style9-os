/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

/*
 * Boot entry stub (PVH or multiboot2) and the 32-to-64 transition.
 *
 * The loader hands us 32-bit protected mode with:
 *	eax = boot magic: MB2_BOOTLOADER_MAGIC under multiboot2; memmap_init
 *	      takes 0 to mean PVH
 *	ebx = phys ptr to the multiboot2 info or the hvm_start_info
 *	cs/ds/es/ss = flat 32-bit selectors
 *	cr0.PE = 1, cr0.PG = 0, cr4.PAE = 0
 *
 * We:
 *   1.  Stash eax / ebx in .data so we can clobber GPRs freely.
 *   2.  Build a 3-level page-table tree (PML4 -> PDPT -> PD) that
 *	 identity-maps the low 1 GiB via 2 MiB huge pages.
 *   3.  Enable CR4.PAE, set IA32_EFER.LME and NXE, enable CR0.PG and
 *	 CR0.WP (now in compatibility mode).
 *   4.  lgdt the temporary 64-bit GDT, far-jump to entry64 in the
 *	 64-bit code segment (this is the real long-mode transition).
 *   5.  In 64-bit: reload data segs, set up the stack, pass the stashed
 *	 values to kmain in rdi/rsi.  Halt forever if it returns.
 */

/*
 * Boot headers.
 *
 * QEMU's -kernel loader takes an ELF64 kernel only through the PVH "Xen"
 * ELF note (type 18, XEN_ELFNOTE_PHYS32_ENTRY), which points at _start,
 * still .code32.  The multiboot2 header lets the same ELF boot under mb2
 * loaders such as GRUB.
 */

	.set MB2_MAGIC, 0xE85250D6
	.set MB2_ARCH,  0			/* i386 protected mode */

	.section .multiboot, "a"
	.align	8
mb2_header:
	.long	MB2_MAGIC
	.long	MB2_ARCH
	.long	mb2_header_end - mb2_header
	.long	-(MB2_MAGIC + MB2_ARCH + (mb2_header_end - mb2_header))

	.align	8
	.word	0
	.word	0
	.long	8
mb2_header_end:

/* PVH/Xen ELF note: phys32 entry point at _start (4-byte address). */
	.section .note.Xen, "a", @note
	.align	4
	.long	4				/* namesz: "Xen\0" */
	.long	4				/* descsz: 32-bit entry */
	.long	18				/* XEN_ELFNOTE_PHYS32_ENTRY */
	.ascii	"Xen\0"
	.align	4
	.long	_start

/*
 * Boot-time page tables and stack.  Live in .bss so they are zero
 * initialised; we only need to fill the entries we actually use.
 */
	.section .bss, "aw", @nobits
	.align	4096
	.globl	boot_pml4
boot_pml4:
	.skip	4096
boot_pdpt:
	.skip	4096
boot_pd:
	.skip	4096

	.align	16
boot_stack_bottom:
	.skip	0x4000				/* 16 KiB boot stack */
boot_stack_top:

/*
 * Temporary boot GDT.
 *	0x00  null
 *	0x08  kernel code, L=1 (64-bit)
 *	0x10  kernel data
 */
	.section .data
	.align	16
boot_gdt:
	.quad	0
	.quad	0x00AF9A000000FFFF		/* code64: P, DPL0, S, L=1 */
	.quad	0x00CF92000000FFFF		/* data: P, DPL0, S, G, D=1 */
boot_gdt_end:

	.align	16
boot_gdtr:
	.word	boot_gdt_end - boot_gdt - 1
	.quad	boot_gdt

/* Stash for mb_magic / mb_info; populated in 32-bit, read in 64-bit. */
	.align	4
mb_magic_saved:
	.long	0
mb_info_saved:
	.long	0

/* ---------------------------------------------------------------------- *
 * 32-bit entry (PVH or multiboot2).					  *
 * ---------------------------------------------------------------------- */

	.section .text
	.code32
	.globl	_start
	.type	_start, @function
_start:
	/* Stash boot args before we start clobbering registers. */
	movl	%eax, mb_magic_saved
	movl	%ebx, mb_info_saved

	/* Provisional stack so the next routines may push if they wish. */
	movl	$boot_stack_top, %esp

	/* --- Page-table tree; every entry PRESENT | WRITE. --- */

	/* PML4[0] = boot_pdpt | flags */
	movl	$boot_pdpt, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pml4
	movl	$0, boot_pml4 + 4

	/* PDPT[0] = boot_pd | flags */
	movl	$boot_pd, %eax
	orl	$0x03, %eax
	movl	%eax, boot_pdpt
	movl	$0, boot_pdpt + 4

	/*
	 * PD[i] = (i * 2 MiB) | PRESENT | WRITE | PS  for i in 0..511,
	 * covering 0 .. 1 GiB with 2 MiB huge pages.
	 */
	movl	$boot_pd, %edi
	movl	$0x00000083, %eax		/* phys=0, flags = P|W|PS */
	movl	$512, %ecx
1:
	movl	%eax, (%edi)
	movl	$0, 4(%edi)
	addl	$0x00200000, %eax		/* next 2 MiB */
	addl	$8, %edi
	loop	1b

	/* --- Enable PAE + LME + PG, then load GDT and far-jump. --- */

	movl	$boot_pml4, %eax
	movl	%eax, %cr3

	movl	%cr4, %eax
	orl	$0x20, %eax			/* CR4.PAE */
	movl	%eax, %cr4

	movl	$0xC0000080, %ecx		/* IA32_EFER */
	rdmsr
	orl	$0x100, %eax			/* EFER.LME */
	orl	$0x800, %eax			/* EFER.NXE -- else NX (PTE   */
						/* bit 63) is a reserved bit  */
	wrmsr

	movl	%cr0, %eax
	orl	$0x80000000, %eax		/* CR0.PG */
	orl	$0x00010000, %eax		/* CR0.WP -- ring 0 obeys the */
						/* read-only bit too, which   */
						/* copy-on-write relies on.   */
	movl	%eax, %cr0

	lgdt	boot_gdtr

	ljmp	$0x08, $entry64
	.size	_start, . - _start

/* ---------------------------------------------------------------------- *
 * 64-bit entry, reached via the far-jump above.			  *
 * ---------------------------------------------------------------------- */

	.code64
	.globl	entry64
	.type	entry64, @function
entry64:
	/* Reload data segments with the new GDT's data selector. */
	movw	$0x10, %ax
	movw	%ax, %ds
	movw	%ax, %es
	movw	%ax, %fs
	movw	%ax, %gs
	movw	%ax, %ss

	/* Long-mode-clean stack. */
	movq	$boot_stack_top, %rsp
	xorq	%rbp, %rbp

	/* Move boot args into SysV-AMD64 arg registers (rdi, rsi). */
	movl	mb_magic_saved, %edi		/* zero-extends to rdi */
	movl	mb_info_saved,  %esi

	cld
	call	kmain

	/* kmain must not return; lock the CPU if it does. */
	cli
1:	hlt
	jmp	1b
	.size	entry64, . - entry64
