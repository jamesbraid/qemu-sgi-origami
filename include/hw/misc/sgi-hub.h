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

struct SGIHubState {
    SysBusDevice parent_obj;

    MemoryRegion regs;

    /* Which node this Hub is, which its network-interface status reports. */
    uint32_t nasid;

    uint64_t pi_region_present;
    uint32_t pi_calias_size;
    uint64_t pi_int_pend[2];
    uint64_t pi_int_mask[4];
    uint64_t ni_status;
};

#endif /* HW_MISC_SGI_HUB_H */
