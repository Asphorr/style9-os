/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Linker script for ring-3 ELF64 programs.  Lays out a single PT_LOAD
 * with .text + .rodata followed by .data + .bss at the start of the
 * page-mapped user image, anchored at 0x40000000 -- just past the
 * kernel's 1 GiB boot identity map, so the kernel can hand out
 * 4 KiB user pages there without splitting any huge page.
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

	/DISCARD/ : { *(.note.*) *(.comment) *(.eh_frame) }
}
