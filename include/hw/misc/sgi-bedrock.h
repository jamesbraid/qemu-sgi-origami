/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_H
#define HW_MISC_SGI_BEDROCK_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-bedrock-lb.h"
#include "hw/misc/sgi-bedrock-ni.h"
#include "hw/misc/sgi-bedrock-pi.h"

#define TYPE_SGI_BEDROCK "sgi-bedrock"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockState, SGI_BEDROCK)

#define SGI_BEDROCK_SIZE 0x683000

struct SGIBedrockState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    SGIBedrockPIState pi[2];
    SGIBedrockLBState lb;
    SGIBedrockNIState ni;
    qemu_irq hardware_irq[2 * SGI_BEDROCK_PI_PROCESSORS *
                           SGI_BEDROCK_PI_HW_IRQS];
    qemu_irq nmi[2 * SGI_BEDROCK_PI_PROCESSORS];
    uint32_t processor_count;
};

#endif
