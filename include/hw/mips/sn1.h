/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN1_H
#define HW_MIPS_SN1_H

#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"

#define TYPE_SN1_MACHINE MACHINE_TYPE_NAME("origin300")
OBJECT_DECLARE_SIMPLE_TYPE(SN1MachineState, SN1_MACHINE)

#define SN1_PROCESSORS 2

struct SN1MachineState {
    MachineState parent_obj;

    Clock *cpu_refclk;
    CPUState *cpu[SN1_PROCESSORS];
    IRQState cpu_b_enable;
    bool cpu_b_enabled;
    DeviceState *bedrock;
    DeviceState *l1;

    MemoryRegion hub_memory;
    MemoryRegion bedrock_node;
    MemoryRegion hub_local_registers;
    MemoryRegion prom;
    MemoryRegion prom_reset;
    MemoryRegion prom_local;
    MemoryRegion prom_remote;
};

#endif
