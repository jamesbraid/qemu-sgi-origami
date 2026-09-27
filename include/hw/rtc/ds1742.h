/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_RTC_DS1742_H
#define HW_RTC_DS1742_H

#include "hw/core/sysbus.h"
#include "system/runstate.h"
#include "qom/object.h"

#define TYPE_DS1742 "ds1742"
OBJECT_DECLARE_SIMPLE_TYPE(DS1742State, DS1742)
#define DS1742_SIZE 0x800

struct DS1742State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    BlockBackend *blk, *clock_blk;
    VMChangeStateEntry *vmstate_change;
    Notifier shutdown;
    uint8_t ram[0x7f8];
    uint8_t control, weekday, migration_weekday, staged[8], frozen[8];
    int64_t epoch, reference_ns, migration_epoch;
    bool stopped, frozen_valid;
};

#endif /* HW_RTC_DS1742_H */
