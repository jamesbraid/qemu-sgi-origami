/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_1WIRE_DS2502_H
#define HW_1WIRE_DS2502_H

#include "hw/core/qdev.h"

/*
 * The DS2502-family add-only EPROM: the DS2502 itself (four programmable
 * pages) and its DS2501 sibling (two). Both decode the same 0000h-007Fh
 * address space; see docs/library/dallas-1wire-part-specs.md section C.1
 * for the command set this implements.
 */
#define TYPE_ONEWIRE_DS2501 "ds2501"
#define TYPE_ONEWIRE_DS2502 "ds2502"

#define DS2502_PAGE_SIZE 32
#define DS2502_DATA_SIZE 128   /* 0000h-007Fh on both parts */

/*
 * Load the part's EPROM image before realize. Bytes at or beyond the
 * part's programmable-page count read 0xff regardless of what buf holds
 * there, matching real silicon: a DS2501's upper two pages "always read
 * FFH and cannot be programmed". Bytes beyond DS2502_DATA_SIZE in buf are
 * ignored.
 */
void ds2502_set_image(DeviceState *dev, const uint8_t *buf, size_t len);

#endif
