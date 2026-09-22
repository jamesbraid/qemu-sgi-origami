/* SPDX-License-Identifier: GPL-2.0-or-later */
/* XBridge widget-zero register and link state. */
#ifndef HW_MISC_SGI_XBRIDGE_XBOW_H
#define HW_MISC_SGI_XBRIDGE_XBOW_H

#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_XBRIDGE_XBOW "sgi-xbridge-xbow"
OBJECT_DECLARE_SIMPLE_TYPE(SGIXBridgeXbowState, SGI_XBRIDGE_XBOW)

#define SGI_XBRIDGE_XBOW_LINKS 8
#define SGI_XBRIDGE_XBOW_REG_COUNT (0x300 / 8)

struct SGIXBridgeXbowState {
    SysBusDevice parent_obj;
    MemoryRegion view[SGI_XBRIDGE_XBOW_LINKS];
    struct XbowPort {
        SGIXBridgeXbowState *xbow;
        unsigned port;
    } port[SGI_XBRIDGE_XBOW_LINKS];
    uint32_t reg[SGI_XBRIDGE_XBOW_REG_COUNT];
    uint32_t status;
    uint8_t links_present;
    uint8_t links_8bit;
    uint32_t revision;
    qemu_irq warm_reset[SGI_XBRIDGE_XBOW_LINKS];
    /* A warm reset arriving before this virtual time is ignored. */
    int64_t warm_reset_quiet_until;
    MemoryRegion *target[16];
    AddressSpace target_as[16];
};

/*
 * Board wiring, not guest-writable state: whether a link has a widget on
 * the other end, which is what its status register's alive bit and its
 * aux-status widget-present bit report. The "links-present" property is
 * the same mask for a chip whose wiring is fixed before realize; this is
 * for a port a machine attaches to after that, and takes a widget number
 * (8 to 0xf) rather than a link index.
 */
void sgi_xbridge_xbow_set_link_present(SGIXBridgeXbowState *s,
                                       unsigned int widget, bool present);


#endif
