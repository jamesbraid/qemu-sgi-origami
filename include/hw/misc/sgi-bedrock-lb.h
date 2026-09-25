/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_LB_H
#define HW_MISC_SGI_BEDROCK_LB_H

#include "hw/1wire/onewire.h"
#include "hw/core/sysbus.h"
#include "qemu/notify.h"
#include "qemu/timer.h"

#define TYPE_SGI_BEDROCK_LB "sgi-bedrock-lb"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockLBState, SGI_BEDROCK_LB)

#define SGI_BEDROCK_LB_SIZE 0xc040

/* Opaque here: only the owning sgi-bedrock.c dereferences it. */
typedef struct SGIBedrockState SGIBedrockState;

struct SGIBedrockLBState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq soft_reset;
    qemu_irq error;

    /*
     * The owning node, set directly by bedrock_realize() the way
     * sgi-hub.c's elsc back-reference is: a plain pointer rather than a
     * QOM link, because this child's lifetime is enclosed by its parent's.
     * Scratch 1 writes are handed to sgi_bedrock_hold_scratch1_write(),
     * which owns the processor-mask/cpu-enable state and the CPUs; see
     * sgi-bedrock.h.
     */
    SGIBedrockState *bedrock;

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
     * This node's hub register window, where a vector request arriving over
     * the link from a peer reads or writes (sgi_bedrock_lb_hub_access()).
     */
    MemoryRegion *hub_registers;
    AddressSpace hub_as;
    bool hub_as_initialized;

    uint32_t manufacturer;
    uint32_t part_number;
    uint32_t revision;
    /* The realized node number and region size, restored by every reset. */
    uint32_t node_number;
    uint32_t region_size;
    uint32_t request_region;
    /* The revision word's two fields as the firmware last wrote them. */
    uint8_t programmed_node;
    uint8_t programmed_region;
    /*
     * Told whenever programmed_node changes, by a write, a reset or a
     * load: whoever decodes addresses by this node's number (sn1-node.c's
     * claim layer) re-derives it. Not migrated; listeners re-register.
     */
    NotifierList node_notifiers;

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

/* The node number the revision word currently carries. */
uint8_t sgi_bedrock_lb_node_number(SGIBedrockLBState *s);

/* Call notifier whenever the node number the revision word carries moves. */
void sgi_bedrock_lb_add_node_notifier(SGIBedrockLBState *s,
                                      Notifier *notifier);

/*
 * One aligned 64-bit access to this node's hub register window at a
 * hub-relative offset, as a vector request arriving over the link performs
 * it. A block with no window linked answers MEMTX_DECODE_ERROR.
 */
MemTxResult sgi_bedrock_lb_hub_access(SGIBedrockLBState *s, hwaddr addr,
                                      uint64_t *value, bool write);

#endif
