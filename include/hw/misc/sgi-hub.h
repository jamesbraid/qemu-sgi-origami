/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI SN0 Hub ASIC.
 *
 * One 8 MB register window per node, reached through either half of the
 * crossbar's widget 1 small window: the lower half resolves to whichever node
 * the reading processor is attached to, so firmware can reach its own Hub
 * without knowing its node number, and the upper half names a node explicitly.
 */

#ifndef HW_MISC_SGI_HUB_H
#define HW_MISC_SGI_HUB_H

#include "qemu/units.h"
#include "hw/core/sysbus.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_HUB "sgi-hub"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHubState, SGI_HUB)

#define SGI_HUB_WINDOW_SIZE (8 * MiB)

/*
 * The seven big windows. Each is 512 MB of the node's crosstalk window aimed at
 * a slice of some widget's crosstalk space by one entry of the Hub's I/O
 * interface translation table, and programming those entries is how firmware
 * makes a widget reachable at all.
 *
 * They belong to the Hub rather than to the machine because the table does: a
 * second node's Hub has its own seven windows onto the same crosstalk space,
 * pointed wherever that node's firmware pointed them.
 */
#define SGI_HUB_BWIN_ENTRIES 7
#define SGI_HUB_BWIN_SIZE    (512 * MiB)
#define SGI_HUB_BWIN_TOTAL   (SGI_HUB_BWIN_ENTRIES * SGI_HUB_BWIN_SIZE)

struct SGIHubState {
    SysBusDevice parent_obj;

    MemoryRegion regs;

    /* The seven big windows, as one region for the machine to place. */
    MemoryRegion bwin;
    MemoryRegion bwin_window[SGI_HUB_BWIN_ENTRIES];
    MemoryRegion bwin_empty;

    /*
     * The crosstalk space the windows translate into, and how far apart the
     * widgets sit in it. Both are the machine's to define — a real crossbar
     * routes on a widget number rather than decoding an address bit, so the
     * stride exists only in the model — and the Hub is told rather than
     * assuming, because a wrong stride would decode silently onto the wrong
     * widget.
     */
    MemoryRegion *xtalk;
    uint32_t xtalk_widget_shift;

    /* Which node this Hub is, which its network-interface status reports. */
    uint32_t nasid;

    uint64_t pi_region_present;
    uint32_t pi_calias_size;
    uint64_t pi_int_pend[2];
    uint64_t pi_int_mask[4];
    uint64_t ni_status;
    uint32_t io_bwin[SGI_HUB_BWIN_ENTRIES];
};

#endif /* HW_MISC_SGI_HUB_H */
