/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MISC_SGI_VICE_MSP_H
#define HW_MISC_SGI_VICE_MSP_H

#include "hw/core/clock.h"
#include "migration/vmstate.h"

typedef enum SGIViceMspIssue {
    VICE_MSP_SCALAR,
    VICE_MSP_BRANCH,
    VICE_MSP_VECTOR_SCALAR,
    VICE_MSP_VECTOR_COMPUTE,
    VICE_MSP_UNCLASSIFIED,
} SGIViceMspIssue;

/* A delay group and a leading branch always consume a single word. */
static inline unsigned sgi_vice_msp_group(SGIViceMspIssue first,
                                         SGIViceMspIssue second, bool delay)
{
    if (delay || first == VICE_MSP_BRANCH ||
        first == VICE_MSP_UNCLASSIFIED || second == VICE_MSP_UNCLASSIFIED) {
        return 1;
    }
    return (first == VICE_MSP_VECTOR_COMPUTE) !=
           (second == VICE_MSP_VECTOR_COMPUTE) ? 2 : 1;
}

static inline bool sgi_vice_msp_interlock_match(uint32_t word, unsigned dest)
{
    unsigned op = word >> 26;

    return ((word >> 21) & 31) == dest ||
           (!(op >= 8 && op <= 15) && op != 0x32 && op != 0x3a &&
            ((word >> 16) & 31) == dest);
}

typedef struct SGIViceMspOps {
    bool (*fetch)(void *opaque, uint16_t pc, uint32_t words[2]);
    bool (*local)(void *opaque, uint32_t address, uint32_t *value,
                  unsigned size, bool write);
    bool (*common)(void *opaque, unsigned slot, uint32_t *value, bool write);
    uint32_t (*grants)(void *opaque);
    bool (*admitted)(void *opaque);
    void (*held)(void *opaque, const char *reason, uint32_t pc);
    void (*exception)(void *opaque);
} SGIViceMspOps;

typedef struct SGIViceMspStage {
    uint32_t word;
    uint32_t a;
    uint32_t b;
    uint32_t result;
    uint32_t address;
    uint32_t sources;
    uint16_t pc;
    uint16_t target;
    uint16_t common_slot;
    uint8_t kind;
    uint8_t dest;
    uint8_t size;
    uint8_t faults;
    bool valid;
    bool delay;
    bool writes;
    bool loadlike;
    bool result_ready;
    bool ex_done;
    bool ls_done;
    bool wb_done;
} SGIViceMspStage;

typedef struct SGIViceMsp {
    const SGIViceMspOps *ops;
    void *opaque;
    Clock *clock;
    uint32_t gpr[32];
    uint32_t words[2];
    uint16_t pcs[2];
    bool fetch_fault[2];
    uint8_t count;
    uint16_t fetch_pc;
    uint16_t decode_pc;
    uint32_t bad_address;
    uint32_t watch;
    uint16_t epc;
    uint32_t cause;
    uint8_t flags;
    bool reset_held;
    int64_t reset_start;
    bool fresh_pc;
    bool running;
    bool draining;
    bool fault_drain;
    bool model_blocked;
    bool pending;
    int64_t next_ns;
    uint32_t next_fraction;
    uint64_t cycles;
    SGIViceMspStage ex;
    SGIViceMspStage ls;
    SGIViceMspStage wb;
    uint8_t last_dest;
    uint8_t previous_dest;
    uint8_t interlock;
    bool waiting_target;
    uint8_t target_reg;
    uint16_t waiting_pc;
    bool branch_valid;
    bool branch_resolved;
    bool branch_taken;
    uint16_t branch_pc;
    uint16_t branch_target;
    uint16_t branch_sequential;
    bool delay_pending;
    uint16_t delay_pc;
    uint8_t penalty;
    int8_t reserved_bank;
    int64_t reservation_start_ns;
    uint32_t reservation_start_fraction;
    int64_t reservation_end_ns;
    uint32_t reservation_end_fraction;
} SGIViceMsp;

void sgi_vice_msp_init(SGIViceMsp *m, const SGIViceMspOps *ops,
                       void *opaque, Clock *clock);
void sgi_vice_msp_reset(SGIViceMsp *m);
void sgi_vice_msp_tick(SGIViceMsp *m);
bool sgi_vice_msp_host_read(SGIViceMsp *m, unsigned slot, uint32_t *value);
bool sgi_vice_msp_host_write(SGIViceMsp *m, unsigned slot, uint32_t value);
bool sgi_vice_msp_reserved(SGIViceMsp *m, unsigned bank,
                           int64_t ns, uint32_t fraction);
bool sgi_vice_msp_rebase(SGIViceMsp *m, int64_t delta);
extern const VMStateDescription vmstate_sgi_vice_msp;

#endif
