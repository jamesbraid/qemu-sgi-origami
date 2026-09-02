/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "hw/1wire/onewire.h"
#include "hw/1wire/crc.h"

struct OneWireBus { BusState parent_obj; };

static const TypeInfo onewire_bus_info = {
    .name = TYPE_ONEWIRE_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(OneWireBus),
};

OneWireBus *onewire_create_bus(DeviceState *parent, const char *name)
{
    return ONEWIRE_BUS(qbus_new(TYPE_ONEWIRE_BUS, parent, name));
}

/* The phase machine for one part; defined below, used by both bus entries. */
static bool onewire_slave_slot(OneWireSlave *s, bool drive);

/* Each part answers whether it pulls the line low; the wire is the AND. */
bool onewire_touch_bit(OneWireBus *bus, bool drive)
{
    BusState *b = BUS(bus);
    bool pulled = false;
    BusChild *kid;

    QTAILQ_FOREACH(kid, &b->children, sibling) {
        OneWireSlave *s = ONEWIRE_SLAVE(kid->child);
        pulled |= onewire_slave_slot(s, drive);
    }
    return drive && !pulled;
}

/*
 * The state a part returns to on either kind of reset: the long low pulse
 * that starts a 1-Wire transaction, and QEMU's own machine/device reset.
 * Shared so the two can never drift apart -- see onewire_reset() and
 * onewire_slave_reset_hold() below.
 */
static void onewire_slave_reset_state(OneWireSlave *s)
{
    OneWireSlaveClass *k = ONEWIRE_SLAVE_GET_CLASS(s);

    s->phase = ONEWIRE_ROM_COMMAND;
    s->bit = 0;
    s->shift = 0;
    s->out = 0xff;          /* idle high: no function byte answered yet */
    if (k->function_reset) {
        k->function_reset(s);
    }
}

bool onewire_reset(OneWireBus *bus)
{
    BusState *b = BUS(bus);
    bool present = false;
    BusChild *kid;

    QTAILQ_FOREACH(kid, &b->children, sibling) {
        onewire_slave_reset_state(ONEWIRE_SLAVE(kid->child));
        present = true;
    }
    return present;
}

void onewire_slave_set_identity(OneWireSlave *s, uint8_t family,
                                 uint64_t serial)
{
    s->family = family;
    s->serial = serial;
    s->identity[0] = family;
    for (unsigned i = 0; i < 6; i++) {
        s->identity[1 + i] = (serial >> (8 * i)) & 0xff;
    }
    s->identity[7] = onewire_crc8(0, s->identity, 7);
}

/*
 * The ROM-command phase machine, common to every part regardless of which
 * memory function it implements. Bits arrive and leave LSB first, matching
 * the wire order of every Dallas/Maxim 1-Wire part.
 *
 * The four ROM-command success paths -- Read ROM, Match ROM, a completed
 * Search ROM, and Skip ROM -- all converge on ONEWIRE_FUNCTION: a part that
 * survives addressing is selected outright, with no second reset or second
 * addressing command needed (DS2502/DS2505 ROM-function flow chart). Match
 * ROM drops a part to ONEWIRE_IDLE at the first identity bit that fails to
 * compare, rather than clocking all 64 bits before deciding; the datasheets'
 * prose agrees: a part that does not match simply waits for the next reset.
 */
static bool onewire_slave_slot(OneWireSlave *s, bool drive)
{
    switch ((OneWirePhase)s->phase) {
    case ONEWIRE_ROM_COMMAND: {
        if (drive) {
            s->shift |= 1u << s->bit;
        }
        s->bit++;
        if (s->bit == 8) {
            uint8_t cmd = s->shift;

            s->bit = 0;
            s->shift = 0;
            switch (cmd) {
            case 0x33: /* Read ROM */
                s->phase = ONEWIRE_ROM_READ;
                break;
            case 0x55: /* Match ROM */
                s->phase = ONEWIRE_ROM_MATCH;
                break;
            case 0xf0: /* Search ROM */
                s->phase = ONEWIRE_ROM_SEARCH;
                break;
            case 0xcc: /* Skip ROM */
                s->phase = ONEWIRE_FUNCTION;
                break;
            case 0x3c: /* Overdrive-Skip ROM */
            case 0x69: /* Overdrive-Match ROM */
                qemu_log_mask(LOG_UNIMP,
                              "onewire: overdrive ROM command 0x%02x not "
                              "implemented\n", cmd);
                s->phase = ONEWIRE_IDLE;
                break;
            default:
                qemu_log_mask(LOG_GUEST_ERROR,
                              "onewire: unknown ROM command 0x%02x\n", cmd);
                s->phase = ONEWIRE_IDLE;
                break;
            }
        }
        return false;
    }

    case ONEWIRE_ROM_READ: {
        bool bitval = (s->identity[s->bit / 8] >> (s->bit % 8)) & 1;

        s->bit++;
        if (s->bit == 64) {
            s->bit = 0;
            s->phase = ONEWIRE_FUNCTION;
        }
        return !bitval;
    }

    case ONEWIRE_ROM_MATCH: {
        bool idbit = (s->identity[s->bit / 8] >> (s->bit % 8)) & 1;

        if (drive != idbit) {
            s->phase = ONEWIRE_IDLE;
            s->bit = 0;
            return false;
        }
        s->bit++;
        if (s->bit == 64) {
            s->bit = 0;
            s->phase = ONEWIRE_FUNCTION;
        }
        return false;
    }

    case ONEWIRE_ROM_SEARCH: {
        unsigned idbit_no = s->bit / 3;
        unsigned triplet_slot = s->bit % 3;
        bool idbit = (s->identity[idbit_no / 8] >> (idbit_no % 8)) & 1;
        bool pulled = false;

        switch (triplet_slot) {
        case 0: /* identify: the part drives its own bit */
            pulled = !idbit;
            break;
        case 1: /* complement: the part drives the inverse */
            pulled = idbit;
            break;
        default: /* direction: the master picks a branch; follow or drop */
            if (drive != idbit) {
                s->phase = ONEWIRE_IDLE;
                s->bit = 0;
                return false;
            }
            break;
        }
        s->bit++;
        if (s->bit == 64 * 3) {
            s->bit = 0;
            s->phase = ONEWIRE_FUNCTION;
        }
        return pulled;
    }

    case ONEWIRE_FUNCTION: {
        bool pulled = !((s->out >> s->bit) & 1);

        if (drive) {
            s->shift |= 1u << s->bit;
        }
        s->bit++;
        if (s->bit == 8) {
            OneWireSlaveClass *k = ONEWIRE_SLAVE_GET_CLASS(s);
            uint8_t in = s->shift;
            uint8_t out = 0xff;

            if (!k->function_byte || !k->function_byte(s, in, &out)) {
                qemu_log_mask(LOG_UNIMP,
                              "onewire: unimplemented memory function byte "
                              "0x%02x\n", in);
            }
            s->out = out;
            s->shift = 0;
            s->bit = 0;
        }
        return pulled;
    }

    case ONEWIRE_IDLE:
    default:
        return false;
    }
}

/*
 * The base realize: compute the identity from the family/serial properties
 * -- already applied by the time realize runs -- then chain to the
 * concrete part's own realize, if it has one. A concrete part can never
 * forget this the way it could forget to call onewire_slave_set_identity()
 * itself, because it no longer has a DeviceClass::realize of its own to
 * forget it in (hw/ssi/ssi.c: ssi_peripheral_realize is the same shape).
 */
static void onewire_slave_realize(DeviceState *dev, Error **errp)
{
    OneWireSlave *s = ONEWIRE_SLAVE(dev);
    OneWireSlaveClass *k = ONEWIRE_SLAVE_GET_CLASS(s);

    onewire_slave_set_identity(s, s->family, s->serial);
    if (k->realize) {
        k->realize(s, errp);
    }
}

/* QEMU's own reset reaches the same state a 1-Wire reset pulse does. */
static void onewire_slave_reset_hold(Object *obj, ResetType type)
{
    onewire_slave_reset_state(ONEWIRE_SLAVE(obj));
}

static int onewire_slave_post_load(void *opaque, int version_id)
{
    OneWireSlave *s = opaque;
    uint32_t limit;

    switch ((OneWirePhase)s->phase) {
    case ONEWIRE_ROM_COMMAND:
    case ONEWIRE_FUNCTION:
        limit = 8;
        break;
    case ONEWIRE_ROM_READ:
    case ONEWIRE_ROM_MATCH:
        limit = 64;
        break;
    case ONEWIRE_ROM_SEARCH:
        limit = 64 * 3;
        break;
    case ONEWIRE_IDLE:
        limit = 1;           /* bit is always reset to 0 in ONEWIRE_IDLE */
        break;
    default:
        return -EINVAL;      /* not one of the phases this bus defines */
    }
    if (s->bit >= limit) {
        return -EINVAL;
    }
    return 0;
}

static const Property onewire_slave_properties[] = {
    DEFINE_PROP_UINT8("family", OneWireSlave, family, 0),
    DEFINE_PROP_UINT64("serial", OneWireSlave, serial, 0),
};

static void onewire_slave_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->bus_type = TYPE_ONEWIRE_BUS;
    dc->realize = onewire_slave_realize;
    rc->phases.hold = onewire_slave_reset_hold;
    device_class_set_props(dc, onewire_slave_properties);
}

static const TypeInfo onewire_slave_info = {
    .name = TYPE_ONEWIRE_SLAVE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(OneWireSlave),
    .abstract = true,
    .class_size = sizeof(OneWireSlaveClass),
    .class_init = onewire_slave_class_init,
};

const VMStateDescription vmstate_onewire_slave = {
    .name = "onewire-slave",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = onewire_slave_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(phase, OneWireSlave),
        VMSTATE_UINT32(bit, OneWireSlave),
        VMSTATE_UINT8(shift, OneWireSlave),
        VMSTATE_UINT8(out, OneWireSlave),
        VMSTATE_END_OF_LIST()
    }
};

static void onewire_register_types(void)
{
    type_register_static(&onewire_bus_info);
    type_register_static(&onewire_slave_info);
}

type_init(onewire_register_types)
