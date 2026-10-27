/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_PS2_H
#define HW_MIPS_SGI_MACE_PS2_H

#include "qemu/host-utils.h"
#include "hw/core/qdev.h"

#define TYPE_SGI_MACE_PS2 "sgi-mace-ps2"

/* Run child progress after MACE has restored its IRQ baseline. */
void sgi_mace_ps2_restore_post_load(DeviceState *dev);

#define MACE_PS2_POST_TX  0x01
#define MACE_PS2_TXEN     0x02
#define MACE_PS2_TXIE     0x04
#define MACE_PS2_RXIE     0x08
#define MACE_PS2_CLOCK    0x10
#define MACE_PS2_RESET    0x20
#define MACE_PS2_TXEMPTY  0x08
#define MACE_PS2_RXFULL   0x10
#define MACE_PS2_PARITY   0x40
#define MACE_PS2_FRAMING  0x80

typedef struct MACEPS2Port {
    uint8_t control;
    uint8_t tx;
    uint8_t rx;
    uint8_t errors;
    bool tx_pending;
    bool rx_full;
    bool inhibited;
} MACEPS2Port;

static inline bool mace_ps2_clock_permitted(const MACEPS2Port *p)
{
    return (p->control & (MACE_PS2_CLOCK | MACE_PS2_RESET)) ==
           MACE_PS2_CLOCK && !p->inhibited && !p->rx_full;
}

static inline uint8_t mace_ps2_status(const MACEPS2Port *p)
{
    return p->errors | (mace_ps2_clock_permitted(p) ? 3 : 0) |
           (!(p->control & MACE_PS2_RESET) && !p->tx_pending ?
            MACE_PS2_TXEMPTY : 0) | (p->rx_full ? MACE_PS2_RXFULL : 0);
}

static inline void mace_ps2_clear(MACEPS2Port *p)
{
    p->tx = 0;
    p->rx = 0;
    p->errors = 0;
    p->tx_pending = false;
    p->rx_full = false;
    p->inhibited = false;
}

static inline uint16_t mace_ps2_receive(MACEPS2Port *p)
{
    uint16_t value = p->rx | ((uint16_t)mace_ps2_status(p) << 8);

    p->rx_full = false;
    return value;
}

/* Shared byte admission boundary; no guest-visible frame injector. */
static inline bool mace_ps2_receive_frame(MACEPS2Port *p, uint8_t data,
                                          bool start, bool parity, bool stop)
{
    if (!mace_ps2_clock_permitted(p) || p->tx_pending) {
        return false;
    }
    p->rx = data;
    p->errors = (((ctpop8(data) + parity) & 1) ? 0 : MACE_PS2_PARITY) |
                ((start || !stop) ? MACE_PS2_FRAMING : 0);
    p->rx_full = true;
    return true;
}

#endif
