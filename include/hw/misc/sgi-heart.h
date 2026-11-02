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
/*
 * The "processor" outputs: lines 0 to 4 are processor 0's interrupt
 * levels 0 to 4 (its IP2 to IP6), lines 5 to 7 the level-4 line (IP6) of
 * processors 1 to 3.
 */
#define HEART_PROCESSOR_LEVELS 5
#define HEART_PROCESSORS 4
#define HEART_PROCESSOR_LINES (HEART_PROCESSOR_LEVELS + HEART_PROCESSORS - 1)

struct SGIHeartState {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion widget;
    MemoryRegion packet;
    MemoryRegion packet_control;
    MemoryRegion packet_ram;
    MemoryRegion *cpu_memory;
    qemu_irq processor[HEART_PROCESSOR_LINES];
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
    /* Widget error type (0x60) and the PIO error latches (0x70, 0x78, 0x90). */
    uint64_t widget_error_type;
    uint64_t pio_error_upper;
    uint64_t pio_error_lower;
    uint64_t pio_read_timeout;
    /* A widget error set pending bit 63, so mode bit 13 does not gate it. */
    bool exception_from_widget;
    bool mapped[HEART_BANKS][HEART_MAX_UNITS];

    MemoryRegion ecc_memory;
    GPtrArray *ecc_regions;
    uint64_t *ecc_words;
    uint32_t ecc_word_count;
    uint32_t ecc_word_capacity;
    /* Cause (0x10040): the ECC bits 0 and 1 and the widget error bit 16. */
    uint64_t cause;
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
    uint64_t mask[HEART_PROCESSORS];
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
