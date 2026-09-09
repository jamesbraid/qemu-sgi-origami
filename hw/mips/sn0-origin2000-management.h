/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SN0_ORIGIN2000_MANAGEMENT_H
#define HW_MIPS_SN0_ORIGIN2000_MANAGEMENT_H

#include "qemu/typedefs.h"
#include "sn0-model.h"

/* Check shared module resources before constructing any devices. */
bool sn0_origin2000_management_validate(const SN0ModelComposition *model,
                                       Error **errp);

#endif
