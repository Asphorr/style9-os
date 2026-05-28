/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 *
 * crasher -- a deliberately short-lived program for exercising the
 * launchd keep_alive respawn throttle.  It prints one line and exits
 * immediately.  Under a keep_alive job the supervisor respawns it, it
 * exits again, and after a few such fast exits launchd's throttle
 * trips and parks the job in the THROTTLED state instead of respinning
 * it forever.  (launchctl drives this; see its respawn-throttle demo.)
 */

#include "style9.h"

int
main(void)
{

	printf("crasher: immediate exit (keep_alive throttle target)\n");
	return (1);
}
