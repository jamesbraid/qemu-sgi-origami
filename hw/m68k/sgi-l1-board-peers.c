/*
 * The polled devices of an Origin 3000 C-brick L1 board.
 *
 * The firmware's C-brick tables name a bus select expander, byte-wide
 * expanders and voltage and temperature monitors on four segments of its
 * two-wire bus. Each part here is the documented bus behaviour of the family
 * the tables fit, with the segment a device sits on gating whether it can
 * be reached. The board page rates every choice.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "monitor/qdev.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "hw/i2c/i2c-wire.h"
#include "hw/m68k/sgi-l1-board-peers.h"
#include "migration/vmstate.h"
#include "trace.h"

#define TYPE_SGI_L1_SEGMENT_TARGET "sgi-l1-segment-target"
OBJECT_DECLARE_SIMPLE_TYPE(SegmentTarget, SGI_L1_SEGMENT_TARGET)

#define TYPE_SGI_L1_EXPANDER "sgi-l1-expander"
OBJECT_DECLARE_SIMPLE_TYPE(Expander, SGI_L1_EXPANDER)

#define TYPE_SGI_L1_BUS_SELECT "sgi-l1-bus-select"
OBJECT_DECLARE_SIMPLE_TYPE(BusSelect, SGI_L1_BUS_SELECT)

#define SELECT_ADDR7        0x27    /* wire address 0x4e */
#define SELECT_OFF          0xff    /* every segment off */

/* A device on one segment of the board's two-wire bus. */
struct SegmentTarget {
    I2CSlave parent_obj;

    /* 0 is the controller's own bus; 2 to 13 are behind the select. */
    uint8_t segment;
    BusSelect *select;
};

/* A byte-wide quasi-bidirectional expander: no register byte. */
struct Expander {
    SegmentTarget parent_obj;

    uint8_t input_mask;
    uint8_t latch;
};

struct BusSelect {
    Expander parent_obj;
};

/*
 * Which segments a select byte enables, as the firmware's bytes read:
 * bits 7 to 4 low enable segments 2 to 5, and bit 3 low enables the
 * eight-way selector that bits 2 to 0 steer to segments 6 to 13. Bytes
 * the firmware never writes are decoded the same way, so more than one
 * segment can be enabled at once.
 */
static bool select_reaches(uint8_t sel, unsigned segment)
{
    if (segment >= 2 && segment <= 5) {
        return !(sel & (0x80 >> (segment - 2)));
    }
    if (segment >= 6 && segment <= 13) {
        return !(sel & 0x08) && (sel & 7) == segment - 6;
    }
    return false;
}

static bool segment_target_match(I2CSlave *candidate, uint8_t address,
                                 bool broadcast, I2CNodeList *current_devs)
{
    SegmentTarget *t = SGI_L1_SEGMENT_TARGET(candidate);
    I2CNode *node;

    if (t->segment && !(t->select &&
        select_reaches(t->select->parent_obj.latch, t->segment))) {
        return broadcast;
    }
    if (candidate->address != address && !broadcast) {
        return false;
    }
    node = g_new(I2CNode, 1);
    node->elt = candidate;
    QLIST_INSERT_HEAD(current_devs, node, next);
    return true;
}

static int segment_target_event(I2CSlave *s, enum i2c_event event)
{
    switch (event) {
    case I2C_START_RECV:
    case I2C_START_SEND:
    case I2C_FINISH:
    case I2C_NACK:
    case I2C_RESTART:
        return 0;
    default:
        return -1;
    }
}

static void segment_target_realize(DeviceState *dev, Error **errp)
{
    SegmentTarget *t = SGI_L1_SEGMENT_TARGET(dev);

    if (t->segment == 1 || t->segment > 13) {
        error_setg(errp, "segment is 0, or 2 to 13");
    } else if (t->segment && !t->select) {
        error_setg(errp, "a device on segment %u needs a bus-select",
                   t->segment);
    }
}

static const Property segment_target_props[] = {
    DEFINE_PROP_UINT8("segment", SegmentTarget, segment, 0),
    DEFINE_PROP_LINK("bus-select", SegmentTarget, select,
                     TYPE_SGI_L1_BUS_SELECT, BusSelect *),
};

static void segment_target_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    sc->match_and_add = segment_target_match;
    sc->event = segment_target_event;
    DEVICE_CLASS(oc)->realize = segment_target_realize;
    device_class_set_props(DEVICE_CLASS(oc), segment_target_props);
}

static int expander_send(I2CSlave *s, uint8_t data)
{
    Expander *e = SGI_L1_EXPANDER(s);

    e->latch = data;
    trace_sgi_l1_expander_write(s->address << 1,
                                SGI_L1_SEGMENT_TARGET(s)->segment, data);
    return 0;
}

/* The pins the firmware uses as inputs are written high and read high. */
static uint8_t expander_recv(I2CSlave *s)
{
    Expander *e = SGI_L1_EXPANDER(s);
    uint8_t val = e->latch | e->input_mask;

    trace_sgi_l1_expander_read(s->address << 1,
                               SGI_L1_SEGMENT_TARGET(s)->segment, val);
    return val;
}

static void expander_reset_hold(Object *obj, ResetType type)
{
    SGI_L1_EXPANDER(obj)->latch = 0xff;
}

/*
 * The board refuses migration, but a timed two-wire bus takes no device
 * that does not describe its state.
 */
static const VMStateDescription vmstate_expander = {
    .name = TYPE_SGI_L1_EXPANDER,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj.parent_obj, Expander),
        VMSTATE_UINT8(latch, Expander),
        VMSTATE_END_OF_LIST()
    },
};

static const Property expander_props[] = {
    DEFINE_PROP_UINT8("input-mask", Expander, input_mask, 0),
};

static void expander_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);

    sc->send = expander_send;
    sc->recv = expander_recv;
    device_class_set_props(DEVICE_CLASS(oc), expander_props);
    DEVICE_CLASS(oc)->desc = "SGI L1 board byte-wide expander";
    DEVICE_CLASS(oc)->vmsd = &vmstate_expander;
    RESETTABLE_CLASS(oc)->phases.hold = expander_reset_hold;
}

/* The enabled segments as a mask, bit n for segment n: a test hook. */
static void bus_select_get_segments(Object *obj, Visitor *v, const char *name,
                                    void *opaque, Error **errp)
{
    uint8_t sel = SGI_L1_EXPANDER(obj)->latch;
    uint16_t mask = 0;
    unsigned seg;

    for (seg = 2; seg <= 13; seg++) {
        if (select_reaches(sel, seg)) {
            mask |= 1u << seg;
        }
    }
    visit_type_uint16(v, name, &mask, errp);
}

static void bus_select_class_init(ObjectClass *oc, const void *data)
{
    DEVICE_CLASS(oc)->desc = "SGI L1 board bus select expander";
    object_class_property_add(oc, "enabled-segments", "uint16",
                              bus_select_get_segments, NULL, NULL, NULL);
}

static const TypeInfo peer_types[] = {
    {
        .name          = TYPE_SGI_L1_SEGMENT_TARGET,
        .parent        = TYPE_I2C_SLAVE,
        .instance_size = sizeof(SegmentTarget),
        .class_init    = segment_target_class_init,
        .abstract      = true,
        .interfaces    = (const InterfaceInfo[]) {
            { TYPE_I2C_WIRE_ENDPOINT },
            { }
        },
    },
    {
        .name          = TYPE_SGI_L1_EXPANDER,
        .parent        = TYPE_SGI_L1_SEGMENT_TARGET,
        .instance_size = sizeof(Expander),
        .class_init    = expander_class_init,
    },
    {
        .name          = TYPE_SGI_L1_BUS_SELECT,
        .parent        = TYPE_SGI_L1_EXPANDER,
        .instance_size = sizeof(BusSelect),
        .class_init    = bus_select_class_init,
    },
};

DEFINE_TYPES(peer_types)

/* Wire addresses and input masks are the firmware's expander table. */
static const struct {
    uint8_t wire;
    uint8_t segment;
    uint8_t input_mask;
} expanders[] = {
    { 0x40, 0, 0x0f },  /* display control */
    { 0x42, 2, 0xff },  /* LED 0 low */
    { 0x44, 2, 0xff },  /* LED 0 high */
    { 0x46, 2, 0xff },  /* LED 1 low */
    { 0x48, 2, 0xff },  /* LED 1 high */
    { 0x4c, 2, 0xf1 },  /* COM control */
    { 0x42, 3, 0x00 },  /* Bedrock control 1 */
    { 0x44, 3, 0x00 },  /* Bedrock control 2 */
    { 0x46, 3, 0x55 },  /* port control */
    { 0x48, 3, 0xde },  /* power control */
    { 0x4a, 3, 0xff },  /* processor module presence */
    { 0x4c, 3, 0x40 },  /* fan control */
};

void sgi_l1_cbrick_peers_fit(I2CBus *bus)
{
    I2CSlave *sel = i2c_slave_new(TYPE_SGI_L1_BUS_SELECT, SELECT_ADDR7);
    int i;

    qdev_set_id(DEVICE(sel), g_strdup("l1-bus-select"), &error_abort);
    i2c_slave_realize_and_unref(sel, bus, &error_abort);
    for (i = 0; i < ARRAY_SIZE(expanders); i++) {
        I2CSlave *e = i2c_slave_new(TYPE_SGI_L1_EXPANDER,
                                    expanders[i].wire >> 1);
        DeviceState *dev = DEVICE(e);

        qdev_prop_set_uint8(dev, "segment", expanders[i].segment);
        qdev_prop_set_uint8(dev, "input-mask", expanders[i].input_mask);
        object_property_set_link(OBJECT(e), "bus-select", OBJECT(sel),
                                 &error_abort);
        i2c_slave_realize_and_unref(e, bus, &error_abort);
    }
}
