/*
 * Motorola MCF5206e M-Bus (two-wire) controller.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#ifndef HW_M68K_MCF5206E_MBUS_H
#define HW_M68K_MCF5206E_MBUS_H

#include "hw/i2c/i2c.h"
#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_MCF5206E_MBUS "mcf5206e-mbus"
OBJECT_DECLARE_SIMPLE_TYPE(MCF5206EMBusState, MCF5206E_MBUS)

/* The five byte registers, as MBAR-relative offsets. */
#define MCF5206E_MBUS_MADR  0x1e0
#define MCF5206E_MBUS_MFDR  0x1e4
#define MCF5206E_MBUS_MBCR  0x1e8
#define MCF5206E_MBUS_MBSR  0x1ec
#define MCF5206E_MBUS_MBDR  0x1f0

/*
 * One aligned byte read or write of a register. @side_effects is false for a
 * read that only fetches a value to merge into a wider write.
 */
uint8_t mcf5206e_mbus_read(MCF5206EMBusState *s, hwaddr offset,
                           bool side_effects);
void mcf5206e_mbus_write(MCF5206EMBusState *s, hwaddr offset, uint8_t value);

/** The bus the controller drives: boards attach their devices to it. */
I2CBus *mcf5206e_mbus_get_bus(MCF5206EMBusState *s);

#endif
