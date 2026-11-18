/* SPDX-License-Identifier: BSD-3-Clause */
/* The Altix 350 node's L1 controller. */
#ifndef HW_MISC_SGI_L1_SN2_H
#define HW_MISC_SGI_L1_SN2_H

#include "qom/object.h"

#define TYPE_SGI_L1_SN2 "sgi-l1-sn2"
OBJECT_DECLARE_SIMPLE_TYPE(SGIL1SN2State, SGI_L1_SN2)

/* The link chardev the device creates, for the node 16550's "chardev". */
typedef struct Chardev Chardev;
Chardev *sgi_l1_sn2_link(SGIL1SN2State *s);

/* The SH_SCRATCH2 word: rack, bay, slab 0 and partition, low byte 0x6c. */
static inline uint64_t sgi_l1_sn2_identity_word(unsigned rack, unsigned bay,
                                                unsigned partition)
{
    return ((uint64_t)(rack >> 6 & 0xf) << 32) |
           ((uint64_t)(rack & 0x1ff) << 26) |
           ((uint64_t)(bay & 0x3f) << 20) |
           ((uint64_t)(partition & 0xff) << 8) | 0x6c;
}

#endif
