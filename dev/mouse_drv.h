/*-
 * SPDX-License-Identifier: BSD-2-Clause
 *
 * Copyright (c) 2026 The Hobby OS Project
 * All rights reserved.
 */

#ifndef _SYS_MOUSE_DRV_H_
#define	_SYS_MOUSE_DRV_H_

#include "port.h"

/*
 * Mouse driver: the Mach bridge over mouse.c's packet ring, kbd_drv.c's
 * sibling.
 *
 * mouse_drv_init allocates `mouse_input_port' in kernel_space with
 * RECEIVE | SEND, registers "dev/mouse" (INFO + OPEN_STREAM), and starts
 * the `mouse-drv' thread, which decodes each packet and sends it to
 * mouse_input_port.  The consumer holds the RECEIVE right, moved to it by
 * DEV_OP_OPEN_STREAM.
 *
 * The event rides entirely in msgh_id, with no body:
 *	bit  24		1 -- valid-event marker (msgh_id is never 0)
 *	bits 18..16	buttons: bit 0 Left, bit 1 Right, bit 2 Middle
 *	bits 15..8	dy: signed int8, PS/2 Y delta
 *	bits  7..0	dx: signed int8, PS/2 X delta
 *
 * The nine-bit device deltas are clamped to [-128, 127], so a fast move
 * saturates rather than wraps; an overflowed axis reads as full scale.
 * dx is screen X (right positive); dy keeps the PS/2 sign (up positive),
 * so a consumer wanting screen Y negates it.
 */

#define	MOUSE_MSG_EVENT		0x01000000u	/* bit 24: valid-event marker */
#define	MOUSE_MSG_BTN_LEFT	0x01u
#define	MOUSE_MSG_BTN_RIGHT	0x02u
#define	MOUSE_MSG_BTN_MIDDLE	0x04u

extern mach_port_name_t	mouse_input_port;

void	mouse_drv_init(void);

#endif /* !_SYS_MOUSE_DRV_H_ */
