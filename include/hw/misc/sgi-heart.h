/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MISC_SGI_HEART_H
#define HW_MISC_SGI_HEART_H

#include "hw/core/sysbus.h"
#include "hw/1wire/onewire.h"
#include "qemu/timer.h"

#define TYPE_SGI_HEART "sgi-heart"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHeartState, SGI_HEART)

#define HEART_BANKS 4
#define HEART_MAX_UNITS 64
#define HEART_SDRAM_BASE_SIZE 0x4000
/* SysBus MMIO indices, in the order heart_init() registers them. */
#define HEART_MMIO_SDRAM_BASE 3

struct SGIHeartState {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion widget;
    MemoryRegion packet;
    MemoryRegion packet_control;
    MemoryRegion packet_ram;
    MemoryRegion *cpu_memory;
    qemu_irq processor[5];
    MemoryRegion backing[HEART_BANKS];
    MemoryRegion views[HEART_BANKS][HEART_MAX_UNITS];
    MemoryRegion packet_views[HEART_BANKS][HEART_MAX_UNITS];
    MemoryRegion packet_guards[HEART_BANKS * 2];
    /* The first 16 KiB of SDRAM, whichever bank holds 0x20000000. */
    MemoryRegion sdram_base;
    MemoryRegion sdram_base_views[HEART_BANKS];
    bool sdram_base_mapped[HEART_BANKS];
    bool packet_guard_mapped[HEART_BANKS * 2];
    uint64_t capacity[HEART_BANKS];
    uint8_t widget_revision;
    uint64_t widget_id_64;
    bool widget_status_f_measured;
    uint32_t widget_control;
    uint32_t widget_request_timeout;
    uint32_t widget_error_mask;
    bool mapped[HEART_BANKS][HEART_MAX_UNITS];

    MemoryRegion ecc_memory;
    GPtrArray *ecc_regions;
    uint64_t *ecc_words;
    uint32_t ecc_word_count;
    uint32_t ecc_word_capacity;
    uint64_t ecc_cause;
    uint64_t ecc_address;
    uint64_t ecc_data;

    uint64_t mode;
    uint64_t sdram_mode;
    uint64_t mem_refresh;
    uint64_t mem_req_arb;
    uint64_t flow_cell[2];
    uint64_t clock_divider;
    uint32_t compare;
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
