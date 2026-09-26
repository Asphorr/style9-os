/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_KPRINTF_H_
#define	_SYS_KPRINTF_H_

#include <stdarg.h>
#include <stddef.h>

/*
 * Minimal in-kernel printf.
 *
 * Conversions supported: %c %s %d %i %u %o %x %X %p %%
 * Flags supported:        - (left-align), 0 (zero-pad)
 * Width supported:        decimal
 * Length modifiers:       l, ll, z
 *
 * Output goes through tty_putc() (VGA, with serial and debugcon
 * mirrors).  Each call is one console write: tty_batch_begin holds the
 * console for this CPU until it returns, so outside a panic lines from
 * different CPUs do not interleave.
 */

int	kprintf(const char *, ...) __attribute__((format(printf, 1, 2)));
int	kvprintf(const char *, va_list);

#endif /* !_SYS_KPRINTF_H_ */
