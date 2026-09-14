/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_IO_H
#define HW_MIPS_SN0_ORIGIN2000_IO_H

#include "system/memory.h"
#include "sn0.h"

typedef struct SN0Origin2000HubIO {
    MemoryRegion face;
    AddressSpace *target;
    AddressSpace *local_registers;
    MemoryRegion local_register_view;
    uint64_t memory_size;
    uint64_t xtalk_base;
    uint64_t node_stride;
    uint64_t modify_offset;
    bool initialized;
    bool active;
} SN0Origin2000HubIO;

/*
 * Start zero-initialized; storage must live until owner's instance_finalize.
 * Initialize after node realization. Remove users and destroy before releasing
 * owner or target regions. The adapter's regions are finalized with owner.
 */
bool sn0_origin2000_hub_io_init(SN0Origin2000HubIO *io, Object *owner,
                               SN0NodeState *node,
                               Error **errp);
void sn0_origin2000_hub_io_destroy(SN0Origin2000HubIO *io);

typedef struct SN0Origin2000HubOutgoingIO {
    MemoryRegion face;
    AddressSpace *packets;
    bool initialized;
    bool active;
} SN0Origin2000HubOutgoingIO;

/*
 * Storage lives until owner finalization. Supply a per-source packet proxy
 * before
 * Hub realization and bind it before execution. Neither adapter is reusable.
 */
bool sn0_origin2000_hub_outgoing_io_init(SN0Origin2000HubOutgoingIO *io,
                                        Object *owner, SN0NodeState *node,
                                        MemoryRegion *packet_ingress,
                                        Error **errp);
void sn0_origin2000_hub_outgoing_io_destroy(SN0Origin2000HubOutgoingIO *io);

#endif
