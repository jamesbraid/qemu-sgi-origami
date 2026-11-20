/*
 * The devices an Origin 3000 C-brick L1 board carries on its two-wire bus
 * and at chip select 3, beyond the identity EEPROMs.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_M68K_SGI_L1_BOARD_PEERS_H
#define HW_M68K_SGI_L1_BOARD_PEERS_H

#include "hw/i2c/i2c.h"

/* The management USB controller's 64 KiB window, a sysbus device. */
#define TYPE_SGI_L1_USB_WINDOW "sgi-l1-usb-window"

/*
 * Fit the bus devices on @bus; @signals, a signal link board end, is driven.
 * The scan chain is wired to @parallel_port's pp-out and pp-in lines.
 */
void sgi_l1_cbrick_peers_fit(I2CBus *bus, DeviceState *signals,
                             DeviceState *parallel_port);

#endif
