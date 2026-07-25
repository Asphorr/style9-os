/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Linker script for ring-3 ELF64 programs.  Lays out .text, then .rodata,
 * then .data + .bss at the start of the page-mapped user image, anchored at
 * 0x40000000 -- just past the kernel's 1 GiB boot identity map, so the
 * kernel can hand out 4 KiB user pages there without splitting any huge
 * page.
 *
 * The ALIGN(0x1000) between the sections is load-bearing twice over.  It is
 * what makes ld emit three PT_LOADs with distinct permissions rather than
 * one writable-and-executable blanket -- so a program's text is genuinely
 * read-only at runtime -- and, because a segment then begins on a page
 * boundary in the file as well as in memory, it is also what lets the loader
 * map a read-only segment straight out of the kernel image instead of
 * copying it (vm/vm.h).
 */

OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)
ENTRY(_start)

SECTIONS {
	. = 0x40000000;

	.text : {
		*(.text._start)
		*(.text .text.*)
	}

	. = ALIGN(0x1000);
	.rodata : {
		*(.rodata .rodata.*)
	}

	. = ALIGN(0x1000);
	.data : {
		*(.data .data.*)
	}

	.bss : {
		*(.bss .bss.*)
		*(COMMON)
	}

	/*
	 * The initial user stack sits at USER_STACK_VA (arch/amd64/usermode.h)
	 * and the loader maps it after the image, so an image whose .bss runs
	 * past that address collides with its own stack.  When that happened
	 * the symptom was
	 *
	 *	*** kernel panic: usermode_elf_launcher: setup_image sh rv=-4
	 *
	 * midway through boot, naming neither the section that overflowed nor
	 * the address it hit.  Here it is a link error that names both.
	 *
	 * The constant is duplicated from the header because a linker script
	 * cannot include one; it is checked by this assert firing, which is
	 * the only way it can be wrong and go unnoticed.
	 */
	ASSERT(. <= 0x40FFF000,
	    "user image overruns the initial stack at USER_STACK_VA 0x40FFF000 -- shrink it or move the stack (arch/amd64/usermode.h)")

	/DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
