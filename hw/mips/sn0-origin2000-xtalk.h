/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_XTALK_H
#define HW_MIPS_SN0_ORIGIN2000_XTALK_H

#include "qapi/qapi-types-common.h"
#include "sn0-model.h"

typedef struct SN0MachineState SN0MachineState;

bool sn0_origin2000_xtalk_validate(const SN0ModelComposition *model,
                                  Error **errp);
bool sn0_origin2000_xtalk_prepare(SN0MachineState *machine, Error **errp);
void sn0_origin2000_xtalk_destroy(SN0MachineState *machine);
bool sn0_origin2000_xtalk_test_proxies_disabled(SN0MachineState *machine);

#endif
