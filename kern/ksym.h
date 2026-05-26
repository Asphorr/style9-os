/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_KSYM_H_
#define	_SYS_KSYM_H_

#include <stdint.h>

/*
 * Kernel symbol lookup.
 *
 * The .ksymtab section is generated post-link by tools/gen_ksyms.sh
 * (see Makefile's two-pass kernel.elf rule).  It contains the global
 * text / rodata / data / bss symbols of the running kernel, sorted by
 * address.  ksym_lookup walks the table and returns the symbol whose
 * address is the largest one not exceeding `addr`, plus the byte
 * offset into it.  Callers turn that into "sym+0xoff" output.
 *
 * Lookup is linear; the table is small enough (~1000 entries) that
 * panic-path performance is not a concern.  Returns NULL if no symbol
 * is at or below `addr`, or if the table is empty / stub.
 */

const char	*ksym_lookup(uint64_t addr, uint64_t *offset_out);

/* Pretty-print "sym+0xoff" for `addr` to stdout via kprintf. */
void		 ksym_print(uint64_t addr);

#endif /* !_SYS_KSYM_H_ */
