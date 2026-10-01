/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI Odyssey (VPro) graphics board on an XIO slot. See hw/misc/sgi-odyssey.c
 * for the modeled surface and what is deliberately left unimplemented.
 */
#ifndef HW_MISC_SGI_ODYSSEY_H
#define HW_MISC_SGI_ODYSSEY_H

#include "hw/1wire/onewire.h"
#include "hw/mips/sgi/xio-board.h"
#include "system/memory.h"
#include "qom/object.h"

#define TYPE_SGI_ODYSSEY "sgi-odyssey"
OBJECT_DECLARE_TYPE(SGIOdysseyState, SGIOdysseyClass, SGI_ODYSSEY)

/*
 * Distinct unimplemented offsets reported before the device falls silent
 * about them. Diagnostic bookkeeping, not guest state.
 */
#define SGI_ODYSSEY_UNIMP_REPORTS 64

struct SGIOdysseyState {
    SGIXioBoardState parent_obj;

    MemoryRegion regs;
    qemu_irq irq;
    OneWireBus *nic_bus;

    /* Properties. */
    uint32_t revision;
    uint32_t memory_size;       /* megabytes */
    uint32_t memory_banks;
    uint32_t cas_latency;
    uint64_t nic_serial;
    char *nic_record;

    /* Composed at realize from the properties above. */
    uint32_t memory_config;

    /* Modeled registers. */
    uint32_t widget_control;
    uint32_t int_dest_upper;
    uint32_t int_dest_lower;
    uint32_t error_command;
    uint32_t intr_flags;
    uint32_t intr_enable;
    uint32_t sw_sync;
    uint32_t nic;

    bool irq_asserted;

    /*< private >*/
    uint32_t reported[SGI_ODYSSEY_UNIMP_REPORTS];
    unsigned int reported_count;
};

struct SGIOdysseyClass {
    SGIXioBoardClass parent_class;

    DeviceRealize parent_realize;
};

#endif /* HW_MISC_SGI_ODYSSEY_H */
