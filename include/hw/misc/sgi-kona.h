/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI KONA: the InfiniteReality graphics pipe, increment 1, identity only.
 * An sgi-xtalk-board subclass whose widget answers the PROM's widget-header
 * probe, its `+0x64` classification probe, and the `+0x98` one-wire
 * identity chain the driver reads during board discovery -- nothing else.
 * See docs/datasheets/infinitereality-host-interface.md for the register
 * evidence and docs/designs/infinitereality-host-command-approximation.md
 * for the approximation ledger this increment implements the first stage
 * of. Every value this file seeds beyond the widget-ID and `+0x64` fields
 * is synthetic: no one-wire capture of a real InfiniteReality pipe exists.
 */

#ifndef HW_MISC_SGI_KONA_H
#define HW_MISC_SGI_KONA_H

#include "hw/mips/sgi/xio-board.h"
#include "hw/1wire/onewire.h"
#include "qemu/units.h"
#include "qom/object.h"

#define TYPE_SGI_KONA "sgi-kona"
OBJECT_DECLARE_SIMPLE_TYPE(SGIKonaState, SGI_KONA)

/* One widget's worth of crosstalk address space; the rest reads unmapped. */
#define SGI_KONA_APERTURE_SIZE (16 * MiB)

/* Widget-header identification, the generic crosstalk layout. */
#define SGI_KONA_WIDGET_ID 0x000004

/* A plain, generic crosstalk register; this model interprets none of it. */
#define SGI_KONA_WIDGET_CONTROL 0x000024

/*
 * The PROM's subsystem-classification probe: a 3-bit field read masked
 * with 7, established by docs/datasheets/infinitereality-host-interface.md
 * as the switch between a part-based classify (0), a one-wire-bypass path
 * (1) and "unknown subsystem" (else). This model always answers 0,
 * admitting the part-based path -- the only one the reviewed disassembly
 * traced completely.
 */
#define SGI_KONA_PROBE 0x000064

/*
 * The one-wire identity chain: the same low-twenty-bit pulse/sample/done/
 * level layout the Bridge's own NIC register and the Hub's one-wire
 * register carry (docs/datasheets/hub-onewire.md), reused here rather than
 * invented. A 32-bit write starts one bit slot; completion is reported
 * synchronously, inside the write, exactly as the Bridge does.
 */
#define SGI_KONA_ONEWIRE 0x000098

#define SGI_KONA_ONEWIRE_LEVEL        (1u << 0)
#define SGI_KONA_ONEWIRE_DONE         (1u << 1)
#define SGI_KONA_ONEWIRE_SAMPLE_SHIFT 2
#define SGI_KONA_ONEWIRE_SAMPLE_MASK  (0xffu << 2)
#define SGI_KONA_ONEWIRE_PULSE_SHIFT  10
#define SGI_KONA_ONEWIRE_PULSE_MASK   (0x3ffu << 10)
#define SGI_KONA_ONEWIRE_WRITABLE \
    (SGI_KONA_ONEWIRE_SAMPLE_MASK | SGI_KONA_ONEWIRE_PULSE_MASK)

struct SGIKonaState {
    SGIXioBoardState parent_obj;

    /*
     * Set by build_widget(), before this board re-parents: the crosstalk
     * fabric proxy sgi_xtalk_crossbow_link_claim_fabric() claims. Required;
     * a board realized without it refuses.
     */
    MemoryRegion *fabric;

    MemoryRegion regs;

    uint32_t manufacturer;
    uint32_t part;
    uint32_t revision;

    /* Plain stored fields; this model interprets neither. */
    uint32_t widget_control;
    uint32_t probe_value;

    uint32_t onewire_reg;
    OneWireBus *onewire_bus;
};

#endif
