/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_R10K_COHERENCE_H
#define HW_MIPS_R10K_COHERENCE_H

#include "qemu/typedefs.h"

/* Secondary CACHE bus request operations. */
typedef enum R10KSystemRequest {
    R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE,
    R10K_REQUEST_HIT_WRITEBACK_INVALIDATE,
    R10K_REQUEST_HIT_INVALIDATE,
    R10K_REQUEST_PREFETCH_LOAD,
    R10K_REQUEST_PREFETCH_STORE,
} R10KSystemRequest;

typedef bool (*R10KBusSystemRequestFn)(void *opaque, uint16_t requester_id,
                                      R10KSystemRequest request,
                                      hwaddr paddr, unsigned int set_bits);

#endif
