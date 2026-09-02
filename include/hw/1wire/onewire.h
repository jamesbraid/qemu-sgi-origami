/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_1WIRE_ONEWIRE_H
#define HW_1WIRE_ONEWIRE_H

#include "hw/core/qdev.h"
#include "qom/object.h"

#define TYPE_ONEWIRE_BUS "onewire-bus"
OBJECT_DECLARE_SIMPLE_TYPE(OneWireBus, ONEWIRE_BUS)

#define TYPE_ONEWIRE_SLAVE "onewire-slave"
OBJECT_DECLARE_TYPE(OneWireSlave, OneWireSlaveClass, ONEWIRE_SLAVE)

/* Where a transaction stands, which decides how a command byte decodes. */
typedef enum {
    ONEWIRE_ROM_COMMAND,   /* after a reset: the next byte is a ROM command */
    ONEWIRE_ROM_MATCH,     /* clocking 64 identity bits for Match ROM */
    ONEWIRE_ROM_SEARCH,    /* the Search ROM bit triplets */
    ONEWIRE_ROM_READ,      /* shifting the identity out for Read ROM */
    ONEWIRE_FUNCTION,      /* selected: bytes belong to the part */
    ONEWIRE_IDLE,          /* dropped out; silent until the next reset */
} OneWirePhase;

struct OneWireSlaveClass {
    DeviceClass parent_class;

    /* A bus reset reached this part: drop any memory-function state. */
    void (*function_reset)(OneWireSlave *s);

    /*
     * One byte of a memory function, once the part is selected. `in` is what
     * the master drove; store the part's reply in `*out`. Return false to
     * refuse the byte, which the base logs as unimplemented.
     */
    bool (*function_byte)(OneWireSlave *s, uint8_t in, uint8_t *out);
};

struct OneWireSlave {
    DeviceState qdev;

    uint8_t family;        /* first identity byte */
    uint64_t serial;       /* 48 bits */
    uint8_t identity[8];   /* family, serial, CRC-8; built at realize */

    uint32_t phase;        /* a OneWirePhase; fixed width so it migrates */
    uint32_t bit;          /* bit index within the current byte or field */
    uint8_t shift;         /* byte being accumulated from the master */
    uint8_t out;           /* byte being shifted out to the master */
};

OneWireBus *onewire_create_bus(DeviceState *parent, const char *name);

/* Long low pulse: returns true if any part answered with a presence pulse. */
bool onewire_reset(OneWireBus *bus);

/*
 * One bit slot. The master drives `drive`; the wire is open-drain, so the
 * sampled level is `drive` AND NOT (any part pulling low). Reading is a slot
 * that drives one.
 */
bool onewire_touch_bit(OneWireBus *bus, bool drive);

/* Set before realize; the base computes the identity CRC-8. */
void onewire_slave_set_identity(OneWireSlave *s, uint8_t family,
                                 uint64_t serial);

extern const VMStateDescription vmstate_onewire_slave;

#define VMSTATE_ONEWIRE_SLAVE(_field, _state) \
    VMSTATE_STRUCT(_field, _state, 0, vmstate_onewire_slave, OneWireSlave)

#endif
