/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI Crossbow, the crosstalk crossbar.
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
 * The chip's register file: the widget 0 block, then eight per-link register
 * sets of 0x40 each from 0x0100. The whole range is claimed even though almost
 * none of it is implemented, so that a read of a register this chip really has
 * is reported as a register we have not written rather than as no device.
 */
#define SGI_XBOW_REGS_SIZE 0x300

struct SGIXbowState {
    SysBusDevice parent_obj;

    MemoryRegion regs;

    /* What widget 0 answers with: revision, part number, manufacturer. */
    uint32_t revision;
    uint32_t part;
    uint32_t manufacturer;
};

#endif /* HW_MISC_SGI_XBOW_H */
