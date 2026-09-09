/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN1_H
#define HW_MIPS_SN1_H

#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "qapi/qapi-builtin-types.h"

#define TYPE_SN1_MACHINE MACHINE_TYPE_NAME("origin300")
OBJECT_DECLARE_SIMPLE_TYPE(SN1MachineState, SN1_MACHINE)

struct SN1MachineState {
    MachineState parent_obj;

    Clock *cpu_refclk;
    CPUState **cpu;
    IRQState cpu_b_enable;
    bool cpu_b_enabled;
    DeviceState *bedrock;
    DeviceState *l1;
    DeviceState *xbridge;
    DeviceState *boot_flash;

    MemoryRegion hub_memory;
    MemoryRegion bedrock_node;
    MemoryRegion hub_local_registers;
    MemoryRegion board_id;
    MemoryRegion board_id_remote;
    MemoryRegion prom;
    MemoryRegion *prom_views;
    MemoryRegion prom_flash;
    MemoryRegion prom_flash_payload;
    MemoryRegion prom_flash_erased;

    strList *chassis_eeprom;
    strList *board_eeprom;
    strList *spd_eeprom;
};

#endif
