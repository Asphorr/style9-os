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
 * Conversions supported: %c %s %d %i %u %x %X %p %%
 * Flags supported:        0 (zero-pad)
 * Width supported:        decimal, up to two digits
 * Length modifiers:       none -- the caller casts as needed
 *
 * Output is sent unconditionally to the VGA console via tty_putc().
 * No locking; safe to call only single-threaded.
 */

int	kprintf(const char *, ...) __attribute__((format(printf, 1, 2)));
int	kvprintf(const char *, va_list);

#endif /* !_SYS_KPRINTF_H_ */
