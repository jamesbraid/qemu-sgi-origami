/* SPDX-License-Identifier: GPL-2.0-or-later */
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
    R10K_REQUEST_READ_NONCOHERENT,
    R10K_REQUEST_READ_SHARED,
    R10K_REQUEST_READ_EXCLUSIVE,
    R10K_REQUEST_UPGRADE,
    R10K_REQUEST_WRITEBACK,
} R10KSystemRequest;

#define R10K_SYSTEM_REQUEST_PADDR_BITS 48
#define R10K_SYSTEM_REQUEST_KIND_SHIFT 48
#define R10K_SYSTEM_REQUEST_SET_BITS_SHIFT 52
#define R10K_SYSTEM_REQUEST_SIZE (UINT64_C(1) << 58)
#define R10K_COHERENCE_BLOCK_MAX 128

typedef enum R10KCoherenceState {
    R10K_INVALID,
    R10K_SHARED,
    R10K_CLEAN_EXCLUSIVE,
    R10K_DIRTY_EXCLUSIVE,
} R10KCoherenceState;

typedef enum R10KRequestPhase {
    R10K_REQUEST_FREE,
    R10K_REQUEST_PENDING_ISSUE,
    R10K_REQUEST_PENDING_RESPONSE,
    R10K_REQUEST_COMPLETE,
    R10K_REQUEST_TRANSFERRING,
} R10KRequestPhase;

typedef enum R10KCompletion {
    R10K_ACK,
    R10K_NACK,
    R10K_ERR,
} R10KCompletion;

typedef enum R10KIntervention {
    R10K_INTERVENTION_SHARED,
    R10K_INTERVENTION_EXCLUSIVE,
    R10K_INTERVENTION_INVALIDATE,
} R10KIntervention;

typedef enum R10KRequestCause {
    R10K_CAUSE_LOAD,
    R10K_CAUSE_STORE,
    R10K_CAUSE_REPLACEMENT,
    R10K_CAUSE_CACHE,
} R10KRequestCause;

typedef struct R10KTransaction {
    uint64_t id;
    uint64_t address;
    uint64_t block;
    unsigned int size;
    unsigned int tag;
    unsigned int pidx;
    unsigned int cca;
    bool store;
    R10KSystemRequest command;
    R10KRequestCause cause;
    R10KCoherenceState former;
    R10KRequestPhase phase;
    R10KCompletion completion;
    R10KCoherenceState response_state;
    bool have_data;
    bool need_data;
    uint8_t data[R10K_COHERENCE_BLOCK_MAX];
} R10KTransaction;

typedef struct R10KExternalTransaction {
    uint64_t id;
    uint64_t block;
    unsigned int size;
    R10KCoherenceState former;
    R10KCoherenceState data_state;
    bool have_data;
    bool consumed;
    uint8_t data[R10K_COHERENCE_BLOCK_MAX];
} R10KExternalTransaction;

/*
 * These operations require the BQL. Data arguments are copied in full.
 * No caller may retain a pointer into CPU state across an operation.
 */
bool mips_r10k_request_snapshot(CPUState *cpu, bool writeback,
                                R10KTransaction *out);
bool mips_r10k_request_issue(CPUState *cpu, uint64_t id);
bool mips_r10k_response_data(CPUState *cpu, uint64_t id,
                             R10KCoherenceState state,
                             const uint8_t *data, unsigned int size);
bool mips_r10k_response_complete(CPUState *cpu, uint64_t id,
                                 R10KCompletion completion);
bool mips_r10k_writeback_consumed(CPUState *cpu, uint64_t id);
bool mips_r10k_external_request(CPUState *cpu, uint64_t id, uint64_t address,
                                R10KIntervention kind,
                                R10KExternalTransaction *out);
bool mips_r10k_external_consumed(CPUState *cpu, uint64_t id);
bool mips_r10k_external_complete(CPUState *cpu, uint64_t id);
void mips_r10k_global_performed(CPUState *cpu, bool performed);
bool mips_r10k_attach_agent(CPUState *cpu, MemoryRegion *requests,
                            Error **errp);
void mips_r10k_detach_agent(CPUState *cpu, MemoryRegion *requests);

#endif
