/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_IO_H
#define HW_MIPS_SN0_ORIGIN2000_IO_H

#include "system/memory.h"
#include "sn0.h"

typedef struct SN0Origin2000HubIO {
    MemoryRegion face;
    AddressSpace target;
    uint64_t memory_size;
    uint64_t xtalk_base;
    uint64_t node_stride;
    uint64_t modify_offset;
} SN0Origin2000HubIO;

/* Initialize after node realization, destroy before releasing its regions. */
bool sn0_origin2000_hub_io_init(SN0Origin2000HubIO *io, SN0NodeState *node,
                               Error **errp);
void sn0_origin2000_hub_io_destroy(SN0Origin2000HubIO *io);

#endif
