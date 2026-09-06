/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_BEDROCK_NI_H
#define HW_MISC_SGI_BEDROCK_NI_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-bedrock-ni-regs.h"

#define TYPE_SGI_BEDROCK_NI "sgi-bedrock-ni"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockNIState, SGI_BEDROCK_NI)

/*
 * MMIO 0 uses unit-relative offsets. The parent maps it at its NI unit base.
 * local-reset pulses request a parent reset; error is a reflected level.
 */
struct SGIBedrockNIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq local_reset;
    qemu_irq error;
    SGIBedrockNIRegs regs;
};
#endif
