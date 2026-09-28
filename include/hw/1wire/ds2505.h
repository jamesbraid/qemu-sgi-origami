/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DS2505-family add-only EPROM device declarations. */
#ifndef HW_1WIRE_DS2505_H
#define HW_1WIRE_DS2505_H

#include "hw/core/qdev.h"

/*
 * The DS2505-family add-only EPROM: 16 kbit (2048 bytes), the class the
 * Bridge's board-identity part and the Hub's node-record part both belong to.
 * The DS2505 datasheet documents the command set implemented here; DS2506
 * (the larger, overdrive-capable sibling) is out of scope here.
 */
#define TYPE_ONEWIRE_DS2505 "ds2505"

#define DS2505_PAGE_SIZE 32
#define DS2505_DATA_SIZE 2048   /* 0000h-07FFh: 16 kbit */

/*
 * Load the part's EPROM image before realize. Bytes beyond len keep the
 * unprogrammed 0xff instance_init already set; bytes beyond DS2505_DATA_SIZE
 * in buf are ignored.
 */
void ds2505_set_image(DeviceState *dev, const uint8_t *buf, size_t len);

#endif
