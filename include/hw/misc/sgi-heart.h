/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_HEART_H
#define HW_MISC_SGI_HEART_H

#include "hw/core/sysbus.h"
#include "hw/1wire/onewire.h"
#include "qemu/timer.h"

#define TYPE_SGI_HEART "sgi-heart"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHeartState, SGI_HEART)

#define HEART_BANKS 4
#define HEART_MAX_UNITS 64

struct SGIHeartState {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion widget;
    MemoryRegion packet;
    MemoryRegion packet_control;
    MemoryRegion packet_ram;
    MemoryRegion *cpu_memory;
    qemu_irq processor[3];
    MemoryRegion backing[HEART_BANKS];
    MemoryRegion views[HEART_BANKS][HEART_MAX_UNITS];
    uint64_t capacity[HEART_BANKS];
    uint8_t widget_revision;
    uint32_t widget_control;
    uint32_t widget_error_mask;
    bool mapped[HEART_BANKS][HEART_MAX_UNITS];

    uint64_t mode;
    uint64_t sdram_mode;
    uint64_t mem_refresh;
    uint64_t mem_req_arb;
    uint32_t descriptor[8];
    uint64_t mask[4];
    uint64_t pending;
    uint64_t saved_count;
    int64_t counter_epoch_ns;
    OneWireBus *onewire_bus;
    QEMUTimer *onewire_timer;
    uint64_t onewire_control;
    int64_t onewire_deadline_ns;
    int64_t onewire_remaining_ns;
    bool onewire_level;
    bool onewire_busy;
};

#endif
