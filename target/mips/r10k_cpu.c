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
    uint32_t value = MIPS_CPU(obj)->r10k_profile->r10000_prid;

    visit_type_uint32(v, name, &value, errp);
}

static void mips_r10000_prid_set(Object *obj, Visitor *v, const char *name,
                                void *opaque, Error **errp)
{
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
    MIPS_CPU(obj)->r10k_profile->r10000_prid = value;
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
    uint32_t value = cpu->r10k_profile->values[field];

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

void mips_r10k_cpu_init(MIPSCPU *cpu)
{
    Object *obj = OBJECT(cpu);
    MIPSCPUClass *mcc = MIPS_CPU_GET_CLASS(cpu);

    if (!(mcc->cpu_def->insn_flags & INSN_R10K)) {
        return;
    }
    cpu->r10k_profile = g_new0(R10KCPUProfile, 1);
    if (!strcmp(mcc->cpu_def->name, "R10000")) {
        cpu->r10k_profile->r10000_prid = mcc->cpu_def->CP0_PRid;
        object_property_add(obj, "r10000-prid", "uint32",
                            mips_r10000_prid_get, mips_r10000_prid_set,
                            NULL, NULL);
    }
    if (!strcmp(mcc->cpu_def->name, "R12000")) {
        for (uintptr_t i = 0; i < R12000_PROFILE_FIELDS; i++) {
            object_property_add(obj, r12000_property_names[i], "uint32",
                                mips_r12000_property_get,
                                mips_r12000_property_set,
                                NULL, (void *)i);
        }
    }
}

bool mips_r10k_cpu_validate(MIPSCPU *cpu, Error **errp)
{
    return strcmp(MIPS_CPU_GET_CLASS(cpu)->cpu_def->name, "R12000") ||
           mips_r12000_validate(cpu, errp);
}

void mips_r10k_cpu_realize(MIPSCPU *cpu)
{
    if (cpu->env.cpu_model->insn_flags & INSN_R10K) {
        cpu->env.r10k = g_new0(R10KState, 1);
    }
}

void mips_r10k_cpu_reset(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;

    /* Cold initialization clears diagnostics while preserving board links. */
    if (env->r10k) {
        R10KState *r10k = env->r10k;

        memset(r10k, 0, offsetof(R10KState, system_requests));
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

void mips_r10k_cpu_unrealize(MIPSCPU *cpu)
{
    g_clear_pointer(&cpu->env.r10k, g_free);
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
