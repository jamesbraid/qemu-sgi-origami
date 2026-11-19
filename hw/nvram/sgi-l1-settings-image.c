/* SPDX-License-Identifier: BSD-3-Clause */
/* The Origin 3000 L1 board's settings window; l1-board.rst has the layout. */

#include "qemu/osdep.h"
#include "qemu/bswap.h"
#include "hw/nvram/sgi-l1-settings-image.h"

#define HEADER_BYTES    8
#define RECORD1_OFFSET  0x008
#define RECORD1_MIRROR  0x078
#define RECORD1_BYTES   0x70

/* Every byte of a region, its checksum included, adds to zero. */
static uint8_t sum_complement(const uint8_t *p, size_t len)
{
    uint8_t sum = 0;

    while (len--) {
        sum += *p++;
    }
    return -sum;
}

bool sgi_l1_settings_image(uint8_t *image, const SGIL1Settings *s,
                           Error **errp)
{
    uint8_t *r = image + RECORD1_OFFSET;
    uint32_t id;

    if (s->rack > 0x3ff) {
        error_setg(errp, "rack %u is above 1023", s->rack);
        return false;
    }
    if (s->bay > 0x3f) {
        error_setg(errp, "bay %u is above 63", s->bay);
        return false;
    }
    /* The firmware replaces 0xff with 0 and writes it back. */
    if (s->partition == 0xff) {
        error_setg(errp, "partition 255 is not a partition number");
        return false;
    }
    if (s->brick_type && s->brick_type != 'C') {
        error_setg(errp, "brick type '%c' is not one the firmware is known "
                   "to take", s->brick_type);
        return false;
    }

    memset(image, 0, SGI_L1_SETTINGS_WINDOW_BYTES);
    /* Version 1; byte 1 is where record 1 starts, in units of 8 bytes. */
    image[0] = 1;
    image[1] = RECORD1_OFFSET / 8;

    /* The version nibble 0xa, the rack in bits 23:14, the bay in bits 13:8. */
    id = 0x0a000000 | s->rack << 14 | s->bay << 8;
    stl_be_p(r, id);
    r[0x04] = 0x70 | (s->auto_power_up ? 0x80 : 0);
    r[0x11] = 0x02;
    /* The router role and series defaults of a rebuilt record, and bit 3. */
    r[0x17] = 0xf4 | (s->skip_serial_check ? 0x08 : 0);
    r[0x1f] = s->partition;
    r[0x20] = 0x05;
    r[0x21] = 0x0a;
    r[0x22] = s->brick_type;
    r[0x2e] = 0x02;
    r[RECORD1_BYTES - 1] = sum_complement(r, RECORD1_BYTES - 1);
    memcpy(image + RECORD1_MIRROR, r, RECORD1_BYTES);
    image[HEADER_BYTES - 1] = sum_complement(image, HEADER_BYTES - 1);
    return true;
}
