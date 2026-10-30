/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef MIPS_R5000_H
#define MIPS_R5000_H

void mips_r5000_cpu_class_init(ObjectClass *oc, const mips_def_t *def);
bool mips_r5000_cpu_validate(MIPSCPU *cpu, Error **errp);
int mips_r5000_cpu_post_load(MIPSCPU *cpu);
uint32_t mips_r5000_config_reset(MIPSCPU *cpu, uint32_t config);

#endif
