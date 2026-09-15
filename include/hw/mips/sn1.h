/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN1_H
#define HW_MIPS_SN1_H

#include "hw/core/boards.h"
#include "hw/core/clock.h"
#include "hw/core/irq.h"
#include "qapi/qapi-builtin-types.h"

#define TYPE_SN1_MACHINE MACHINE_TYPE_NAME("origin300")
OBJECT_DECLARE_SIMPLE_TYPE(SN1MachineState, SN1_MACHINE)

/*
 * A cacheable ROM device mirroring the boot flash's current storage. Stores
 * still reach its write callback and are refused; its RAM block migrates.
 */
typedef struct SN1PromView {
    MemoryRegion rom;
    MemoryRegion *backing;
    const char *name;
} SN1PromView;

struct SN1MachineState {
    MachineState parent_obj;

    Clock *cpu_refclk;
    /* The node's CPUs, Bedrock, L1 and boot flash; see sgi-sn1-node. */
    DeviceState *node;

    MemoryRegion hub_memory;
    MemoryRegion hub_local_registers;
    MemoryRegion board_id;
    MemoryRegion board_id_remote;
    MemoryRegion prom;
    MemoryRegion *prom_views;
    MemoryRegion prom_flash;
    MemoryRegion prom_flash_remote;

    /* Used in place of the aliases above once a flash part is attached. */
    SN1PromView prom_view_reset;
    SN1PromView prom_view_local_high;

    /* Test-only perturbation of the model's back-to-back CPU release. */
    uint64_t cpu_release_skew_ns;

    strList *chassis_eeprom;
    strList *board_eeprom;
    strList *spd_eeprom;
};

/*
 * Copies the machine's own identity-record file lists onto an L1 device
 * before it is realized. Takes the L1 explicitly because it lives on the
 * node (sgi-sn1-node), while the file lists it reads are machine
 * properties -- one identity per machine, one L1 per node.
 */
void sn1_set_l1_identity_records(SN1MachineState *s, DeviceState *l1);

#endif
