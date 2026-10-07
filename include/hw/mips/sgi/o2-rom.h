/* SPDX-License-Identifier: BSD-3-Clause */
#ifndef HW_MIPS_SGI_O2_ROM_H
#define HW_MIPS_SGI_O2_ROM_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SGI_O2_ROM "sgi-o2-rom"
OBJECT_DECLARE_SIMPLE_TYPE(SGIO2ROMState, SGI_O2_ROM)

/* MACE's miscellaneous-control bit 0 drives this input. */
void sgi_o2_rom_set_write_enable(SGIO2ROMState *rom, bool enabled);

#endif
