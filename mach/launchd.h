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
 * In-kernel launchd.  Registered with bootstrap as SVC_LAUNCHD_NAME, it
 * handles LAUNCHCTL_OP_* synchronously over a fixed registry of
 * LAUNCHD_MAX_SERVICES jobs.  The wire protocol and states are in
 * mach/services.h.
 *
 * Bring-up is two calls, in this order:
 *
 *	launchd_subsystem_init()	from services_init
 *	launchd_load_catalog()		from kmain, after progreg_init
 *
 * The boot catalog names programs that only progreg_init makes
 * resolvable, and services_init runs before it.  launchd_load_catalog
 * is idempotent.
 */
void	launchd_subsystem_init(void);
void	launchd_load_catalog(void);

/*
 * BOOTSTRAP_OP_CHECK_IN's half in launchd: hand over the receive right of
 * the Mach service kept under `label` to its job's task `task_id`, as the
 * kernel_space name it sits under.  Launchd lets go of it here; the
 * caller moves it out, and if that fails, drops the RECEIVE there, which
 * PORT_DESTROYED brings back.  MACH_E_NAME for no such service,
 * MACH_E_RIGHT for another task or a right already out.
 */
int	launchd_check_in(const char *label, uint64_t task_id,
	    mach_port_name_t *recv_out);

#endif /* !_MACH_LAUNCHD_H_ */
