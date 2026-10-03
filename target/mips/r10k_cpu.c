/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* R10000-family properties, model ownership and CPU lifecycle. */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "cpu.h"
#include "internal.h"

static void mips_r10000_prid_get(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    R10KCPUProfile *profile = MIPS_CPU(obj)->r10k_profile;
    uint32_t value = profile ? profile->r10000_prid : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void mips_r10000_prid_set(Object *obj, Visitor *v, const char *name,
                                 void *opaque, Error **errp)
{
    MIPSCPU *cpu = MIPS_CPU(obj);
    uint32_t value;

    if (DEVICE(obj)->realized) {
        error_setg(errp, "%s cannot change after CPU realization", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if ((value & ~0xffu) != 0x0900) {
        error_setg(errp, "%s requires implementation 0x09 and zero upper bits",
                   name);
        return;
    }
    if (!cpu->r10k_profile) {
        error_setg(errp, "%s unavailable on this CPU model", name);
        return;
    }
    cpu->r10k_profile->r10000_prid = value;
}

static const char *const r12000_property_names[R12000_PROFILE_FIELDS] = {
    "r12000-prid", "r12000-fpu-id", "r12000-reset-mode",
    "r12000-scache-bytes", "r12000-scache-block-words",
};

static void mips_r12000_property_get(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    MIPSCPU *cpu = MIPS_CPU(obj);
    unsigned int field = (uintptr_t)opaque;
    uint32_t value = cpu->r10k_profile ? cpu->r10k_profile->values[field] : 0;

    visit_type_uint32(v, name, &value, errp);
}

static void mips_r12000_property_set(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    MIPSCPU *cpu = MIPS_CPU(obj);
    unsigned int field = (uintptr_t)opaque;
    uint32_t value;

    if (DEVICE(obj)->realized) {
        error_setg(errp, "%s cannot change after CPU realization", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!cpu->r10k_profile) {
        error_setg(errp, "%s unavailable on this CPU model", name);
        return;
    }
    cpu->r10k_profile->values[field] = value;
    cpu->r10k_profile->set[field] = true;
}

static bool mips_r12000_validate(MIPSCPU *cpu, Error **errp)
{
    R10KCPUProfile *profile = cpu->r10k_profile;
    uint32_t mode, bytes, words, ec, sc, k0, dsd;

    for (unsigned int i = 0; i < R12000_PROFILE_FIELDS; i++) {
        if (!profile->set[i]) {
            error_setg(errp, "R12000 requires %s", r12000_property_names[i]);
            return false;
        }
    }
    mode = profile->values[R12000_RESET_MODE];
    bytes = profile->values[R12000_SCACHE_BYTES];
    words = profile->values[R12000_SCACHE_BLOCK_WORDS];
    ec = (mode >> CP0C0_R10K_EC) & 15;
    sc = (mode >> CP0C0_R10K_SC) & 7;
    k0 = mode & 7;
    dsd = (mode >> 22) & 7;
    if ((profile->values[R12000_PRID] & 0xff00) != 0x0e00) {
        error_setg(errp, "r12000-prid requires implementation 0x0e");
    } else if (mode & ~0x01ffffffu) {
        error_setg(errp, "r12000-reset-mode exceeds 25 bits");
    } else if (k0 < 2 || k0 == 6 || ec < 3 || ec > 11 ||
               sc < 2 || sc == 6 || (dsd != 0 && dsd != 4)) {
        error_setg(errp, "r12000-reset-mode has a reserved encoding");
    } else if (((mode >> CP0C0_BE) & 1) != cpu->is_big_endian) {
        error_setg(errp, "r12000-reset-mode endianness differs from CPU");
    } else if ((bytes != 1024 * 1024 && bytes != 2 * 1024 * 1024) ||
               ((mode >> CP0C0_R10K_SS) & 7) != (bytes >> 20)) {
        error_setg(errp, "r12000-scache-bytes disagrees with Config SS");
    } else if ((words != 16 && words != 32) ||
               ((mode >> CP0C0_R10K_SB) & 1) != (words == 32)) {
        error_setg(errp, "r12000-scache-block-words disagrees with Config SB");
    } else {
        profile->cpu_model = g_new(mips_def_t, 1);
        *profile->cpu_model = *cpu->env.cpu_model;
        profile->cpu_model->CP0_PRid =
            profile->values[R12000_PRID];
        profile->cpu_model->CP1_fcr0 =
            profile->values[R12000_FPU_ID];
        profile->cpu_model->CP0_Config0 |= mode;
        profile->cpu_model->cfg.scache_size = bytes;
        profile->cpu_model->cfg.scache_line_size = words * 4;
        cpu->env.cpu_model = profile->cpu_model;
        profile->valid = true;
        return true;
    }
    return false;
}

static void mips_r16000_experimental_get(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    R10KCPUProfile *profile = MIPS_CPU(obj)->r10k_profile;
    bool value = profile ? profile->r16000_experimental : false;

    visit_type_bool(v, name, &value, errp);
}

static void mips_r16000_experimental_set(Object *obj, Visitor *v,
                                         const char *name, void *opaque,
                                         Error **errp)
{
    MIPSCPU *cpu = MIPS_CPU(obj);
    bool value;

    if (DEVICE(obj)->realized) {
        error_setg(errp, "%s cannot change after CPU realization", name);
        return;
    }
    if (!visit_type_bool(v, name, &value, errp)) {
        return;
    }
    if (!cpu->r10k_profile) {
        error_setg(errp, "%s unavailable on this CPU model", name);
        return;
    }
    cpu->r10k_profile->r16000_experimental = value;
}

static const char *const r16000_property_names[] = {
    "r16000-reset-mode", "r16000-scache-bytes",
};

static void mips_r16000_property_get(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    R10KCPUProfile *profile = MIPS_CPU(obj)->r10k_profile;
    unsigned int field = (uintptr_t)opaque;
    uint32_t value = 0;

    if (profile) {
        value = field == 0 ? profile->r16000_reset_mode :
                             profile->r16000_scache_bytes;
    }
    visit_type_uint32(v, name, &value, errp);
}

static void mips_r16000_property_set(Object *obj, Visitor *v,
                                     const char *name, void *opaque,
                                     Error **errp)
{
    MIPSCPU *cpu = MIPS_CPU(obj);
    R10KCPUProfile *profile = cpu->r10k_profile;
    unsigned int field = (uintptr_t)opaque;
    uint32_t value;

    if (DEVICE(obj)->realized) {
        error_setg(errp, "%s cannot change after CPU realization", name);
        return;
    }
    if (!visit_type_uint32(v, name, &value, errp)) {
        return;
    }
    if (!profile) {
        error_setg(errp, "%s unavailable on this CPU model", name);
        return;
    }
    if (field == 0) {
        profile->r16000_reset_mode = value;
        profile->r16000_reset_mode_set = true;
    } else {
        profile->r16000_scache_bytes = value;
        profile->r16000_scache_bytes_set = true;
    }
}

static bool mips_r16000_validate(MIPSCPU *cpu, Error **errp)
{
    R10KCPUProfile *profile = cpu->r10k_profile;
    uint32_t mode, bytes, ss;

    if (!profile->r16000_experimental) {
        error_setg(errp, "R16000 requires r16000-experimental=on");
    } else if (!profile->r16000_reset_mode_set) {
        error_setg(errp, "R16000 requires r16000-reset-mode");
    } else if (!profile->r16000_scache_bytes_set) {
        error_setg(errp, "R16000 requires r16000-scache-bytes");
    } else if (profile->r16000_reset_mode & ~0x01ffffffu) {
        error_setg(errp,
                   "r16000-reset-mode uses only Config0 bits 24:0; "
                   "primary cache sizes are fixed at 32 KiB");
    } else {
        mode = profile->r16000_reset_mode;
        bytes = profile->r16000_scache_bytes;
        if (bytes == 4 * 1024 * 1024) {
            ss = 3;
        } else if (bytes == 8 * 1024 * 1024) {
            ss = 4;
        } else if (bytes == 16 * 1024 * 1024) {
            ss = 5;
        } else {
            error_setg(errp,
                       "r16000-scache-bytes must be 4194304, 8388608, or 16777216");
            return false;
        }
        if (((mode >> CP0C0_R10K_SS) & 7) != ss) {
            error_setg(errp,
                       "r16000-scache-bytes disagrees with Config0 SS");
        } else if (!((mode >> CP0C0_R10K_SB) & 1)) {
            error_setg(errp,
                       "r16000-reset-mode requires Config0 SB=1 for 128-byte lines");
        } else if (((mode >> CP0C0_BE) & 1) != cpu->is_big_endian) {
            error_setg(errp,
                       "r16000-reset-mode endianness differs from CPU");
        } else {
            profile->cpu_model = g_new(mips_def_t, 1);
            *profile->cpu_model = *cpu->env.cpu_model;
            profile->cpu_model->CP0_Config0 |= mode;
            profile->cpu_model->cfg.scache_size = bytes;
            profile->cpu_model->cfg.scache_line_size = 128;
            cpu->env.cpu_model = profile->cpu_model;
            profile->valid = true;
            return true;
        }
    }
    return false;
}

void mips_r10k_cpu_class_init(ObjectClass *oc, const void *data)
{
    const struct mips_def_t *def = data;

    if (!(def->insn_flags & INSN_R10K)) {
        return;
    }
    if (!strcmp(def->name, "R10000")) {
        object_class_property_add(oc, "r10000-prid", "uint32",
                                  mips_r10000_prid_get, mips_r10000_prid_set,
                                  NULL, NULL);
    }
    if (!strcmp(def->name, "R12000")) {
        for (uintptr_t i = 0; i < R12000_PROFILE_FIELDS; i++) {
            object_class_property_add(oc, r12000_property_names[i], "uint32",
                                      mips_r12000_property_get,
                                      mips_r12000_property_set,
                                      NULL, (void *)i);
        }
    }
    if (!strcmp(def->name, "R16000")) {
        object_class_property_add(oc, "r16000-experimental", "bool",
                                  mips_r16000_experimental_get,
                                  mips_r16000_experimental_set, NULL, NULL);
        object_class_property_set_description(oc, "r16000-experimental",
            "Opt in to the CPU-only R16000 compatibility experiment");
        for (uintptr_t i = 0; i < ARRAY_SIZE(r16000_property_names); i++) {
            object_class_property_add(oc, r16000_property_names[i], "uint32",
                                      mips_r16000_property_get,
                                      mips_r16000_property_set,
                                      NULL, (void *)i);
        }
        object_class_property_set_description(oc, "r16000-reset-mode",
            "Explicit board Config0 bits 24:0; primary cache sizes are fixed");
        object_class_property_set_description(oc, "r16000-scache-bytes",
            "Explicit secondary-cache capacity in bytes for the experiment");
    }
}

void mips_r10k_cpu_init(MIPSCPU *cpu)
{
    MIPSCPUClass *mcc = MIPS_CPU_GET_CLASS(cpu);

    if (!(mcc->cpu_def->insn_flags & INSN_R10K)) {
        return;
    }
    cpu->r10k_profile = g_new0(R10KCPUProfile, 1);
    if (!strcmp(mcc->cpu_def->name, "R10000")) {
        cpu->r10k_profile->r10000_prid = mcc->cpu_def->CP0_PRid;
    }
}

bool mips_r10k_cpu_validate(MIPSCPU *cpu, Error **errp)
{
    const char *name = MIPS_CPU_GET_CLASS(cpu)->cpu_def->name;

    if (!strcmp(name, "R12000")) {
        return mips_r12000_validate(cpu, errp);
    }
    if (!strcmp(name, "R16000")) {
        return mips_r16000_validate(cpu, errp);
    }
    return true;
}

void mips_r10k_cpu_realize(MIPSCPU *cpu)
{
    if (cpu->env.cpu_model->insn_flags & INSN_R10K) {
        uint32_t capacity;
        uint32_t alloc_size;
        R10KState *r10k;

        cpu->env.r10k = g_new0(R10KState, 1);
        r10k = cpu->env.r10k;

        capacity = r10k_scache_capacity(cpu->env.cpu_model->cfg.scache_size,
            extract32(cpu->env.cpu_model->CP0_Config0, CP0C0_R10K_SS, 3));
        if (capacity == 0) {
            capacity = R10K_SCACHE_SIZE;
        }
        alloc_size = MAX(capacity, R10K_SCACHE_SIZE);
        r10k->scache_size = capacity;
        r10k->scache_taglo = g_new0(uint32_t, alloc_size / 64);
        r10k->scache_taghi = g_new0(uint8_t, alloc_size / 64);
        r10k->scache_mru = g_new0(uint8_t, alloc_size / 128);
        r10k->scache_data = g_new0(uint64_t, alloc_size / 8);
        r10k->scache_ecc = g_new0(uint16_t, alloc_size / 16);
    }
}

void mips_r10k_cpu_reset(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;

    /* Cold initialization clears diagnostics while preserving board links. */
    if (env->r10k) {
        R10KState *r10k = env->r10k;

        memset(r10k, 0, offsetof(R10KState, bus_request));
        if (r10k->scache_size) {
            uint32_t alloc_size = MAX(r10k->scache_size, R10K_SCACHE_SIZE);

            memset(r10k->scache_taglo, 0,
                   (alloc_size / 64) * sizeof(uint32_t));
            memset(r10k->scache_taghi, 0,
                   (alloc_size / 64) * sizeof(uint8_t));
            memset(r10k->scache_mru, 0,
                   (alloc_size / 128) * sizeof(uint8_t));
            memset(r10k->scache_data, 0, alloc_size);
            memset(r10k->scache_ecc, 0,
                   (alloc_size / 16) * sizeof(uint16_t));
        }
        r10k->CP0_TagLo_rw_bitmask = env->cpu_model->CP0_TagLo_rw_bitmask;
        r10k->CP0_ECC_rw_bitmask = env->cpu_model->CP0_ECC_rw_bitmask;
        r10k->CP0_Config0_preserve_bitmask =
            env->cpu_model->CP0_Config0_preserve_bitmask;
        r10k->CP0_PageMask_rw_bitmask = env->cpu_model->CP0_PageMask_rw_bitmask;
        r10k->CP0_Framemask_rw_bitmask =
            env->cpu_model->CP0_Framemask_rw_bitmask;
        r10k->CP0_EntryLo_extra_rw_bitmask =
            env->cpu_model->CP0_EntryLo_extra_rw_bitmask;
        r10k->xkseg_limit = env->cpu_model->xkseg_limit;
    }

    if (!strcmp(env->cpu_model->name, "R10000")) {
        env->CP0_PRid = cpu->r10k_profile->r10000_prid;
    }
}

void r10k_cpu_set_bus_handler(MIPSCPU *cpu,
                              R10KBusSystemRequestFn handler,
                              void *opaque)
{
    if (cpu->env.r10k) {
        cpu->env.r10k->bus_request = handler;
        cpu->env.r10k->bus_request_opaque = opaque;
    }
}

void r10k_cpu_set_requester_id(MIPSCPU *cpu, uint16_t requester_id)
{
    if (cpu->env.r10k) {
        cpu->env.r10k->requester_id = requester_id;
    }
}

void mips_r10k_cpu_unrealize(MIPSCPU *cpu)
{
    if (cpu->env.r10k) {
        g_free(cpu->env.r10k->scache_taglo);
        g_free(cpu->env.r10k->scache_taghi);
        g_free(cpu->env.r10k->scache_mru);
        g_free(cpu->env.r10k->scache_data);
        g_free(cpu->env.r10k->scache_ecc);
        g_clear_pointer(&cpu->env.r10k, g_free);
    }
    cpu->env.cpu_model = MIPS_CPU_GET_CLASS(cpu)->cpu_def;
    if (cpu->r10k_profile) {
        g_clear_pointer(&cpu->r10k_profile->cpu_model, g_free);
        cpu->r10k_profile->valid = false;
    }
}

void mips_r10k_cpu_finalize(MIPSCPU *cpu)
{
    mips_r10k_cpu_unrealize(cpu);
    g_clear_pointer(&cpu->r10k_profile, g_free);
}
