/* SPDX-License-Identifier: BSD-3-Clause */
/* Bedrock network-interface device state. */
#ifndef HW_MISC_SGI_BEDROCK_NI_H
#define HW_MISC_SGI_BEDROCK_NI_H

#include "hw/core/sysbus.h"
#include "qemu/notify.h"
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
    NotifierList changed;
};

/* Port status bit 1: the link is up. */
#define SGI_BEDROCK_NI_LINK_UP (1ULL << 1)
/*
 * Port status bit 0, set while no cable is wired to the port. Inferred from a
 * real one-node IP35 console, not established: docs/datasheets/
 * bedrock-network-interface.md, "port status". The guest's bus-error
 * handler reads port status before any processor-interface record and
 * reports a link reset when bits 1:0 read 0, so a node with no cable must
 * not read 0 there.
 */
#define SGI_BEDROCK_NI_UNCABLED (1ULL << 0)

void sgi_bedrock_ni_add_change_notifier(SGIBedrockNIState *s,
                                        Notifier *notifier);

#endif
