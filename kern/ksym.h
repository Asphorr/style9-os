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
 * (see Makefile's two-pass kernel.elf rule) from the kernel's global
 * text / rodata / data / bss symbols.  ksym_lookup returns the symbol
 * with the largest address not exceeding `addr', and the offset into it
 * in *offset_out; NULL (offset 0) if there is none or the table is the
 * stub.  A linear scan: about a thousand entries, on the panic path.
 */

const char	*ksym_lookup(uint64_t addr, uint64_t *offset_out);

/* Print `addr' as "0x... <sym+0xoff>", or bare hex, via kprintf. */
void		 ksym_print(uint64_t addr);

#endif /* !_SYS_KSYM_H_ */
