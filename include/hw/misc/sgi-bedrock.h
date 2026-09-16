/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_H
#define HW_MISC_SGI_BEDROCK_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-bedrock-lb.h"
#include "hw/misc/sgi-bedrock-io.h"
#include "hw/misc/sgi-bedrock-md.h"
#include "hw/misc/sgi-bedrock-ni.h"
#include "hw/misc/sgi-bedrock-pi.h"
#include "hw/misc/sgi-bedrock-xb.h"

#define TYPE_SGI_BEDROCK "sgi-bedrock"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockState, SGI_BEDROCK)

#define SGI_BEDROCK_SIZE 0x7e0048

struct SGIBedrockState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    /*
     * A zero-based view of the register window for the vector engine, so a
     * routed read addresses hub registers wherever mmio itself is mapped.
     */
    MemoryRegion hub_window;
    MemoryRegion hub_special_registers;
    MemoryRegion remote_registers;
    MemoryRegion io_registers;
    SGIBedrockPIState pi[2];
    SGIBedrockIOState io;
    SGIBedrockMDState md;
    SGIBedrockLBState lb;
    SGIBedrockNIState ni;
    SGIBedrockXBState xb;
    qemu_irq hardware_irq[2 * SGI_BEDROCK_PI_PROCESSORS *
                           SGI_BEDROCK_PI_HW_IRQS];
    qemu_irq nmi[2 * SGI_BEDROCK_PI_PROCESSORS];
    qemu_irq cpu_enable[2 * SGI_BEDROCK_PI_PROCESSORS];
    uint32_t processor_count;
    uint32_t processor_mask;
    uint32_t cpu_enable_initial_mask;
    uint32_t cpu_base;
    uint8_t revision;
    uint8_t local_region;
    uint8_t installed_memory_bank_mask;
    uint16_t installed_memory_bank_mib[8];
    uint16_t inbound_widget_mask;
    uint8_t inbound_device;
    uint64_t device_mask_reset;
    MemoryRegion *dma_mr;
    MemoryRegion *node_mr;
    MemoryRegion *l1_uart_mr;
    AddressSpace dma_as;
    bool dma_as_initialized;
    AddressSpace l1_uart_as;
    bool l1_uart_as_initialized;
    uint8_t initiator_region;
    MemoryRegion *widget_mr[16];
};

bool sgi_bedrock_hub_registers_permitted(SGIBedrockState *s);

#endif
