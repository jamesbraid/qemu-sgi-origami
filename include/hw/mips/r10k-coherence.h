/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_R10K_COHERENCE_H
#define HW_MIPS_R10K_COHERENCE_H

#include "qemu/typedefs.h"

/* Keep the existing secondary CACHE and prefetch transport values. */
typedef enum R10KSystemRequest {
    R10K_REQUEST_INDEX_WRITEBACK_INVALIDATE,
    R10K_REQUEST_HIT_WRITEBACK_INVALIDATE,
    R10K_REQUEST_HIT_INVALIDATE,
    R10K_REQUEST_PREFETCH_LOAD,
    R10K_REQUEST_PREFETCH_STORE,
} R10KSystemRequest;

#define R10K_SYSTEM_REQUEST_PADDR_BITS 48
#define R10K_SYSTEM_REQUEST_KIND_SHIFT 48
#define R10K_SYSTEM_REQUEST_SET_BITS_SHIFT 52
#define R10K_SYSTEM_REQUEST_SIZE (UINT64_C(1) << 58)

#endif
