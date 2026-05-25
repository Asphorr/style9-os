/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_TSC_H_
#define	_SYS_TSC_H_

#include <stdint.h>

/*
 * Time Stamp Counter: a free-running 64-bit cycle counter that has
 * existed on every x86 since the original Pentium.  Cheap to read
 * (rdtsc, 25-ish cycles), monotonic across CPL changes, but its
 * frequency is implementation-defined -- we calibrate against the PIT
 * once at boot and treat the result as constant thereafter.
 *
 * Calibration must run AFTER pit_init and AFTER sti, because it
 * advances the comparator by counting IRQ-driven PIT ticks.  Spinning
 * for hz/10 ticks (100ms at 100Hz) is enough to keep the relative
 * error in the parts-per-million range on QEMU.
 */

void		tsc_calibrate(void);
uint64_t	tsc_hz(void);
uint64_t	tsc_to_ns(uint64_t cycles);
uint64_t	tsc_to_us(uint64_t cycles);

static inline uint64_t
tsc_read(void)
{
	uint32_t	lo, hi;

	__asm__ __volatile__ ("rdtsc" : "=a"(lo), "=d"(hi));
	return (((uint64_t)hi << 32) | lo);
}

#endif /* !_SYS_TSC_H_ */
