/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* R5000 functional mode inputs; no factory module mode word is implied. */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/error-report.h"
#include "hw/core/qdev-properties.h"
#include "cpu.h"
#include "internal.h"
#include "r5000.h"

static const Property r5000_properties[] = {
    DEFINE_PROP_UINT32("r5000-ec", MIPSCPU, r5000_ec, UINT32_MAX),
    DEFINE_PROP_UINT32("r5000-ep", MIPSCPU, r5000_ep, UINT32_MAX),
};

void mips_r5000_cpu_class_init(ObjectClass *oc, const mips_def_t *def)
{
    if (!strcmp(def->name, "R5000")) {
        device_class_set_props(DEVICE_CLASS(oc), r5000_properties);
    }
}

bool mips_r5000_cpu_validate(MIPSCPU *cpu, Error **errp)
{
    if (strcmp(cpu->env.cpu_model->name, "R5000")) {
        return true;
    }
    if (cpu->r5000_ec == UINT32_MAX) {
        error_setg(errp, "r5000-ec must be explicitly supplied");
        return false;
    }
    if (cpu->r5000_ep == UINT32_MAX) {
        error_setg(errp, "r5000-ep must be explicitly supplied");
        return false;
    }
    if (cpu->r5000_ec > 6) {
        error_setg(errp, "r5000-ec must be in 0..6");
        return false;
    }
    if (cpu->r5000_ep > 8) {
        error_setg(errp, "r5000-ep must be in 0..8");
        return false;
    }
    return true;
}

uint32_t mips_r5000_config_reset(MIPSCPU *cpu, uint32_t config)
{
    if (!strcmp(cpu->env.cpu_model->name, "R5000")) {
        config = deposit32(config, 28, 3, cpu->r5000_ec);
        config = deposit32(config, 24, 4, cpu->r5000_ep);
    }
    return config;
}

int mips_r5000_cpu_post_load(MIPSCPU *cpu)
{
    CPUMIPSState *env = &cpu->env;
    uint32_t reset_config;
    uint32_t writable;

    if (strcmp(env->cpu_model->name, "R5000")) {
        return 0;
    }
    reset_config = deposit32(env->cpu_model->CP0_Config0,
                              CP0C0_BE, 1, cpu->is_big_endian);
    reset_config = mips_r5000_config_reset(cpu, reset_config);
    writable = env->cpu_model->CP0_Config0_rw_bitmask;
    if (!cpu->r5000_modes_loaded ||
        env->CP0_PRid != env->cpu_model->CP0_PRid ||
        env->active_fpu.fcr0 != env->cpu_model->CP1_fcr0 ||
        ((env->CP0_Config0 ^ reset_config) & ~writable)) {
        error_report("R5000 migration requires matching CPU identity "
                     "and modes");
        return -EINVAL;
    }
    return 0;
}
