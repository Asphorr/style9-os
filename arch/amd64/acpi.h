/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACHINE_ACPI_H_
#define	_MACHINE_ACPI_H_

#include <stdbool.h>
#include <stdint.h>

/*
 * As much ACPI as it takes to be told how many processors there are.
 *
 * WHY THIS AND NOT CPUID.  CPUID answers a question about the part -- how many
 * logical processors a package can have -- and says nothing about how many the
 * firmware actually brought out of reset or what APIC id each of them answers
 * to.  Only the MADT has that list, so counting processors is an ACPI job on
 * this architecture whether one likes ACPI or not.  Nothing else here needs
 * it: no AML, no interpreter, no namespace.  A signature scan, three
 * checksums and a table walk.
 *
 * WHAT IT REFUSES TO DO is guess.  Every step is checked -- the RSDP's
 * checksum, each table's checksum, the entry lengths as the walk consumes
 * them -- and any failure ends the probe with a printed reason and a kernel
 * that keeps running on one processor.  A machine that cannot describe itself
 * is not a machine to start extra CPUs on.
 */

/*
 * Find the MADT and register every usable processor in it.  Needs the boot
 * identity map (the tables live in low physical memory and are read through
 * it) and nothing else; safe with interrupts off.
 *
 * Returns false if there is no ACPI, no MADT, or nothing in it survived the
 * checks -- in which case the kernel knows about exactly the processor it is
 * running on, which is what it knew before.
 */
bool		acpi_madt_probe(void);

/*
 * The local APIC's register address as ACPI describes it, or zero if the MADT
 * was not read.  A second opinion, not the authority: lapic_init asks the
 * MSR, which is per-CPU and current.  The two agreeing is worth knowing
 * BECAUSE they come from different places -- firmware's description and the
 * hardware's own answer.
 */
uint64_t	acpi_lapic_pa(void);

/*
 * First IO APIC's register address and the global interrupt number its inputs
 * start at, or zero.  Nothing routes interrupts through it yet; it is recorded
 * here because the MADT is where it is written down and this is the walk that
 * reads the MADT.
 */
uint64_t	acpi_ioapic_pa(void);
uint32_t	acpi_ioapic_gsi_base(void);

/*
 * Whether the MADT says an 8259 pair is present and its interrupts must be
 * masked off before the IO APIC is used.  True on every PC-compatible machine
 * so far, including this one, and the day it is false the legacy path this
 * kernel still boots on is not there.
 */
bool		acpi_pcat_compat(void);

#endif /* !_MACHINE_ACPI_H_ */
