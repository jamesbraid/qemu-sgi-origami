/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_LB_H
#define HW_MISC_SGI_BEDROCK_LB_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_LB "sgi-bedrock-lb"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockLBState, SGI_BEDROCK_LB)

#define SGI_BEDROCK_LB_SIZE 0xc040

struct SGIBedrockLBState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq soft_reset;
    qemu_irq error;

    uint32_t manufacturer;
    uint32_t part_number;
    uint32_t revision;
    uint32_t node_number;
    uint32_t region_size;
    uint32_t request_region;

    uint64_t cpu_permission;
    uint64_t io_permission;
    uint64_t region_present;
    uint64_t nodes_absent;
    uint64_t errors;
    uint64_t error_header[2];
    uint64_t error_data;
    uint64_t debug_select;
    uint64_t rtc_local;
    uint64_t rtc_filter;
    uint64_t scratch[5];
    uint64_t vector_route;
    uint64_t vector_data;
};

#endif
