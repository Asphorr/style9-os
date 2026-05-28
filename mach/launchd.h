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
 * Bring-up is one call: launchd_subsystem_init().  Invoked from
 * services_init after the other services so a debugger probe at
 * kmain wedge sees a fully-populated bootstrap registry.
 */
void	launchd_subsystem_init(void);

#endif /* !_MACH_LAUNCHD_H_ */
