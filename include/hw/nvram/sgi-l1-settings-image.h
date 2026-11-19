/* SPDX-License-Identifier: BSD-3-Clause */
/* The Origin 3000 L1 board's settings window; l1-board.rst has the layout. */

#ifndef HW_NVRAM_SGI_L1_SETTINGS_IMAGE_H
#define HW_NVRAM_SGI_L1_SETTINGS_IMAGE_H

#include "qapi/error.h"

/* The whole window, and the part below the clock registers at its top. */
#define SGI_L1_SETTINGS_WINDOW_BYTES    0x800
#define SGI_L1_SETTINGS_CLOCK_OFFSET    0x7f8

typedef struct SGIL1Settings {
    unsigned rack;              /* 0 to 1023 */
    unsigned bay;               /* 0 to 63 */
    uint8_t partition;          /* 0 to 254 */
    char brick_type;            /* 0, or 'C': the identity fall-back */
    bool auto_power_up;
    bool skip_serial_check;
} SGIL1Settings;

/* Write @image: a header and a first record the firmware takes as it stands. */
bool sgi_l1_settings_image(uint8_t *image, const SGIL1Settings *settings,
                           Error **errp);

#endif
