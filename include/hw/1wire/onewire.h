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

    /*
     * Called by the base realize (hw/1wire/core.c: onewire_slave_realize)
     * after it has already computed the identity from the family/serial
     * properties. A concrete part implements this instead of DeviceClass's
     * own realize, matching hw/ssi/ssi.c's ssi_peripheral_realize chain --
     * the base always runs first, so a part can never forget the identity
     * computation the way a raw DeviceClass::realize override could.
     */
    void (*realize)(OneWireSlave *s, Error **errp);

    /* A bus reset reached this part: drop any memory-function state. */
    void (*function_reset)(OneWireSlave *s);

    /*
     * One byte of a memory function, once the part is selected. `in` is
     * what the master drove during this byte's eight slots; store the
     * part's reply for the NEXT byte's eight slots in `*out`. Return false
     * to refuse the byte, which the base logs as unimplemented.
     *
     * The reply set by call N appears on the wire during window N+1, not
     * during the byte that produced it: on real silicon the part's first
     * answer bit is already on the wire before the master has driven the
     * rest of that byte's bits, so a real part cannot make its own answer
     * depend on `in` from the same window either. This one-window lag is
     * the hardware, not a modelling shortcut -- do not try to remove it.
     *
     * A device must leave `*out` at 0xff for any window the master itself
     * drives (a command or address byte): any other value pulls the wire
     * low during a master write slot, which no real part ever does.
     * During a window the master reads, `in` is 0xff filler -- the master
     * releases the bus throughout -- so a device must track its own
     * position in the transaction rather than key off `in` to decide what
     * to send next.
     *
     * The model assumes byte-aligned framing: every command, address, and
     * data byte of a memory function is a whole 8-bit window. Every
     * DS2502/DS2505/DS2506 sequence in the extraction satisfies this.
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

/*
 * Computes the identity CRC-8 from family and serial. The base realize
 * calls this automatically from the family/serial properties before
 * OneWireSlaveClass::realize runs; exposed for a part that must derive its
 * identity from something other than those two properties.
 */
void onewire_slave_set_identity(OneWireSlave *s, uint8_t family,
                                 uint64_t serial);

extern const VMStateDescription vmstate_onewire_slave;

#define VMSTATE_ONEWIRE_SLAVE(_field, _state) \
    VMSTATE_STRUCT(_field, _state, 0, vmstate_onewire_slave, OneWireSlave)

#endif
