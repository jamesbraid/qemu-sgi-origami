/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow, the crosstalk crossbar.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * An eight-port switch whose register block answers as crosstalk widget 0.
 * The identity reported there is a machine property, not a constant: guests
 * decide the machine's shape from it.
 */

#ifndef HW_MISC_SGI_XBOW_H
#define HW_MISC_SGI_XBOW_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_XBOW "sgi-xbow"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXbowState, SGI_XBOW)

/*
 * The chip claims its widget's whole crosstalk space, not just the registers
 * it decodes: an undecoded address is an error it reports, not an answer the
 * machine underneath should give.
 */
struct SGIXbowState {
    SysBusDevice parent_obj;

    MemoryRegion regs;

    /* How much crosstalk space this widget owns, which the machine defines. */
    uint64_t span;

    /* What widget 0 answers with: revision, part number, manufacturer. */
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif /* HW_MISC_SGI_XBOW_H */
