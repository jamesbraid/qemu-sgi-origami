/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_I2C_H
#define HW_MIPS_SGI_MACE_I2C_H

#include "hw/i2c/i2c-wire.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "system/memory.h"

#define TYPE_SGI_MACE_I2C "sgi-mace-i2c-untimed"
OBJECT_DECLARE_SIMPLE_TYPE(MACEI2C, SGI_MACE_I2C)

struct MACEI2C {
    SysBusDevice parent_obj;
    MemoryRegion regs;
    I2CBus *bus;
    uint8_t enabled;
    uint8_t config;
    uint8_t control;
    uint8_t data;
    uint8_t address;
    bool owned;
    bool address_acked;
    bool receive;
    bool nack;
    bool error;
    bool pending_ack;
    bool busy;
    bool reset_pending;
    bool cold_pending;
};

void mace_i2c_reset(MACEI2C *s, bool cold);
void mace_i2c_prepare_unrealize(MACEI2C *s, Error **errp);

#endif
