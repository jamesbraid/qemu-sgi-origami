/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_CRIME_H
#define HW_MIPS_SGI_CRIME_H

#include "hw/core/sysbus.h"
#include "qapi/error.h"

#define TYPE_SGI_CRIME "sgi-crime"
#define SGI_CRIME_BANK_COUNT 8
OBJECT_DECLARE_SIMPLE_TYPE(SGICrimeState, SGI_CRIME)

typedef struct CrimeMigrationState {
    uint32_t hardware;
    uint32_t enable;
    uint32_t software;
    uint32_t timer_base;
    uint32_t timer_fraction;
} CrimeMigrationState;

typedef struct CrimeProfileState {
    uint32_t inputs;
    uint32_t latched;
    uint64_t cpu_error_address;
    uint32_t cpu_error_status;
    uint32_t memory_error_status;
    uint32_t memory_error_address;
} CrimeProfileState;

typedef struct CrimeTransferState {
    uint64_t tlb[32];
    uint32_t parameter[10];
    uint64_t ecc_enable;
} CrimeTransferState;

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
    uint32_t inputs;
    uint32_t latched;
    uint32_t cpu_error_status;
    uint64_t cpu_error_address;
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
    uint8_t transfer_policy;
    uint8_t profile_id;
    bool legacy_software;
    bool profile_state_received;
    CrimeProfileState incoming_profile;
    bool eight_128m_banks;
    bool reset_low_ram_approx;
    bool reset_low_ram_active;
    bool empty_renderer_status;
    bool functional_memory_transfer;
    bool transfer_state_received;
    bool healthy_memory_reported;
    CrimeMigrationState incoming;
    CrimeTransferState transfer;
    CrimeTransferState incoming_transfer;
    uint32_t incoming_memory_error_status;
    uint32_t incoming_memory_error_address;
    bool prom_id_word;
    bool alias_profile;
    bool carrier_aliases_initialized;
    bool ram_aliases_initialized;
    bool ram_aliases_mapped;
    bool bank_migration_marker_registered;
};

#endif
