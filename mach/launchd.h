/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _MACH_LAUNCHD_H_
#define	_MACH_LAUNCHD_H_

#include "port.h"
#include "services.h"

/*
 * In-kernel launchd analog.  Registers itself with the global
 * bootstrap port under SVC_LAUNCHD_NAME, handles LAUNCHCTL_OP_*
 * messages synchronously, owns a small fixed-size service registry
 * (LAUNCHD_MAX_SERVICES rows) protected by an internal spinlock.
 *
 * Wire protocol + state model live in mach/services.h alongside the
 * other kernel-side services.  Implementation in mach/launchd.c.
 *
 * Bring-up is TWO calls, in this order, and the order is load-bearing:
 *
 *	launchd_subsystem_init()	from services_init
 *	launchd_load_catalog()		from kmain, AFTER progreg_init
 *
 * The split exists because the boot catalog names programs by string and
 * resolves them through the program registry, which services_init runs too
 * early to see.  Materialising the catalog from the worker thread instead
 * only appeared to fix that: it made the load wait for the scheduler rather
 * than for progreg_init, and the two are not the same thing.  See the
 * comment above launchd_load_catalog in mach/launchd.c.
 *
 * Calling it twice is harmless -- the second call returns immediately.
 */
void	launchd_subsystem_init(void);
void	launchd_load_catalog(void);

#endif /* !_MACH_LAUNCHD_H_ */
