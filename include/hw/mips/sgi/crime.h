/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_CRIME_H
#define HW_MIPS_SGI_CRIME_H

#include "hw/core/sysbus.h"

#define TYPE_SGI_CRIME "sgi-crime"
OBJECT_DECLARE_SIMPLE_TYPE(SGICrimeState, SGI_CRIME)

struct SGICrimeState {
    SysBusDevice parent_obj;
    MemoryRegion registers;
    MemoryRegion *ram;
    MemoryRegion *alias_target;
    MemoryRegion low_ram;
    MemoryRegion full_ram;
    MemoryRegion no_ecc_ram;
    qemu_irq cpu_ip[5];
    uint32_t hardware;
    uint32_t enable;
    uint32_t software;
    uint8_t interface_revision;
    bool ram_aliases_initialized;
    bool ram_aliases_mapped;
};

#endif
