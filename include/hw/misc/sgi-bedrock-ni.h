/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Bedrock network-interface device state. */
#ifndef HW_MISC_SGI_BEDROCK_NI_H
#define HW_MISC_SGI_BEDROCK_NI_H

#include "hw/core/sysbus.h"
#include "hw/misc/sgi-bedrock-ni-regs.h"

#define TYPE_SGI_BEDROCK_NI "sgi-bedrock-ni"
OBJECT_DECLARE_SIMPLE_TYPE(SGIBedrockNIState, SGI_BEDROCK_NI)

/* Opaque here: only the owning sgi-bedrock.c dereferences it. */
typedef struct SGIBedrockState SGIBedrockState;

/*
 * MMIO 0 uses unit-relative offsets. The parent maps it at its NI unit base.
 * local-reset pulses request a parent reset; error is a reflected level.
*/
struct SGIBedrockNIState {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    qemu_irq local_reset;
    qemu_irq error;
    /*
     * The owning node, set by bedrock_realize(), which owns the port's cable;
     * port status reads the link from it. NULL for a standalone unit, whose
     * port is cabled to nothing.
    */
    SGIBedrockState *bedrock;
    SGIBedrockNIRegs regs;
};

/* Port status bit 1: the link is up. */
#define SGI_BEDROCK_NI_LINK_UP (1ULL << 1)

#endif
