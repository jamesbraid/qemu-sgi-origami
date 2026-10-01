/* SPDX-License-Identifier: BSD-3-Clause */
/* MGRAS raster command and DMA mapping hooks. */
#ifndef HW_MISC_SGI_MGRAS_RASTER_H
#define HW_MISC_SGI_MGRAS_RASTER_H

#include "hw/misc/sgi-mgras.h"

void sgi_mgras_raster_command(SGIMgrasState *s, uint64_t word);
void sgi_mgras_raster_fifo_write(SGIMgrasState *s, uint64_t word);
bool sgi_mgras_raster_pending_valid(SGIMgrasState *s);
void sgi_mgras_raster_command_token(SGIMgrasState *s, uint32_t token);
/* A read of the data port's high (0x02d1c0) or low (0x02c1c4) word. */
uint32_t sgi_mgras_raster_pio_read(SGIMgrasState *s, bool high);
bool sgi_mgras_raster_pio_read_valid(SGIMgrasState *s);
/* Start the OpenGL client's data-port read (0x8f08) from the registers. */
void sgi_mgras_raster_pio_read_trigger(SGIMgrasState *s);
/* Apply one raster register record; false for a register it leaves. */
bool sgi_mgras_raster_record(SGIMgrasState *s, uint32_t reg, uint32_t value);
void sgi_mgras_dma_map_write(SGIMgrasState *s, uint64_t value,
                              unsigned int size);
uint64_t sgi_mgras_dma_map_slot_base(SGIMgrasState *s, unsigned int slot);
uint32_t sgi_mgras_dma_map_slot_count(SGIMgrasState *s, unsigned int slot);
uint32_t sgi_mgras_dma_map_slot_width(SGIMgrasState *s, unsigned int slot);
uint32_t sgi_mgras_dma_map_slot_control(SGIMgrasState *s, unsigned int slot);

#endif
