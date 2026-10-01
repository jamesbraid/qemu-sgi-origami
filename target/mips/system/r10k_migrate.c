/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * R10000-family migration state
 *
 * The state the R10000 family adds lives behind env->r10k, so it travels as
 * a pointed-to sub-description rather than as fields of the shared CPU state.
 * The base subsection carries the state every family member has. Additional
 * subsections carry the larger secondary-cache ranges selected from the same
 * effective capacity used by diagnostic operations.
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
 * prediction table its operation fields read and write. The postponed Watch
 * exception is invisible in the architectural registers, so it travels too.
 * A watched access under EXL is owed an exception after migration.
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

static uint32_t mips_r10k_scache_capacity(const MIPSCPU *cpu)
{
    return cpu->env.r10k ?
        r10k_scache_capacity(cpu->env.cfg.scache_size,
            extract32(cpu->env.CP0_Config0, CP0C0_R10K_SS, 3)) : 0;
}

static bool mips_r12000_profile_active(const MIPSCPU *cpu)
{
    return cpu->r10k_profile && cpu->r10k_profile->valid &&
           !strcmp(MIPS_CPU_GET_CLASS(cpu)->cpu_def->name, "R12000");
}

static bool mips_r14k_scache_needed(void *opaque)
{
    const MIPSCPU *cpu = opaque;

    return !mips_r12000_profile_active(cpu) &&
           mips_r10k_scache_capacity(cpu) > R10K_SCACHE_SIZE;
}

static bool mips_r12000_extra_needed(void *opaque)
{
    return mips_r12000_profile_active(opaque);
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
    const MIPSCPU *cpu = opaque;

    return mips_r10k_scache_capacity(cpu) > R14K_SCACHE_SIZE;
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

/* The secondary-cache range from four to eight megabytes. */
static const VMStateDescription vmstate_r10k_scache_8m = {
    .name = "mips/r10k-scache-8m",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(scache_taglo, R10KState,
                                 R10K_SCACHE_4M_TAGS,
                                 R10K_SCACHE_MAX_TAGS / 2 -
                                 R10K_SCACHE_4M_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_taghi, R10KState,
                                R10K_SCACHE_4M_TAGS,
                                R10K_SCACHE_MAX_TAGS / 2 -
                                R10K_SCACHE_4M_TAGS),
        VMSTATE_UINT8_SUB_ARRAY(scache_mru, R10KState,
                                R10K_SCACHE_4M_SETS,
                                R10K_SCACHE_MAX_SETS / 2 -
                                R10K_SCACHE_4M_SETS),
        VMSTATE_UINT64_SUB_ARRAY(scache_data, R10KState,
                                 R10K_SCACHE_4M_DWORDS,
                                 R10K_SCACHE_MAX_DWORDS / 2 -
                                 R10K_SCACHE_4M_DWORDS),
        VMSTATE_UINT16_SUB_ARRAY(scache_ecc, R10KState,
                                 R10K_SCACHE_4M_QUADWORDS,
                                 R10K_SCACHE_MAX_QUADWORDS / 2 -
                                 R10K_SCACHE_4M_QUADWORDS),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r10k_scache_8m_needed(void *opaque)
{
    const MIPSCPU *cpu = opaque;

    return mips_r10k_scache_capacity(cpu) > R10K_SCACHE_4M_SIZE;
}

const VMStateDescription mips_vmstate_r10k_scache_8m = {
    .name = "cpu/r10k-scache-8m",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r10k_scache_8m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(env.r10k, MIPSCPU, vmstate_r10k_scache_8m,
                               R10KState),
        VMSTATE_END_OF_LIST()
    }
};

/* The secondary-cache range from eight to sixteen megabytes. */
static const VMStateDescription vmstate_r10k_scache_16m = {
    .name = "mips/r10k-scache-16m",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_SUB_ARRAY(scache_taglo, R10KState,
                                 R10K_SCACHE_MAX_TAGS / 2,
                                 R10K_SCACHE_MAX_TAGS / 2),
        VMSTATE_UINT8_SUB_ARRAY(scache_taghi, R10KState,
                                R10K_SCACHE_MAX_TAGS / 2,
                                R10K_SCACHE_MAX_TAGS / 2),
        VMSTATE_UINT8_SUB_ARRAY(scache_mru, R10KState,
                                R10K_SCACHE_MAX_SETS / 2,
                                R10K_SCACHE_MAX_SETS / 2),
        VMSTATE_UINT64_SUB_ARRAY(scache_data, R10KState,
                                 R10K_SCACHE_MAX_DWORDS / 2,
                                 R10K_SCACHE_MAX_DWORDS / 2),
        VMSTATE_UINT16_SUB_ARRAY(scache_ecc, R10KState,
                                 R10K_SCACHE_MAX_QUADWORDS / 2,
                                 R10K_SCACHE_MAX_QUADWORDS / 2),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r10k_scache_16m_needed(void *opaque)
{
    const MIPSCPU *cpu = opaque;

    return mips_r10k_scache_capacity(cpu) > R10K_SCACHE_MAX_SIZE / 2;
}

const VMStateDescription mips_vmstate_r10k_scache_16m = {
    .name = "cpu/r10k-scache-16m",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r10k_scache_16m_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(env.r10k, MIPSCPU, vmstate_r10k_scache_16m,
                               R10KState),
        VMSTATE_END_OF_LIST()
    }
};

static int mips_r16000_profile_post_load(void *opaque, int version_id)
{
    R10KCPUProfile *profile = opaque;

    if (!profile->r16000_experimental || !profile->r16000_reset_mode_set ||
        !profile->r16000_scache_bytes_set ||
        profile->migration_r16000_reset_mode != profile->r16000_reset_mode ||
        profile->migration_r16000_scache_bytes !=
        profile->r16000_scache_bytes) {
        return -EINVAL;
    }
    profile->migration_r16000_profile_seen = true;
    return 0;
}

static int mips_r16000_profile_pre_load(void *opaque)
{
    MIPSCPU *cpu = opaque;

    if (strcmp(MIPS_CPU_GET_CLASS(cpu)->cpu_def->name, "R16000") ||
        !cpu->r10k_profile || !cpu->r10k_profile->r16000_experimental) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_r16000_profile = {
    .name = "mips/r16000-profile",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mips_r16000_profile_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(migration_r16000_reset_mode, R10KCPUProfile),
        VMSTATE_UINT32(migration_r16000_scache_bytes, R10KCPUProfile),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r16000_profile_needed(void *opaque)
{
    MIPSCPU *cpu = opaque;

    return cpu->r10k_profile && cpu->r10k_profile->r16000_experimental;
}

const VMStateDescription mips_vmstate_r16000_profile = {
    .name = "cpu/r16000-profile",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = mips_r16000_profile_pre_load,
    .needed = mips_r16000_profile_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(r10k_profile, MIPSCPU,
                               vmstate_r16000_profile, R10KCPUProfile),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_r12000_profile = {
    .name = "mips/r12000-profile",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_ARRAY(migration_profile, R10KCPUProfile,
                             R12000_PROFILE_FIELDS),
        VMSTATE_END_OF_LIST()
    }
};

static bool mips_r12000_profile_needed(void *opaque)
{
    return mips_r12000_profile_active(opaque);
}

const VMStateDescription mips_vmstate_r12000_profile = {
    .name = "cpu/r12000-profile",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mips_r12000_profile_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(r10k_profile, MIPSCPU,
                               vmstate_r12000_profile, R10KCPUProfile),
        VMSTATE_END_OF_LIST()
    }
};

void mips_r10k_profile_pre_save(MIPSCPU *cpu)
{
    R10KCPUProfile *profile = cpu->r10k_profile;

    if (mips_r12000_profile_active(cpu)) {
        memcpy(profile->migration_profile, profile->values,
               sizeof(profile->values));
    }
    if (profile && profile->r16000_experimental) {
        profile->migration_r16000_reset_mode = profile->r16000_reset_mode;
        profile->migration_r16000_scache_bytes = profile->r16000_scache_bytes;
    }
}

void mips_r10k_profile_pre_load(MIPSCPU *cpu)
{
    if (cpu->r10k_profile) {
        cpu->r10k_profile->migration_r16000_profile_seen = false;
    }
}

int mips_r10k_profile_post_load(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;
    R10KCPUProfile *profile = cpu->r10k_profile;

    /* PRId is immutable configuration, including the value used by reset. */
    if (!strcmp(MIPS_CPU_GET_CLASS(cpu)->cpu_def->name, "R10000") &&
        env->CP0_PRid != profile->r10000_prid) {
        return -EINVAL;
    }
    if (!strcmp(MIPS_CPU_GET_CLASS(cpu)->cpu_def->name, "R16000") &&
        (!profile || !profile->r16000_experimental ||
         !profile->migration_r16000_profile_seen ||
         env->CP0_PRid != 0x00000F50 || env->active_fpu.fcr0 != 0x00000900 ||
         (env->CP0_Config0 & env->cpu_model->CP0_Config0_preserve_bitmask) !=
         (env->cpu_model->CP0_Config0 &
          env->cpu_model->CP0_Config0_preserve_bitmask))) {
        return -EINVAL;
    }
    if (mips_r12000_profile_active(cpu) &&
        memcmp(profile->migration_profile, profile->values,
               sizeof(profile->values))) {
        return -EINVAL;
    }

    return 0;
}
