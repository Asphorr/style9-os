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
 * As much ACPI as it takes to learn which processors exist.  CPUID
 * describes the part, not how many processors the firmware brought up or
 * their APIC ids; only the MADT lists those.  No AML or namespace: an RSDP
 * scan, checksums and a table walk.
 *
 * Every step is checked -- RSDP and table checksums, entry lengths as the
 * walk consumes them -- and any failure ends the probe with a printed
 * reason, leaving the kernel on one processor.
 */

/*
 * Find the MADT and register every usable processor in it.  Needs only the
 * boot identity map; safe with interrupts off.  Returns false if there is
 * no RSDP or no valid MADT, in which case only the running processor is
 * known.
 */
bool		acpi_madt_probe(void);

/*
 * The local APIC's register address per the MADT, or zero if it was not
 * read.  A second opinion: lapic_init uses the MSR, and acpi_madt_probe
 * reports if the two disagree.
 */
uint64_t	acpi_lapic_pa(void);

/*
 * First IO APIC's register address and the GSI its inputs start at, or
 * zero.  Recorded only; nothing routes interrupts through it yet.
 */
uint64_t	acpi_ioapic_pa(void);
uint32_t	acpi_ioapic_gsi_base(void);

/*
 * Whether the MADT says an 8259 pair is present (PCAT_COMPAT), which must
 * be masked before the IO APIC is used.
 */
bool		acpi_pcat_compat(void);

#endif /* !_MACHINE_ACPI_H_ */
