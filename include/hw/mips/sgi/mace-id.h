/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_MACE_ID_H
#define HW_MIPS_SGI_MACE_ID_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_SGI_MACE_ID "sgi-mace-id"
OBJECT_DECLARE_SIMPLE_TYPE(SGIMACEIDState, SGI_MACE_ID)

/* Fixed C/Rust snapshot used to preserve the existing VMState v1 layout. */
typedef struct SGIMACEIDMigrationState {
    bool password_clear_input;
    bool flash_write_enabled;
    bool released;
    uint32_t led_control;
} SGIMACEIDMigrationState;

enum {
    SGI_MACE_ID_WRITE_DECODE_ERROR = -1,
    SGI_MACE_ID_WRITE_NOOP = -2,
};

/* Rust writes return the flash IRQ level, or one of the negative results. */

void *sgi_mace_id_new(void *bus, bool part, bool misc_word_pio,
                      bool password_clear_input);
void sgi_mace_id_free(void *core);
void sgi_mace_id_set_input(void *core, bool level);
bool sgi_mace_id_reset(void *core);
int sgi_mace_id_read(void *core, int64_t now, uint64_t offset, uint32_t size,
                     uint64_t *value);
int sgi_mace_id_write(void *core, int64_t now, uint64_t offset, uint32_t size,
                      uint64_t value);
void sgi_mace_id_export(void *core, SGIMACEIDMigrationState *state);
bool sgi_mace_id_import(void *core,
                        const SGIMACEIDMigrationState *state);

#endif
