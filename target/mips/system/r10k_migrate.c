/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * R10000-family migration state
 *
 * The state the R10000 family adds lives behind env->r10k, so it travels as
 * a pointed-to sub-description rather than as fields of the shared CPU state.
 * One subsection carries the base state every family member has; two more
 * carry the extra secondary-cache capacity of the larger caches, gated on the
 * same cache-size tests the flat subsections used before the state moved.
 *
 */

#include "qemu/osdep.h"
#include "cpu.h"
#include "internal.h"
#include "migration/cpu.h"

/*
 * The base R10000 state: the first megabyte of the secondary-cache diagnostic
 * arrays, the primary caches, and the CP0 registers the family defines. The
 * Diagnostic register is 64 bits wide and travels whole, along with the branch
 * prediction table its operation fields read and write. The CacheErr register
 * is loaded only by a Cache Error exception, which nothing raises yet, so it
 * carries zero until that delivery path exists. The postponed Watch exception
 * is invisible in the architectural registers, so it travels too: a guest that
 * referenced its watched doubleword under EXL is owed a Watch on the far side.
 */
static const VMStateDescription vmstate_r10k_state = {
    .name = "mips/r10k",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(scache_taglo, R10KState, 0, R10K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_taghi, R10KState, 0, R10K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_mru, R10KState, 0, R10K_SCACHE_SETS),
        VMSTATE_UINT64_SUB_ARRAY(scache_data, R10KState, 0, R10K_SCACHE_DWORDS),
        VMSTATE_UINT16_SUB_ARRAY(scache_ecc, R10KState, 0,
                                 R10K_SCACHE_QUADWORDS),
        VMSTATE_UINT32_ARRAY(icache_taglo, R10KState, R10K_ICACHE_TAGS),
        VMSTATE_UINT8_ARRAY(icache_taghi, R10KState, R10K_ICACHE_TAGS),
        VMSTATE_UINT8_ARRAY(icache_lru, R10KState, R10K_ICACHE_SETS),
        VMSTATE_UINT64_ARRAY(icache_data, R10KState, R10K_PCACHE_WORDS),
        VMSTATE_UINT8_ARRAY(icache_parity, R10KState, R10K_PCACHE_WORDS),
        VMSTATE_UINT32_ARRAY(dcache_taglo, R10KState, R10K_DCACHE_TAGS),
        VMSTATE_UINT32_ARRAY(dcache_taghi, R10KState, R10K_DCACHE_TAGS),
        VMSTATE_UINT8_ARRAY(dcache_lru, R10KState, R10K_DCACHE_SETS),
        VMSTATE_UINT32_ARRAY(dcache_data, R10KState, R10K_PCACHE_WORDS),
        VMSTATE_UINT8_ARRAY(dcache_parity, R10KState, R10K_PCACHE_WORDS),
        VMSTATE_INT32(CP0_CacheErr, R10KState),
        VMSTATE_UINT64(CP0_Diagnostic, R10KState),
        VMSTATE_UINT8_SUB_ARRAY(bpt, R10KState, 0, R10K_BPT_ENTRIES),
        VMSTATE_UINT32(CP0_R10K_DelayedWatch, R10KState),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r10k_needed(void *opaque)
{
    MIPSCPU *cpu = opaque;

    return cpu->env.r10k != NULL;
}

const VMStateDescription mips_vmstate_r10k = {
    .name = "cpu/r10k",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r10k_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(env.r10k, MIPSCPU, vmstate_r10k_state,
                               R10KState),
        VMSTATE_END_OF_LIST()
    }
};

/* The megabyte of secondary cache above the R10000's, present on the R14000. */
static const VMStateDescription vmstate_r10k_scache_r14k = {
    .name = "mips/r14k-scache",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(scache_taglo, R10KState, R10K_SCACHE_TAGS,
                                 R14K_SCACHE_TAGS - R10K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_taghi, R10KState, R10K_SCACHE_TAGS,
                                R14K_SCACHE_TAGS - R10K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_mru, R10KState, R10K_SCACHE_SETS,
                                R14K_SCACHE_SETS - R10K_SCACHE_SETS),
        VMSTATE_UINT64_SUB_ARRAY(scache_data, R10KState, R10K_SCACHE_DWORDS,
                                 R14K_SCACHE_DWORDS - R10K_SCACHE_DWORDS),
        VMSTATE_UINT16_SUB_ARRAY(scache_ecc, R10KState, R10K_SCACHE_QUADWORDS,
                                 R14K_SCACHE_QUADWORDS - R10K_SCACHE_QUADWORDS),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r10k_scache_is_4m(const MIPSCPU *cpu)
{
    return cpu->env.cfg.scache_size == R10K_SCACHE_SIZE &&
           extract32(cpu->env.CP0_Config0, CP0C0_R10K_SS, 3) == 3;
}

static bool mips_r14k_scache_needed(void *opaque)
{
    const MIPSCPU *cpu = opaque;

    return (!cpu->r12000_profile_valid &&
            cpu->env.cfg.scache_size == R14K_SCACHE_SIZE) ||
           mips_r10k_scache_is_4m(cpu);
}

static bool mips_r12000_extra_needed(void *opaque)
{
    return ((MIPSCPU *)opaque)->r12000_profile_valid;
}

static const VMStateDescription vmstate_r12000_extra = {
    .name = "mips/r12000-extra",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(scache_taglo, R10KState, R10K_SCACHE_TAGS,
                                 R10K_SCACHE_4M_TAGS - R10K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_taghi, R10KState, R10K_SCACHE_TAGS,
                                R10K_SCACHE_4M_TAGS - R10K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_mru, R10KState, R10K_SCACHE_SETS,
                                R10K_SCACHE_4M_SETS - R10K_SCACHE_SETS),
        VMSTATE_UINT64_SUB_ARRAY(scache_data, R10KState, R10K_SCACHE_DWORDS,
                                 R14K_SCACHE_DWORDS - R10K_SCACHE_DWORDS),
        VMSTATE_UINT16_SUB_ARRAY(scache_ecc, R10KState,
                                 R10K_SCACHE_QUADWORDS,
                                 R14K_SCACHE_QUADWORDS - R10K_SCACHE_QUADWORDS),
        VMSTATE_UINT8_SUB_ARRAY(bpt, R10KState, R10K_BPT_ENTRIES,
                                R12K_BPT_ENTRIES - R10K_BPT_ENTRIES),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription mips_vmstate_r12000_extra = {
    .name = "cpu/r12000-extra",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r12000_extra_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(env.r10k, MIPSCPU, vmstate_r12000_extra,
                               R10KState),
        VMSTATE_END_OF_LIST()
    }
};

const VMStateDescription mips_vmstate_r14k_scache = {
    .name = "cpu/r14k-scache",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r14k_scache_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(env.r10k, MIPSCPU, vmstate_r10k_scache_r14k,
                               R10KState),
        VMSTATE_END_OF_LIST()
    }
};

/* The top two megabytes of a four-megabyte secondary cache. */
static const VMStateDescription vmstate_r10k_scache_4m = {
    .name = "mips/r10k-scache-4m",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(scache_taglo, R10KState, R14K_SCACHE_TAGS,
                                 R10K_SCACHE_4M_TAGS - R14K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_taghi, R10KState, R14K_SCACHE_TAGS,
                                R10K_SCACHE_4M_TAGS - R14K_SCACHE_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_mru, R10KState, R14K_SCACHE_SETS,
                                R10K_SCACHE_4M_SETS - R14K_SCACHE_SETS),
        VMSTATE_UINT64_SUB_ARRAY(scache_data, R10KState, R14K_SCACHE_DWORDS,
                                 R10K_SCACHE_4M_DWORDS - R14K_SCACHE_DWORDS),
        VMSTATE_UINT16_SUB_ARRAY(scache_ecc, R10KState, R14K_SCACHE_QUADWORDS,
                                 R10K_SCACHE_4M_QUADWORDS -
                                 R14K_SCACHE_QUADWORDS),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r10k_scache_4m_needed(void *opaque)
{
    return mips_r10k_scache_is_4m(opaque);
}

const VMStateDescription mips_vmstate_r10k_scache_4m = {
    .name = "cpu/r10k-scache-4m",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r10k_scache_4m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(env.r10k, MIPSCPU, vmstate_r10k_scache_4m,
                               R10KState),
        VMSTATE_END_OF_LIST()
    }
};

/* Older destinations reject this unknown subsection and its dirty data. */
static bool mips_r10k_data_cache_needed(void *opaque)
{
    return MIPS_CPU(opaque)->r10k_data_cache;
}

static int mips_r10k_data_cache_post_load(void *opaque, int version_id)
{
    MIPSCPU *cpu = MIPS_CPU(opaque);

    if (!cpu->r10k_data_cache) {
        return -EINVAL;
    }
    cpu->r10k_data_cache_stream = true;
    return 0;
}

const VMStateDescription mips_vmstate_r10k_data_cache = {
    .name = "cpu/functional-data-cache",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r10k_data_cache_needed,
    .post_load = mips_r10k_data_cache_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    }
};
