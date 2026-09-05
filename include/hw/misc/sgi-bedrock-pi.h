/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_PI_H
#define HW_MISC_SGI_BEDROCK_PI_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_BEDROCK_PI "sgi-bedrock-pi"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockPIState, SGI_BEDROCK_PI)

#define SGI_BEDROCK_PI_SIZE 0x30108
#define SGI_BEDROCK_PI_PROCESSORS 2
#define SGI_BEDROCK_PI_HW_IRQS 5

struct SGIBedrockPIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq hardware_irq[SGI_BEDROCK_PI_PROCESSORS *
                           SGI_BEDROCK_PI_HW_IRQS];
    qemu_irq nmi[SGI_BEDROCK_PI_PROCESSORS];

    uint32_t interface;
    uint32_t processor_count;
    uint32_t request_region;
    uint64_t cpu_protect;
    uint64_t io_protect;
    uint64_t region_present;
    uint8_t cached_alias;
    uint8_t cpu_enable[2];
    uint64_t pending[2];
    uint64_t mask[2][2];
    uint8_t remap;
    uint64_t error_pending;
    uint64_t error_mask[2];
    uint64_t error_stack[2];
    uint8_t error_stack_size;
    bool lb_error;
    bool ni_error;
};

void sgi_bedrock_pi_set_lb_error(SGIBedrockPIState *s, bool level);
void sgi_bedrock_pi_set_ni_error(SGIBedrockPIState *s, bool level);
void sgi_bedrock_pi_recompute(SGIBedrockPIState *s);

#endif
