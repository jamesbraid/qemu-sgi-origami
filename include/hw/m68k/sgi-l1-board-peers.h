/*
 * The devices an Origin 3000 C-brick L1 board carries on its two-wire bus
 * and at chip select 3, beyond the identity EEPROMs.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_M68K_SGI_L1_BOARD_PEERS_H
#define HW_M68K_SGI_L1_BOARD_PEERS_H

#include "hw/i2c/i2c.h"

/* Fit the bus select, expanders and node monitors on @bus. */
void sgi_l1_cbrick_peers_fit(I2CBus *bus);

#endif
