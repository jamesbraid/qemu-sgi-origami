/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MIPS_SGI_MACE_H
#define HW_MIPS_SGI_MACE_H

#include "hw/core/qdev.h"
#include "qapi/error.h"
#include "qom/object.h"

#define TYPE_SGI_MACE "sgi-mace"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMACEState, SGI_MACE)

void sgi_mace_enable_serial_ring_offsets(DeviceState *dev, Error **errp);
void sgi_mace_enable_storage_diagnostic(DeviceState *dev, Error **errp);

#endif
