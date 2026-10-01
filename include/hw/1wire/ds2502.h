/* SPDX-License-Identifier: BSD-3-Clause */
/* DS2501 and DS2502 add-only EPROM device declarations. */
#ifndef HW_1WIRE_DS2502_H
#define HW_1WIRE_DS2502_H

#include "hw/core/qdev.h"

/*
 * The DS2502-family add-only EPROM: the DS2502 itself (four programmable
 * pages) and its DS2501 sibling (two).
 *
 * Both are modelled as decoding the same 0000h-007Fh space, the smaller part
 * answering 0xff above the pages it has. That is an inference from two
 * datasheet passages, not a quoted fact: the competing reading is that the
 * smaller part's space ends at 003Fh, and the datasheets describe only pages
 * that always read FFh without saying whether those pages are still
 * addressed. No guest on this machine reads far enough to tell the two apart.
 * A plain DS2501 datasheet, or a read past 003Fh on real silicon, would
 * settle it.
 *
 * The DS2502 and DS2501 datasheets document the command set implemented here.
 */
#define TYPE_ONEWIRE_DS2501 "ds2501"
#define TYPE_ONEWIRE_DS2502 "ds2502"

#define DS2502_PAGE_SIZE 32
#define DS2502_DATA_SIZE 128   /* 0000h-007Fh on both parts */

/*
 * Load the part's EPROM image before realize. Bytes at or beyond the part's
 * programmable-page count read 0xff regardless of what buf holds there,
 * matching real silicon: a DS2501's upper two pages "always read FFH and
 * cannot be programmed". Bytes beyond DS2502_DATA_SIZE in buf are ignored.
 */
void ds2502_set_image(DeviceState *dev, const uint8_t *buf, size_t len);

#endif
