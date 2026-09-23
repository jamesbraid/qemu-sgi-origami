/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_MISC_SGI_MGRAS_RASTER_H
#define HW_MISC_SGI_MGRAS_RASTER_H

#include "hw/misc/sgi-mgras.h"

void si_raster_command(SGIMgrasState *s, uint64_t word);
void si_dma_map_write(SGIMgrasState *s, uint64_t value, unsigned int size);
uint64_t si_dma_map_slot_base(SGIMgrasState *s, unsigned int slot);
uint32_t si_dma_map_slot_count(SGIMgrasState *s, unsigned int slot);
uint32_t si_dma_map_slot_width(SGIMgrasState *s, unsigned int slot);
uint32_t si_dma_map_slot_control(SGIMgrasState *s, unsigned int slot);

#endif
