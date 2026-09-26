/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * crasher -- exits at once, to exercise launchd's keepalive respawn
 * throttle.  After a few fast exits under a keepalive job, launchd parks
 * the job in the THROTTLED state instead of respawning it forever.
 * launchctl's respawn-throttle demo drives it.
 */

#include "style9.h"

int
main(void)
{

	printf("crasher: immediate exit (keep_alive throttle target)\n");
	return (1);
}
