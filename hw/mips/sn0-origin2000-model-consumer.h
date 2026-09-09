/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_MODEL_CONSUMER_H
#define HW_MIPS_SN0_ORIGIN2000_MODEL_CONSUMER_H

#include "qemu/typedefs.h"
#include "sn0-model.h"

/* Validate references and supported geometry before constructing devices. */
bool sn0_origin2000_validate(const SN0ModelComposition *model, Error **errp);

#endif
