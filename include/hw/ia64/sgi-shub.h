/* SPDX-License-Identifier: BSD-3-Clause */
/* SGI SHub 1 node controller, local and global MMR windows. */
#ifndef HW_IA64_SGI_SHUB_H
#define HW_IA64_SGI_SHUB_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SGI_SHUB "sgi-shub"
OBJECT_DECLARE_SIMPLE_TYPE(SGIShubState, SGI_SHUB)

/* SHub 1 local MMR window: node field value 2 at the 38-bit NASID shift. */
#define SGI_SHUB_LOCAL_MMR_BASE 0x8000000000ULL
#define SGI_SHUB_MMR_SIZE 0x800000000ULL

/* Global window of a node: bit 35 plus the NASID at bit 38. */
#define SGI_SHUB_GLOBAL_MMR_BASE 0x800000000ULL
#define SGI_SHUB_NASID_SHIFT 38

/* Node memory in the cacheable address space: AS 3 at bit 36, NASID 0. */
#define SGI_SHUB_CACHEABLE_BASE 0x3000000000ULL

/* NASID 0, revision 3, as the PROM's reads need; see the manual page. */
#define SGI_SHUB_ID_DEFAULT 0x30000000ULL

typedef struct SGIShubSlot {
    uint64_t value;
    bool valid;
} SGIShubSlot;

/*
 * Plain 64-bit storage slots. Slots before SHUB_SLOT_HIST have no known
 * reset value and stop on a read before the first write; the rest reset to 0.
 */
enum {
    SHUB_SLOT_PRIV0,
    SHUB_SLOT_PRIV1,
    SHUB_SLOT_IPI_ACCESS,
    SHUB_SLOT_RTC_CTL,
    SHUB_SLOT_HIST,
    SHUB_SLOT_FW_1540 = SHUB_SLOT_HIST + 4,
    SHUB_SLOT_CPU_BITS,
    SHUB_SLOT_FW_DEBUG,
    SHUB_SLOT_PI_2001_0100,
    SHUB_SLOT_L1_STATE,
    SHUB_SLOT_ELECT,
    SHUB_SLOT_MD_BANK_CFG,
    SHUB_SLOT_MD_TIMING_2 = SHUB_SLOT_MD_BANK_CFG + 3,
    SHUB_SLOT_MD_TIMING_3,
    SHUB_SLOT_MD_REFRESH_1,
    SHUB_SLOT_MD_0068,
    SHUB_SLOT_MD_0003_0000,
    SHUB_SLOT_MD_0005_0000,
    SHUB_SLOT_MBIST_STATUS,
    SHUB_SLOT_MBIST_CMD,
    SHUB_SLOT_MBIST_MASK,
    SHUB_SLOT_MBIST_CTL,
    SHUB_SLOT_BTE_LEN,
    SHUB_SLOT_BTE_SRC = SHUB_SLOT_BTE_LEN + 2,
    SHUB_SLOT_BTE_DST = SHUB_SLOT_BTE_SRC + 2,
    SHUB_SLOT_BTE_CTL = SHUB_SLOT_BTE_DST + 2,
    SHUB_SLOT_BTE_NOTIFY = SHUB_SLOT_BTE_CTL + 2,
    SHUB_SLOT_BTE_INT = SHUB_SLOT_BTE_NOTIFY + 2,
    SHUB_SLOT_COUNT = SHUB_SLOT_BTE_INT + 2,
};

struct SGIShubState {
    SysBusDevice parent_obj;

    MemoryRegion local_mmr;
    MemoryRegion global_mmr;

    /* Property: the SHub ID register's reset value. */
    uint64_t shub_id_reset;

    uint64_t shub_id;
    int64_t rtc_base_ns;
    uint64_t rtc_base_count;
    SGIShubSlot slot[SHUB_SLOT_COUNT];
    uint64_t rtc_int_config[3];
    uint64_t int_cmp[3];
    uint16_t led[4];
};

#endif
