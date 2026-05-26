#!/bin/sh
# SPDX-License-Identifier: BSD-2-Clause
#
# Generate ksyms.S from a linked kernel ELF.
#
# Usage: gen_ksyms.sh kernel.elf > obj/ksyms.S
#
# Reads `nm -n kernel.elf` (sorted by address) and emits an assembly
# blob into the .ksymtab section consisting of:
#	(addr, name_ptr) entries, terminated by addr = 0
#	asciz string pool that name_ptr fields point into
#
# Only globally-visible text / rodata / data / bss symbols
# (nm type letters T/R/D/B/A) are included; file-local symbols (lower
# case) and undefined ones are skipped.

set -e

if [ -z "$1" ]; then
	echo "usage: $0 kernel.elf" 1>&2
	exit 1
fi

nm -n "$1" | awk '
# Keep only T (text), R (rodata), D (data), B (bss) globals.  Skip
# A (absolute) -- those carry sizes/constants rather than addresses
# and would corrupt the sorted-by-addr lookup.  Require the address
# field to be a non-empty hex string (some awk dialects do not handle
# the {16} quantifier, so spell the test out as a length check).
$2 ~ /^[TRDB]$/ && $3 != "" && $1 ~ /^[0-9a-fA-F]+$/ && length($1) >= 8 {
	addr[n] = $1
	name[n] = $3
	n++
}
END {
	print ".section .ksymtab,\"a\",@progbits"
	print ".global __ksymtab_start"
	print ".global __ksymtab_end"
	print "__ksymtab_start:"
	for (i = 0; i < n; i++) {
		# Use printf to keep addr[i] in string context.  Plain
		# `print ".quad 0x" addr[i]` makes some awks treat the
		# hex string as a numeric zero (leading zeros) and emit
		# `.quad 0x` with the suffix dropped.
		printf ".quad 0x%s\n", addr[i]
		printf ".quad ksym_str_%d\n", i
	}
	print ".quad 0"
	print ".quad 0"
	for (i = 0; i < n; i++) {
		printf "ksym_str_%d: .asciz \"%s\"\n", i, name[i]
	}
	print "__ksymtab_end:"
}
'
