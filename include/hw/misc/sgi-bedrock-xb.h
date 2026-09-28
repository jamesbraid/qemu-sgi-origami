/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock internal crossbar register state. */
#ifndef HW_MISC_SGI_BEDROCK_XB_H
#define HW_MISC_SGI_BEDROCK_XB_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_XB "sgi-bedrock-xb"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockXBState, SGI_BEDROCK_XB)

#define SGI_BEDROCK_XB_SIZE 0x198
#define SGI_BEDROCK_XB_QUEUES 14

struct SGIBedrockXBState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq error;

    uint64_t parameters;
    uint64_t slow_grant;
    uint64_t spew_control;
    uint64_t arbitration_trigger;
    uint64_t queue_errors[SGI_BEDROCK_XB_QUEUES];
    uint64_t first_error;
    uint64_t debug_data;
    uint64_t debug_arbitration;
    /* Bit n: queue n was written since its clear alias was last serviced. */
    uint16_t unserviced;
};

#endif
