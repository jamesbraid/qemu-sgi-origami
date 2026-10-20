/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_CRIME_H
#define HW_MIPS_SGI_CRIME_H

#include "hw/core/sysbus.h"
#include "qapi/error.h"

#define TYPE_SGI_CRIME "sgi-crime"
#define SGI_CRIME_BANK_COUNT 8
OBJECT_DECLARE_SIMPLE_TYPE(SGICrimeState, SGI_CRIME)

struct SGICrimeState {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion *ram;
    MemoryRegion *alias_target;
    MemoryRegion *decoded_carrier;
    MemoryRegion *alias_carrier;
    MemoryRegion low_ram;
    MemoryRegion full_ram;
    MemoryRegion no_ecc_ram;
    MemoryRegion high_ram;
    MemoryRegion carrier_low_ram;
    MemoryRegion carrier_high_ram;
    MemoryRegion carrier_full_ram;
    MemoryRegion carrier_no_ecc_ram;
    MemoryRegion decoded_ram;
    MemoryRegion invalid_ram;
    MemoryRegion reset_low_ram;
    MemoryRegion bank_ram[SGI_CRIME_BANK_COUNT];
    MemoryRegion wrong_geometry[SGI_CRIME_BANK_COUNT];
    qemu_irq cpu_ip[5];
    uint32_t hardware;
    uint32_t enable;
    uint32_t software;
    uint32_t timer_base;
    uint32_t timer_fraction;
    int64_t timer_epoch_ns;
    uint32_t memory_error_status;
    uint32_t memory_error_address;
    uint64_t decoded_base;
    uint64_t alias_base;
    uint16_t bank_control[SGI_CRIME_BANK_COUNT];
    Error *migration_blocker;
    uint8_t interface_revision;
    bool eight_128m_banks;
    bool reset_low_ram_approx;
    bool reset_low_ram_active;
    bool empty_renderer_status;
    bool prom_id_word;
    bool alias_profile;
    bool carrier_aliases_initialized;
    bool ram_aliases_initialized;
    bool ram_aliases_mapped;
    bool bank_migration_marker_registered;
};

#endif
