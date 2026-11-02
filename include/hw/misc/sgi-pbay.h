/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Origin 3000 power-bay controller: the bay end of the L1's power-bay
 * serial link.
 */

#ifndef HW_MISC_SGI_PBAY_H
#define HW_MISC_SGI_PBAY_H

#include "hw/core/qdev.h"
#include "chardev/char-fe.h"
#include "qom/object.h"

#define TYPE_SGI_PBAY "sgi-pbay"
OBJECT_DECLARE_SIMPLE_TYPE(SGIPbayState, SGI_PBAY)

/* The bay's address, sent on the wire little-endian as 00 44. */
#define SGI_PBAY_ADDRESS        0x4400

/*
 * The largest unstuffed frame body the bay accepts. The longest frame the
 * link carries is a flash chunk (at most SGI_PBAY_FLASH_CHUNK_MAX data
 * bytes) behind a header and checksum; the rest is margin. A model choice:
 * no source states the bay's receive buffer.
 */
#define SGI_PBAY_MAX_FRAME      0x100

/* The flash chunk size the 0xc0 reply advertises: default and upper limit. */
#define SGI_PBAY_FLASH_CHUNK_DEFAULT    0x80
#define SGI_PBAY_FLASH_CHUNK_MAX        0x8c

struct SGIPbayState {
    DeviceState parent_obj;

    CharFrontend chr;

    /* Configuration, not migrated. */
    uint8_t flash_chunk;
    uint8_t state_code;
    /* What P1 reads when the sensors are sampled; every input follows it. */
    uint8_t input_snapshot;
    /* The loader's frame status; only 0xff changes a reply. */
    uint8_t frame_status;

    /* Receive: the unstuffed body so far. */
    uint8_t rx[SGI_PBAY_MAX_FRAME];
    uint32_t rx_len;
    bool rx_escape;
    bool rx_discard;
};

#endif /* HW_MISC_SGI_PBAY_H */
