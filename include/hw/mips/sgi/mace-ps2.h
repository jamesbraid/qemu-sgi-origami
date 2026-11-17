/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_PS2_H
#define HW_MIPS_SGI_MACE_PS2_H

#include "hw/core/qdev.h"

#define TYPE_SGI_MACE_PS2 "sgi-mace-ps2"

enum {
    MACE_PS2_ACTION_NONE,
    MACE_PS2_ACTION_POP_RX,
    MACE_PS2_ACTION_SEND_KEYBOARD,
    MACE_PS2_ACTION_SEND_MOUSE,
    MACE_PS2_ACTION_DROP_MOUSE_STREAM,
};

typedef struct MACEPS2PortMigration {
    uint8_t control;
    uint8_t tx;
    uint8_t rx;
    uint8_t errors;
    bool tx_pending;
    bool rx_full;
    bool inhibited;
    uint16_t reply;
} MACEPS2PortMigration;

void *sgi_mace_ps2_new(void);
void sgi_mace_ps2_free(void *core);
bool sgi_mace_ps2_read(void *core, uint64_t addr, uint32_t size,
                       uint64_t *value);
bool sgi_mace_ps2_write(void *core, uint64_t addr, uint64_t value,
                        uint32_t size);
int sgi_mace_ps2_next_action(void *core, uint32_t port, bool child_empty,
                             uint8_t *data);
void sgi_mace_ps2_transmit_complete(void *core, uint32_t port,
                                    uint32_t reply_count);
bool sgi_mace_ps2_receive(void *core, uint32_t port, uint8_t data,
                          void *child);
void sgi_mace_ps2_stream_dropped(uint32_t count);
void sgi_mace_ps2_outputs(void *core, uint32_t port, bool resetting,
                          bool *irq, bool *poll);
void sgi_mace_ps2_reset(void *core);
void sgi_mace_ps2_export(void *core, MACEPS2PortMigration *out);
bool sgi_mace_ps2_import(void *core, const MACEPS2PortMigration *input);

void sgi_mace_ps2_restore_post_load(DeviceState *dev);

#endif
