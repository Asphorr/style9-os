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
 * Mouse driver -- the Mach bridge over the IRQ-fed packet ring in mouse.c,
 * the exact sibling of kbd_drv.c.
 *
 * mouse_drv_init allocates `mouse_input_port` in kernel_space with
 * RECEIVE | SEND, registers a control port as "dev/mouse" speaking the
 * dev_proto protocol (INFO + OPEN_STREAM), and spawns the `mouse-drv`
 * kernel thread.  That thread parks in mouse_getpkt_block, decodes each
 * 3-byte PS/2 packet, and sends one tagged mach_msg to mouse_input_port.
 * A consumer holding a SEND right (obtained via DEV_OP_OPEN_STREAM, or by
 * holding RECEIVE directly the way the keyboard's shell does) recvs them.
 *
 * The decoded event rides entirely in msgh_id -- no body, so the
 * zero-allocation inline send path applies, just like kbd_drv:
 *	bit  24		1 -- valid-event marker (msgh_id is never 0)
 *	bits 18..16	buttons: bit 0 Left, bit 1 Right, bit 2 Middle
 *	bits 15..8	dy: signed int8, raw PS/2 Y delta
 *	bits  7..0	dx: signed int8, raw PS/2 X delta
 *
 * dx is the conventional screen X (right positive); dy follows the raw
 * PS/2 sign (up positive), so a consumer that wants screen Y negates it.
 */

#define	MOUSE_MSG_EVENT		0x01000000u	/* bit 24: valid-event marker */
#define	MOUSE_MSG_BTN_LEFT	0x01u
#define	MOUSE_MSG_BTN_RIGHT	0x02u
#define	MOUSE_MSG_BTN_MIDDLE	0x04u

extern mach_port_name_t	mouse_input_port;

void	mouse_drv_init(void);

#endif /* !_SYS_MOUSE_DRV_H_ */
