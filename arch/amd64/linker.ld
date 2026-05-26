/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Hobby OS kernel linker script (ELF64, x86_64).
 *
 * Sections are routed into explicit PHDRS so QEMU's PVH loader can
 * find the .note.Xen section in a PT_NOTE program header.  Without
 * that note QEMU's ELF64 -kernel path bails with "Error loading
 * uncompressed kernel without PVH ELF Note".
 */

ENTRY(_start)
OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

PHDRS
{
	text   PT_LOAD FLAGS(5);	/* R+X  */
	rodata PT_LOAD FLAGS(4);	/* R    */
	data   PT_LOAD FLAGS(6);	/* R+W  */
	note   PT_NOTE FLAGS(0);
}

SECTIONS
{
	. = 1M;
	__kernel_start = .;

	.multiboot ALIGN(8) : {
		KEEP(*(.multiboot))
	} : text

	.note.Xen ALIGN(4) : {
		KEEP(*(.note.Xen*))
	} : note : text

	.text ALIGN(4K) : {
		*(.text*)
	} : text

	.rodata ALIGN(4K) : {
		*(.rodata*)
	} : rodata

	.data ALIGN(4K) : {
		*(.data*)
	} : data

	/*
	 * Kernel symbol table.  Populated by a post-link rebuild
	 * (tools/gen_ksyms.sh + Makefile two-pass), kept after .data
	 * and before .bss so its growth between passes shifts only
	 * the BSS image -- function and rodata addresses extracted
	 * in pass 1 stay correct in pass 2.
	 */
	.ksymtab ALIGN(4K) : {
		__ksymtab_start = .;
		KEEP(*(.ksymtab))
		__ksymtab_end = .;
	} : data

	.bss ALIGN(4K) : {
		__bss_start = .;
		*(COMMON)
		*(.bss*)
		__bss_end = .;
	} : data

	__kernel_end = .;

	/DISCARD/ : {
		*(.comment)
		*(.eh_frame*)
		*(.note.gnu*)
		*(.note.GNU-stack)
	}
}
