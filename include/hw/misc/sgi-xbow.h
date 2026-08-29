/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow, the crosstalk crossbar.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * An eight-port switch whose own register block answers as crosstalk widget 0.
 * The identity it reports there is what a guest decides the shape of the
 * machine from, so it is a property rather than a constant: an Origin 200
 * presents something other than a crossbar's own identity at widget 0, and
 * which machine that is belongs in the machine description.
 */

#ifndef HW_MISC_SGI_XBOW_H
#define HW_MISC_SGI_XBOW_H

#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_XBOW "sgi-xbow"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXbowState, SGI_XBOW)

/*
 * The chip's register file is the widget 0 block followed by eight per-link
 * register sets of 0x40 each from 0x0100, ending at 0x300. It claims more than
 * that: the whole of its widget's crosstalk space, which the machine gives it,
 * because the widget's space is the chip's whether or not the chip decodes an
 * address in it. Anything it does not decode is an error condition it reports,
 * and claiming the space is what lets it report rather than leaving the answer
 * to whatever the machine happens to have underneath.
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
