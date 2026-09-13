/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_LB_H
#define HW_MISC_SGI_BEDROCK_LB_H

#include "hw/1wire/onewire.h"
#include "hw/core/sysbus.h"
#include "qemu/timer.h"

#define TYPE_SGI_BEDROCK_LB "sgi-bedrock-lb"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockLBState, SGI_BEDROCK_LB)

#define SGI_BEDROCK_LB_SIZE 0xc040

struct SGIBedrockLBState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq soft_reset;
    qemu_irq error;

    /*
     * The MicroLAN wire and the controller's slot deadline. The endpoint is
     * the machine's to hang here; which part IP45 carries is not established,
     * so nothing is composed by default and the bus can be empty.
     */
    OneWireBus *microlan_bus;
    QEMUTimer *microlan_timer;
    /* Sampled when the slot starts, published when its deadline arrives. */
    bool microlan_level;

    /*
     * This node's hub register window, so the vector engine can read a hub
     * register for a routed read. The window aliases this block, so a self
     * route lands back on these registers.
     */
    MemoryRegion *hub_registers;
    AddressSpace hub_as;
    bool hub_as_initialized;

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
    uint64_t microlan;
    uint64_t errors;
    uint64_t error_header[2];
    uint64_t error_data;
    uint64_t debug_select;
    uint64_t rtc_local;
    uint64_t rtc_filter;
    uint64_t scratch[5];
    uint64_t vector_params;
    uint64_t vector_route;
    uint64_t vector_data;
    uint64_t vector_status;
    uint64_t vector_return;
    uint64_t vector_read;
};

#endif
