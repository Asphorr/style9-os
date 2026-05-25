/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_DBGCON_H_
#define	_SYS_DBGCON_H_

#include <stddef.h>

/*
 * Debug console -- mirrors all VGA console output to the QEMU debugcon
 * I/O port (0xE9), which appears on the host side when QEMU is invoked
 * with -debugcon stdio.  Real hardware silently ignores writes to this
 * port, so leaving the mirror always-on does no harm.
 */

void	dbgcon_putc(char);
void	dbgcon_write(const char *, size_t);

#endif /* !_SYS_DBGCON_H_ */
