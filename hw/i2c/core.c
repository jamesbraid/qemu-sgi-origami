/*
 * QEMU I2C bus interface.
 *
 * Copyright (c) 2007 CodeSourcery.
 * Written by Paul Brook
 *
 * This code is licensed under the LGPL.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "qemu/timer.h"
#include "trace.h"

typedef struct I2CWireMaster {
    int64_t low_ns, high_ns;
    int64_t deadline, staged_at;
    bool configured, sda, clock, scl_low;
    bool pending, staged_sda, staged_clock;
    /* Clock generation stopped with SCL still held low, awaiting a resume. */
    bool paused;
    bool clock_after_loss;
} I2CWireMaster;

struct I2CWire {
    I2CBus *bus;
    QEMUTimer *timer;
    I2CWireObserver observe;
    void *opaque;
    I2CWireObserver master_observe[I2C_WIRE_MAX_MASTERS];
    void *master_opaque[I2C_WIRE_MAX_MASTERS];
    I2CWireMaster masters[I2C_WIRE_MAX_MASTERS];
    uint64_t contenders;
    bool scl, sda, occupied, start_hold;
    bool address_byte, ack, read_valid, read_prepare_pending;
    uint8_t bits, byte;
    uint64_t phase, read_prepare_phase;
    int32_t error;
};

static void i2c_wire_schedule(I2CWire *wire);
static bool i2c_wire_slave_sda(const I2CWire *wire, bool scl);
static int i2c_wire_finish_load(I2CBus *bus);
static int i2c_wire_queue_service(I2CWire *wire, I2CSlave *slave,
                                  enum i2c_wire_service service);
static void i2c_clear_nodes(I2CNodeList *nodes);

static void i2c_bus_reset_hold(Object *obj, ResetType type)
{
    I2CBus *bus = I2C_BUS(obj);
    I2CWire *wire = bus->wire;
    BusChild *kid;

    if (!wire) {
        return;
    }
    timer_del(wire->timer);
    i2c_clear_nodes(&bus->current_devs);
    bus->owner = UINT32_MAX;
    bus->address_valid = false;
    bus->requested_address = 0;
    bus->master_recv = bus->broadcast = false;
    wire->contenders = 0;
    wire->bits = wire->byte = 0;
    wire->phase = wire->read_prepare_phase = 0;
    wire->address_byte = wire->ack = wire->read_valid = false;
    wire->read_prepare_pending = false;
    wire->error = 0;
    wire->occupied = wire->start_hold = false;
    wire->scl = wire->sda = true;
    for (unsigned i = 0; i < bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        m->sda = true;
        m->clock = m->scl_low = m->pending = m->paused = false;
        m->deadline = -1;
    }
    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);

        slave->wire_hold = slave->wire_hold_pending = false;
        slave->wire_hold_value = false;
        slave->wire_hold_at = 0;
        slave->wire_hold_phase = slave->wire_service_phase = 0;
        slave->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
        slave->wire_service_pending = false;
    }
    bus->wire_loading = false;
    bus->wire_slaves_loaded = 0;
}

/*
 * A timed wire finishes loading only when every slave on it has restored,
 * counted by the post-load hook that comes with the I2C slave state. A slave
 * that does not carry that state never reports in, the count stays short,
 * and the wire comes up on the destination with its clock stopped and
 * nothing to say why. Refuse the pairing instead.
 *
 * The state need not be a slave's own top-level field. A load runs the
 * post-load of every description it descends through, so an SMBus or PMBus
 * device that reaches the I2C slave two or three levels down counts as much
 * as one that names it directly. Search the whole tree rather than the
 * device's own fields.
 */
#define I2C_VMSTATE_MAX_DEPTH 8

static bool i2c_vmstate_carries_slave(const VMStateDescription *vmsd,
                                      unsigned depth)
{
    const VMStateField *field;
    const VMStateDescription * const *sub;

    if (!vmsd || depth >= I2C_VMSTATE_MAX_DEPTH) {
        return false;
    }
    if (vmsd == &vmstate_i2c_slave) {
        return true;
    }
    for (field = vmsd->fields; field && field->name; field++) {
        if (i2c_vmstate_carries_slave(field->vmsd, depth + 1)) {
            return true;
        }
    }
    for (sub = vmsd->subsections; sub && *sub; sub++) {
        if (i2c_vmstate_carries_slave(*sub, depth + 1)) {
            return true;
        }
    }
    return false;
}

static bool i2c_slave_state_migrated(DeviceState *dev)
{
    return i2c_vmstate_carries_slave(DEVICE_GET_CLASS(dev)->vmsd, 0);
}

static bool i2c_bus_check_address(BusState *bus, DeviceState *dev,
                                  Error **errp)
{
    if (I2C_BUS(bus)->wire && !i2c_slave_state_migrated(dev)) {
        error_setg(errp, "%s carries no I2C slave migration state and cannot "
                   "join a timed I2C wire",
                   object_get_typename(OBJECT(dev)));
        return false;
    }
    return true;
}

static void i2c_bus_class_init(ObjectClass *oc, const void *data)
{
    RESETTABLE_CLASS(oc)->phases.hold = i2c_bus_reset_hold;
    BUS_CLASS(oc)->check_address = i2c_bus_check_address;
}

static void i2c_bus_finalize(Object *obj)
{
    I2CBus *bus = I2C_BUS(obj);

    if (bus->wire) {
        timer_free(bus->wire->timer);
        g_free(bus->wire);
    }
}

static const Property i2c_props[] = {
    DEFINE_PROP_UINT8("address", struct I2CSlave, address, 0),
};

static const TypeInfo i2c_bus_info = {
    .name = TYPE_I2C_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(I2CBus),
    .instance_finalize = i2c_bus_finalize,
    .class_init = i2c_bus_class_init,
};

static const VMStateDescription vmstate_i2c_wire_master = {
    .name = "i2c-wire-master",
    .version_id = 2,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(low_ns, I2CWireMaster),
        VMSTATE_INT64(high_ns, I2CWireMaster),
        VMSTATE_INT64(deadline, I2CWireMaster),
        VMSTATE_INT64(staged_at, I2CWireMaster),
        VMSTATE_BOOL(configured, I2CWireMaster),
        VMSTATE_BOOL(sda, I2CWireMaster),
        VMSTATE_BOOL(clock, I2CWireMaster),
        VMSTATE_BOOL(scl_low, I2CWireMaster),
        VMSTATE_BOOL(pending, I2CWireMaster),
        VMSTATE_BOOL(staged_sda, I2CWireMaster),
        VMSTATE_BOOL(staged_clock, I2CWireMaster),
        VMSTATE_BOOL(clock_after_loss, I2CWireMaster),
        VMSTATE_BOOL_V(paused, I2CWireMaster, 2),
        VMSTATE_END_OF_LIST()
    },
};

static int i2c_wire_post_load(void *opaque, int version_id)
{
    I2CWire *wire = opaque;
    uint64_t members = (1ULL << wire->bus->master_count) - 1;

    if (wire->bits > 9 || wire->error > 0 ||
        (wire->contenders & ~members) ||
        (!wire->occupied && (wire->contenders || wire->bus->address_valid ||
                             wire->bits || wire->byte)) ||
        (wire->start_hold && (!wire->occupied || !wire->scl)) ||
        (wire->ack && (!wire->occupied || wire->bits < 8)) ||
        (wire->ack && wire->bus->master_recv && !wire->address_byte) ||
        (wire->address_byte && !wire->occupied) ||
        (wire->bits < 8 &&
         (wire->address_byte ||
          (wire->bus->address_valid && !wire->bus->master_recv)) &&
         wire->byte >= (1U << wire->bits)) ||
        (wire->read_valid &&
         (!wire->occupied || !wire->bus->address_valid ||
          !wire->bus->master_recv || wire->address_byte || wire->bits > 8)) ||
        (wire->read_prepare_pending &&
         (!wire->occupied || !wire->bus->address_valid ||
          !wire->bus->master_recv || wire->address_byte || wire->read_valid ||
          wire->bits || !wire->read_prepare_phase ||
          wire->read_prepare_phase != wire->phase)) ||
        (!wire->read_prepare_pending && wire->read_prepare_phase)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if ((m->configured && (m->low_ns <= 0 || m->high_ns <= 0)) ||
            (!m->configured && (m->clock || m->pending || m->paused)) ||
            (!m->clock && m->scl_low && !m->paused) ||
            (m->paused && (m->clock || !m->scl_low)) ||
            m->deadline < -1) {
            return -EINVAL;
        }
    }
    wire->bus->wire_loaded = true;
    return 0;
}

static uint32_t i2c_wire_slave_count(I2CBus *bus)
{
    BusChild *kid;
    uint32_t count = 0;

    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        count++;
    }
    return count;
}

static bool i2c_wire_service_state_valid(I2CWire *wire, I2CSlave *slave)
{
    enum i2c_wire_service service = slave->wire_service_kind;

    if ((!slave->wire_hold && slave->wire_hold_phase) ||
        (slave->wire_hold_phase && slave->wire_hold_phase != wire->phase) ||
        (slave->wire_hold_phase &&
         (!slave->saved_selected || wire->bits)) ||
        (slave->wire_hold_pending &&
         slave->wire_hold_at < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        return false;
    }
    if (!slave->wire_service_pending) {
        return !slave->wire_service_phase &&
               service == I2C_WIRE_SERVICE_COUNT;
    }
    if (!slave->saved_selected || slave->wire_hold ||
        !slave->wire_service_phase ||
        slave->wire_service_phase != wire->phase ||
        wire->bits < 8 || service >= I2C_WIRE_SERVICE_COUNT) {
        return false;
    }
    switch (service) {
    case I2C_WIRE_ADDRESS_SEND:
        return wire->address_byte && wire->bus->address_valid &&
               !wire->bus->master_recv;
    case I2C_WIRE_ADDRESS_RECV:
        return wire->address_byte && wire->bus->address_valid &&
               wire->bus->master_recv;
    case I2C_WIRE_WRITE:
        return !wire->address_byte && wire->bus->address_valid &&
               !wire->bus->master_recv;
    case I2C_WIRE_READ_ACK:
        return !wire->address_byte && wire->bus->address_valid &&
               wire->bus->master_recv;
    case I2C_WIRE_READ_NACK:
    case I2C_WIRE_SERVICE_COUNT:
        return false;
    }
    return false;
}

static int i2c_wire_finish_load(I2CBus *bus)
{
    I2CWire *wire = bus->wire;
    BusChild *kid;
    bool scl = true;
    bool sda;
    bool read_prepare_held = false;

    if (!wire || !wire->bus->wire_loaded ||
        bus->wire_slaves_loaded != i2c_wire_slave_count(bus)) {
        return -EINVAL;
    }
    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);

        if (!i2c_wire_service_state_valid(wire, slave)) {
            return -EINVAL;
        }
        if (wire->read_prepare_pending && slave->saved_selected &&
            slave->wire_hold &&
            slave->wire_hold_phase == wire->read_prepare_phase) {
            read_prepare_held = true;
        }
        scl &= !slave->wire_hold;
    }
    for (unsigned i = 0; i < bus->master_count; i++) {
        scl &= !wire->masters[i].scl_low;
    }
    sda = i2c_wire_slave_sda(wire, wire->scl);
    for (unsigned i = 0; i < bus->master_count; i++) {
        if (wire->contenders & (1ULL << i)) {
            sda &= wire->masters[i].sda;
        }
    }
    if ((wire->read_prepare_pending && !read_prepare_held) ||
        scl != wire->scl || sda != wire->sda) {
        return -EINVAL;
    }
    bus->wire_loading = false;
    i2c_wire_schedule(wire);
    return 0;
}

static const VMStateDescription vmstate_i2c_wire_state = {
    .name = "i2c-wire-state",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = i2c_wire_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(masters, I2CWire, I2C_WIRE_MAX_MASTERS, 1,
                              vmstate_i2c_wire_master, I2CWireMaster),
        VMSTATE_UINT64(contenders, I2CWire),
        VMSTATE_BOOL(scl, I2CWire),
        VMSTATE_BOOL(sda, I2CWire),
        VMSTATE_BOOL(occupied, I2CWire),
        VMSTATE_BOOL(start_hold, I2CWire),
        VMSTATE_BOOL(address_byte, I2CWire),
        VMSTATE_BOOL(ack, I2CWire),
        VMSTATE_BOOL(read_valid, I2CWire),
        VMSTATE_UINT8(bits, I2CWire),
        VMSTATE_UINT8(byte, I2CWire),
        VMSTATE_UINT64(phase, I2CWire),
        VMSTATE_UINT64(read_prepare_phase, I2CWire),
        VMSTATE_BOOL(read_prepare_pending, I2CWire),
        VMSTATE_INT32(error, I2CWire),
        VMSTATE_END_OF_LIST()
    },
};

static bool i2c_wire_needed(void *opaque)
{
    return ((I2CBus *)opaque)->wire != NULL;
}

static const VMStateDescription vmstate_i2c_wire = {
    .name = "i2c_bus/wire",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_wire_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(wire, I2CBus, vmstate_i2c_wire_state, I2CWire),
        VMSTATE_END_OF_LIST()
    },
};

static int i2c_bus_pre_save(void *opaque)
{
    I2CBus *bus = opaque;

    bus->saved_address = -1;
    if (!QLIST_EMPTY(&bus->current_devs)) {
        if (!bus->broadcast) {
            bus->saved_address = QLIST_FIRST(&bus->current_devs)->elt->address;
        } else {
            bus->saved_address = I2C_BROADCAST;
        }
    }

    return 0;
}

static void i2c_clear_nodes(I2CNodeList *nodes)
{
    I2CNode *node, *next;

    QLIST_FOREACH_SAFE(node, nodes, next, next) {
        QLIST_REMOVE(node, next);
        g_free(node);
    }
}

static bool i2c_masters_needed(void *opaque)
{
    return ((I2CBus *)opaque)->master_count != 0;
}

static int i2c_masters_post_load(void *opaque, int version_id)
{
    I2CBus *bus = opaque;

    bus->masters_loaded = true;
    bus->broadcast = bus->address_valid && !bus->master_recv &&
                     bus->requested_address == I2C_BROADCAST;
    if ((bus->owner != UINT32_MAX && bus->owner >= bus->master_count) ||
        (bus->address_valid && bus->owner == UINT32_MAX && !bus->wire) ||
        bus->requested_address > 0x7f) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_i2c_masters = {
    .name = "i2c_bus/masters",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_masters_needed,
    .post_load = i2c_masters_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(master_count, I2CBus),
        VMSTATE_UINT32(owner, I2CBus),
        VMSTATE_UINT8(requested_address, I2CBus),
        VMSTATE_BOOL(master_recv, I2CBus),
        VMSTATE_BOOL(address_valid, I2CBus),
        VMSTATE_END_OF_LIST()
    },
};

static int i2c_bus_pre_load(void *opaque)
{
    I2CBus *bus = opaque;

    bus->masters_loaded = false;
    bus->wire_loaded = false;
    bus->wire_loading = bus->wire != NULL;
    bus->wire_slaves_loaded = 0;
    if (bus->wire) {
        timer_del(bus->wire->timer);
    }
    if (bus->master_count) {
        /* Loading a replacement state is not a physical STOP. */
        i2c_clear_nodes(&bus->current_devs);
        bus->owner = UINT32_MAX;
    }
    return 0;
}

static int i2c_bus_post_load(void *opaque, int version_id)
{
    I2CBus *bus = opaque;

    if ((bus->master_count && !bus->masters_loaded) ||
        (bus->wire && !bus->wire_loaded)) {
        return -EINVAL;
    }
    if (bus->wire && !i2c_wire_slave_count(bus)) {
        return i2c_wire_finish_load(bus);
    }
    return 0;
}

const VMStateDescription vmstate_i2c_bus = {
    .name = "i2c_bus",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = i2c_bus_pre_save,
    .pre_load = i2c_bus_pre_load,
    .post_load = i2c_bus_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(saved_address, I2CBus),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_i2c_masters,
        &vmstate_i2c_wire,
        NULL
    },
};

/* Create a new I2C bus.  */
I2CBus *i2c_init_bus(DeviceState *parent, const char *name)
{
    I2CBus *bus;

    bus = I2C_BUS(qbus_new(TYPE_I2C_BUS, parent, name));
    QLIST_INIT(&bus->current_devs);
    QSIMPLEQ_INIT(&bus->pending_masters);
    vmstate_register_any(NULL, &vmstate_i2c_bus, bus);
    return bus;
}

void i2c_slave_set_address(I2CSlave *dev, uint8_t address)
{
    dev->address = address;
}

/* Return nonzero if bus is busy.  */
int i2c_bus_busy(I2CBus *bus)
{
    if (bus->wire) {
        return bus->wire->occupied;
    }
    if (bus->master_count) {
        return bus->owner != UINT32_MAX;
    }
    return !QLIST_EMPTY(&bus->current_devs) || bus->bh;
}

bool i2c_scan_bus(I2CBus *bus, uint8_t address, bool broadcast,
                  I2CNodeList *current_devs)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        DeviceState *qdev = kid->child;
        I2CSlave *candidate = I2C_SLAVE(qdev);
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(candidate);

        if (sc->match_and_add(candidate, address, broadcast, current_devs)) {
            if (!broadcast) {
                return true;
            }
        }
    }

    /*
     * If broadcast was true, and the list was full or empty, return true. If
     * broadcast was false, return false.
     */
    return broadcast;
}

static void i2c_notify_unmatched_address(I2CBus *bus, uint8_t address,
                                         bool recv)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(slave);

        if (sc->unmatched_address) {
            sc->unmatched_address(slave, address, recv);
        }
    }
}

/* TODO: Make this handle multiple masters.  */
/*
 * Start or continue an i2c transaction.  When this is called for the
 * first time or after an i2c_end_transfer(), if it returns an error
 * the bus transaction is terminated (or really never started).  If
 * this is called after another i2c_start_transfer() without an
 * intervening i2c_end_transfer(), and it returns an error, the
 * transaction will not be terminated.  The caller must do it.
 *
 * This corresponds with the way real hardware works.  The SMBus
 * protocol uses a start transfer to switch from write to read mode
 * without releasing the bus.  If that fails, the bus is still
 * in a transaction.
 *
 * @event must be I2C_START_RECV or I2C_START_SEND.
 */
static int i2c_do_start_transfer(I2CBus *bus, uint8_t address,
                                 enum i2c_event event)
{
    I2CSlaveClass *sc;
    I2CNode *node;
    bool bus_scanned = false;

    assert(!bus->master_count);

    if (address == I2C_BROADCAST) {
        /*
         * This is a broadcast, the current_devs will be all the devices of the
         * bus.
         */
        bus->broadcast = true;
    }

    /*
     * If there are already devices in the list, that means we are in
     * the middle of a transaction and we shouldn't rescan the bus.
     *
     * This happens with any SMBus transaction, even on a pure I2C
     * device.  The interface does a transaction start without
     * terminating the previous transaction.
     */
    if (QLIST_EMPTY(&bus->current_devs)) {
        /* Disregard whether devices were found. */
        (void)i2c_scan_bus(bus, address, bus->broadcast, &bus->current_devs);
        bus_scanned = true;
    }

    if (QLIST_EMPTY(&bus->current_devs)) {
        if (bus_scanned) {
            i2c_notify_unmatched_address(bus, address,
                                         event == I2C_START_RECV);
        }
        return 1;
    }

    QLIST_FOREACH(node, &bus->current_devs, next) {
        I2CSlave *s = node->elt;
        int rv;

        sc = I2C_SLAVE_GET_CLASS(s);
        /* If the bus is already busy, assume this is a repeated
           start condition.  */

        if (sc->event) {
            trace_i2c_event(event == I2C_START_SEND ? "start" : "start_async",
                            s->address);
            rv = sc->event(s, event);
            if (rv && !bus->broadcast) {
                if (bus_scanned) {
                    /* First call, terminate the transfer. */
                    i2c_end_transfer(bus);
                }
                return rv;
            }
        }
    }
    return 0;
}

int i2c_start_transfer(I2CBus *bus, uint8_t address, bool is_recv)
{
    return i2c_do_start_transfer(bus, address, is_recv
                                               ? I2C_START_RECV
                                               : I2C_START_SEND);
}

void i2c_bus_master(I2CBus *bus, QEMUBH *bh)
{
    assert(!bus->master_count);
    I2CPendingMaster *node = g_new(struct I2CPendingMaster, 1);
    node->bh = bh;

    QSIMPLEQ_INSERT_TAIL(&bus->pending_masters, node, entry);
}

void i2c_schedule_pending_master(I2CBus *bus)
{
    assert(!bus->master_count);
    I2CPendingMaster *node;

    if (i2c_bus_busy(bus)) {
        /* someone is already controlling the bus; wait for it to release it */
        return;
    }

    if (QSIMPLEQ_EMPTY(&bus->pending_masters)) {
        return;
    }

    node = QSIMPLEQ_FIRST(&bus->pending_masters);
    bus->bh = node->bh;

    QSIMPLEQ_REMOVE_HEAD(&bus->pending_masters, entry);
    g_free(node);

    qemu_bh_schedule(bus->bh);
}

void i2c_bus_release(I2CBus *bus)
{
    assert(!bus->master_count);
    bus->bh = NULL;

    i2c_schedule_pending_master(bus);
}

int i2c_start_recv(I2CBus *bus, uint8_t address)
{
    return i2c_do_start_transfer(bus, address, I2C_START_RECV);
}

int i2c_start_send(I2CBus *bus, uint8_t address)
{
    return i2c_do_start_transfer(bus, address, I2C_START_SEND);
}

int i2c_start_send_async(I2CBus *bus, uint8_t address)
{
    return i2c_do_start_transfer(bus, address, I2C_START_SEND_ASYNC);
}

static void i2c_do_end_transfer(I2CBus *bus)
{
    I2CSlaveClass *sc;
    I2CNode *node, *next;

    QLIST_FOREACH_SAFE(node, &bus->current_devs, next, next) {
        I2CSlave *s = node->elt;
        sc = I2C_SLAVE_GET_CLASS(s);
        if (sc->event) {
            trace_i2c_event("finish", s->address);
            sc->event(s, I2C_FINISH);
        }
        QLIST_REMOVE(node, next);
        g_free(node);
    }
    bus->broadcast = false;
}

static int i2c_do_send(I2CBus *bus, uint8_t data)
{
    I2CSlaveClass *sc;
    I2CSlave *s;
    I2CNode *node;
    int ret = 0;

    QLIST_FOREACH(node, &bus->current_devs, next) {
        s = node->elt;
        sc = I2C_SLAVE_GET_CLASS(s);
        if (sc->send) {
            trace_i2c_send(s->address, data);
            ret = ret || sc->send(s, data);
        } else {
            ret = -1;
        }
    }

    return ret ? -1 : 0;
}

int i2c_send_async(I2CBus *bus, uint8_t data)
{
    assert(!bus->master_count);
    I2CNode *node = QLIST_FIRST(&bus->current_devs);
    I2CSlave *slave = node->elt;
    I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(slave);

    if (!sc->send_async) {
        return -1;
    }

    trace_i2c_send_async(slave->address, data);

    sc->send_async(slave, data);

    return 0;
}

static uint8_t i2c_do_recv(I2CBus *bus)
{
    uint8_t data = 0xff;
    I2CSlaveClass *sc;
    I2CSlave *s;

    if (!QLIST_EMPTY(&bus->current_devs) && !bus->broadcast) {
        sc = I2C_SLAVE_GET_CLASS(QLIST_FIRST(&bus->current_devs)->elt);
        if (sc->recv) {
            s = QLIST_FIRST(&bus->current_devs)->elt;
            data = sc->recv(s);
            trace_i2c_recv(s->address, data);
        }
    }

    return data;
}

static void i2c_do_nack(I2CBus *bus)
{
    I2CSlaveClass *sc;
    I2CNode *node;

    if (QLIST_EMPTY(&bus->current_devs)) {
        return;
    }

    QLIST_FOREACH(node, &bus->current_devs, next) {
        sc = I2C_SLAVE_GET_CLASS(node->elt);
        if (sc->event) {
            trace_i2c_event("nack", node->elt->address);
            sc->event(node->elt, I2C_NACK);
        }
    }
}

void i2c_ack(I2CBus *bus)
{
    assert(!bus->master_count);
    if (!bus->bh) {
        return;
    }

    trace_i2c_ack();

    qemu_bh_schedule(bus->bh);
}

static int i2c_slave_post_load(void *opaque, int version_id)
{
    I2CSlave *dev = opaque;
    I2CBus *bus;
    I2CNode *node;

    bus = I2C_BUS(qdev_get_parent_bus(DEVICE(dev)));
    if (bus->master_count && dev->saved_selected &&
        (!bus->address_valid || (bus->owner == UINT32_MAX && !bus->wire) ||
         !I2C_SLAVE_GET_CLASS(dev)->supports_master_ids)) {
        return -EINVAL;
    }
    if (bus->master_count ? dev->saved_selected :
        ((bus->saved_address == dev->address) ||
         (bus->saved_address == I2C_BROADCAST))) {
        node = g_new(struct I2CNode, 1);
        node->elt = dev;
        QLIST_INSERT_HEAD(&bus->current_devs, node, next);
    }
    if (bus->wire) {
        if (bus->wire_loading) {
            bus->wire_slaves_loaded++;
            if (bus->wire_slaves_loaded > i2c_wire_slave_count(bus)) {
                return -EINVAL;
            }
            if (bus->wire_slaves_loaded == i2c_wire_slave_count(bus)) {
                return i2c_wire_finish_load(bus);
            }
        } else {
            i2c_wire_schedule(bus->wire);
        }
    }
    return 0;
}

static bool i2c_slave_selected_needed(void *opaque)
{
    I2CSlave *s = opaque;

    return I2C_BUS(qdev_get_parent_bus(DEVICE(s)))->master_count != 0;
}

static int i2c_slave_pre_save(void *opaque)
{
    I2CSlave *s = opaque;
    I2CBus *bus = I2C_BUS(qdev_get_parent_bus(DEVICE(s)));
    I2CNode *node;

    s->saved_selected = false;
    QLIST_FOREACH(node, &bus->current_devs, next) {
        if (node->elt == s) {
            s->saved_selected = true;
            break;
        }
    }
    return 0;
}

static int i2c_slave_pre_load(void *opaque)
{
    ((I2CSlave *)opaque)->saved_selected = false;
    return 0;
}

static const VMStateDescription vmstate_i2c_slave_selected = {
    .name = "I2CSlave/selected",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_slave_selected_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(saved_selected, I2CSlave),
        VMSTATE_END_OF_LIST()
    },
};

static bool i2c_slave_wire_needed(void *opaque)
{
    I2CSlave *slave = opaque;

    return I2C_BUS(qdev_get_parent_bus(DEVICE(slave)))->wire != NULL;
}

static const VMStateDescription vmstate_i2c_slave_wire = {
    .name = "I2CSlave/wire",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_slave_wire_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(wire_hold, I2CSlave),
        VMSTATE_BOOL(wire_hold_pending, I2CSlave),
        VMSTATE_BOOL(wire_hold_value, I2CSlave),
        VMSTATE_INT64(wire_hold_at, I2CSlave),
        VMSTATE_UINT64(wire_hold_phase, I2CSlave),
        VMSTATE_UINT64(wire_service_phase, I2CSlave),
        VMSTATE_UINT8(wire_service_kind, I2CSlave),
        VMSTATE_BOOL(wire_service_pending, I2CSlave),
        VMSTATE_END_OF_LIST()
    },
};

const VMStateDescription vmstate_i2c_slave = {
    .name = "I2CSlave",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = i2c_slave_pre_save,
    .pre_load = i2c_slave_pre_load,
    .post_load = i2c_slave_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(address, I2CSlave),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_i2c_slave_selected,
        &vmstate_i2c_slave_wire,
        NULL
    },
};

I2CSlave *i2c_slave_new(const char *name, uint8_t addr)
{
    DeviceState *dev;

    dev = qdev_new(name);
    qdev_prop_set_uint8(dev, "address", addr);
    return I2C_SLAVE(dev);
}

bool i2c_slave_realize_and_unref(I2CSlave *dev, I2CBus *bus, Error **errp)
{
    return qdev_realize_and_unref(&dev->qdev, &bus->qbus, errp);
}

I2CSlave *i2c_slave_create_simple(I2CBus *bus, const char *name, uint8_t addr)
{
    I2CSlave *dev = i2c_slave_new(name, addr);

    i2c_slave_realize_and_unref(dev, bus, &error_abort);

    return dev;
}

static bool i2c_slave_match(I2CSlave *candidate, uint8_t address,
                            bool broadcast, I2CNodeList *current_devs)
{
    if ((candidate->address == address) || (broadcast)) {
        I2CNode *node = g_new(struct I2CNode, 1);
        node->elt = candidate;
        QLIST_INSERT_HEAD(current_devs, node, next);
        return true;
    }

    /* Not found and not broadcast. */
    return false;
}

static void i2c_slave_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *k = DEVICE_CLASS(klass);
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(klass);
    set_bit(DEVICE_CATEGORY_MISC, k->categories);
    k->bus_type = TYPE_I2C_BUS;
    device_class_set_props(k, i2c_props);
    sc->match_and_add = i2c_slave_match;
}

/*
 * A slave created after the wire was enabled would otherwise start on the
 * first service kind rather than on "none", which is not a state the wire's
 * own invariants accept.
 */
static void i2c_slave_instance_init(Object *obj)
{
    I2C_SLAVE(obj)->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
}

static const TypeInfo i2c_slave_type_info = {
    .name = TYPE_I2C_SLAVE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(I2CSlave),
    .instance_init = i2c_slave_instance_init,
    .abstract = true,
    .class_size = sizeof(I2CSlaveClass),
    .class_init = i2c_slave_class_init,
};

static void i2c_slave_register_types(void)
{
    type_register_static(&i2c_bus_info);
    type_register_static(&i2c_slave_type_info);
}

type_init(i2c_slave_register_types)


void i2c_end_transfer(I2CBus *bus)
{
    assert(!bus->master_count);
    i2c_do_end_transfer(bus);
}

int i2c_send(I2CBus *bus, uint8_t data)
{
    assert(!bus->master_count);
    return i2c_do_send(bus, data);
}

uint8_t i2c_recv(I2CBus *bus)
{
    assert(!bus->master_count);
    return i2c_do_recv(bus);
}

void i2c_nack(I2CBus *bus)
{
    assert(!bus->master_count);
    i2c_do_nack(bus);
}

int i2c_bus_enable_master_ids(I2CBus *bus, uint32_t count)
{
    if (!count) {
        return -EINVAL;
    }
    if (bus->master_count || i2c_bus_busy(bus) ||
        !QSIMPLEQ_EMPTY(&bus->pending_masters)) {
        return -EBUSY;
    }
    bus->master_count = count;
    bus->owner = UINT32_MAX;
    return 0;
}

int i2c_bus_start_resolved(I2CBus *bus, uint32_t master)
{
    if (bus->wire) {
        return -ENOTSUP;
    }
    if (!bus->master_count || master >= bus->master_count) {
        return -EINVAL;
    }
    if (bus->owner != UINT32_MAX) {
        return -EBUSY;
    }
    /* The caller has resolved the wire event; this does not elect a master. */
    bus->owner = master;
    bus->address_valid = false;
    return 0;
}

static bool i2c_master_owns(I2CBus *bus, uint32_t master)
{
    return !bus->wire && bus->master_count && master < bus->master_count &&
           bus->owner == master;
}

static int i2c_resolved_start_transfer(I2CBus *bus, uint8_t address,
                                       bool recv)
{
    I2CNodeList matches = QLIST_HEAD_INITIALIZER(matches);
    I2CNode *node, *next;
    bool broadcast = address == I2C_BROADCAST && !recv;

    if (address > 0x7f) {
        return -EINVAL;
    }
    /* Address byte 0x01 is the START byte; no target acknowledges it. */
    if (address != I2C_BROADCAST || !recv) {
        i2c_scan_bus(bus, address, broadcast, &matches);
    }
    QLIST_FOREACH(node, &matches, next) {
        if (!I2C_SLAVE_GET_CLASS(node->elt)->supports_master_ids) {
            i2c_clear_nodes(&matches);
            return -ENOTSUP;
        }
    }
    if (bus->address_valid) {
        QLIST_FOREACH(node, &bus->current_devs, next) {
            I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

            if (sc->event) {
                sc->event(node->elt, I2C_RESTART);
            }
        }
    }
    i2c_clear_nodes(&bus->current_devs);
    QLIST_SWAP(&bus->current_devs, &matches, next);
    bus->requested_address = address;
    bus->master_recv = recv;
    bus->address_valid = true;
    bus->broadcast = broadcast;
    QLIST_FOREACH_SAFE(node, &bus->current_devs, next, next) {
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

        if (sc->event && sc->event(node->elt, recv ? I2C_START_RECV :
                                                   I2C_START_SEND)) {
            /* Address NAK removes only this receiver, without STOP. */
            QLIST_REMOVE(node, next);
            g_free(node);
        }
    }
    return QLIST_EMPTY(&bus->current_devs) ? 1 : 0;
}

int i2c_master_start_transfer(I2CBus *bus, uint32_t master,
                              uint8_t address, bool recv)
{
    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    return i2c_resolved_start_transfer(bus, address, recv);
}

static int i2c_resolved_send(I2CBus *bus, uint8_t data, I2CWire *wire)
{
    I2CNode *node;
    bool ack = false;

    if (!bus->address_valid || bus->master_recv) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&bus->current_devs)) {
        return -ENXIO;
    }
    QLIST_FOREACH(node, &bus->current_devs, next) {
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

        if (sc->send) {
            int ret;

            trace_i2c_send(node->elt->address, data);
            /* Deliver once to every receiver; any ACK pulls the line low. */
            ret = sc->send(node->elt, data);
            if (!ret) {
                ack = true;
                if (wire && i2c_wire_queue_service(
                        wire, node->elt, I2C_WIRE_WRITE)) {
                    return wire->error;
                }
            }
        }
    }
    return ack ? 0 : 1;
}

int i2c_master_send(I2CBus *bus, uint32_t master, uint8_t data)
{
    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    return i2c_resolved_send(bus, data, NULL);
}

int i2c_master_recv(I2CBus *bus, uint32_t master, uint8_t *data)
{
    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    if (!bus->address_valid || !bus->master_recv || !data || bus->broadcast) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&bus->current_devs)) {
        return -ENXIO;
    }
    *data = i2c_do_recv(bus);
    return 0;
}

int i2c_master_nack(I2CBus *bus, uint32_t master)
{
    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    if (!bus->address_valid || !bus->master_recv) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&bus->current_devs)) {
        return -ENXIO;
    }
    i2c_do_nack(bus);
    return 0;
}

int i2c_master_end_transfer(I2CBus *bus, uint32_t master)
{
    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    i2c_do_end_transfer(bus);
    bus->owner = UINT32_MAX;
    bus->address_valid = false;
    return 0;
}

static int i2c_wire_queue_service(I2CWire *wire, I2CSlave *slave,
                                  enum i2c_wire_service service)
{
    if (slave->wire_service_pending || service >= I2C_WIRE_SERVICE_COUNT) {
        wire->error = -EINVAL;
        return wire->error;
    }
    slave->wire_service_pending = true;
    slave->wire_service_phase = wire->phase;
    slave->wire_service_kind = service;
    return 0;
}

static int i2c_wire_queue_selected(I2CWire *wire,
                                   enum i2c_wire_service service,
                                   bool first_only)
{
    I2CNode *node;

    QLIST_FOREACH(node, &wire->bus->current_devs, next) {
        int ret = i2c_wire_queue_service(wire, node->elt, service);

        if (ret || first_only) {
            return ret;
        }
    }
    return 0;
}

static void i2c_wire_complete_services(I2CWire *wire, bool read_acked)
{
    I2CNode *node;

    QLIST_FOREACH(node, &wire->bus->current_devs, next) {
        I2CSlave *slave = node->elt;
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(slave);
        enum i2c_wire_service service = slave->wire_service_kind;
        bool hold = false;

        if (!slave->wire_service_pending ||
            slave->wire_service_phase != wire->phase) {
            continue;
        }
        if (service == I2C_WIRE_READ_ACK) {
            service = read_acked ? I2C_WIRE_READ_ACK : I2C_WIRE_READ_NACK;
        }
        slave->wire_service_pending = false;
        slave->wire_service_phase = 0;
        slave->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
        if (sc->wire_service) {
            hold = sc->wire_service(slave, service);
        }
        if (hold) {
            if (slave->wire_hold || slave->wire_hold_pending) {
                wire->error = -ENOTSUP;
                continue;
            }
            slave->wire_hold = true;
            slave->wire_hold_phase = wire->phase;
        }
    }
}

static bool i2c_wire_read_prepare_held(I2CWire *wire)
{
    I2CNode *node;

    QLIST_FOREACH(node, &wire->bus->current_devs, next) {
        I2CSlave *slave = node->elt;

        if (slave->wire_hold &&
            slave->wire_hold_phase == wire->read_prepare_phase) {
            return true;
        }
    }
    return false;
}

static void i2c_wire_prepare_read(I2CWire *wire)
{
    if (!wire->read_prepare_pending || i2c_wire_read_prepare_held(wire)) {
        return;
    }
    wire->byte = i2c_do_recv(wire->bus);
    wire->read_valid = true;
    wire->read_prepare_pending = false;
    wire->read_prepare_phase = 0;
}

static void i2c_wire_schedule(I2CWire *wire)
{
    int64_t next = 0;
    bool found = false;
    BusChild *kid;

    if (wire->error || wire->bus->wire_loading) {
        timer_del(wire->timer);
        return;
    }

    QTAILQ_FOREACH(kid, &wire->bus->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);

        if (slave->wire_hold_pending) {
            if (!found || slave->wire_hold_at < next) {
                next = slave->wire_hold_at;
                found = true;
            }
        }
    }
    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if (m->pending) {
            if (!found || m->staged_at < next) {
                next = m->staged_at;
                found = true;
            }
        }
        if (m->clock && m->deadline >= 0) {
            if (!found || m->deadline < next) {
                next = m->deadline;
                found = true;
            }
        }
    }
    if (!found) {
        timer_del(wire->timer);
    } else {
        timer_mod(wire->timer, next);
    }
}

static bool i2c_wire_slave_sda(const I2CWire *wire, bool scl)
{
    unsigned int bit = wire->bits;

    if (wire->ack) {
        return false;
    }
    if (wire->occupied && wire->bus->address_valid &&
        wire->bus->master_recv && !wire->address_byte && wire->read_valid) {
        /* The sampled bit remains stable until SCL falls. */
        if (scl && bit) {
            bit--;
        }
        if (bit < 8) {
            return (wire->byte >> (7 - bit)) & 1;
        }
    }
    return true;
}

static bool i2c_wire_set_deadlines(I2CWire *wire, int64_t now, bool low)
{
    int64_t deadlines[I2C_WIRE_MAX_MASTERS];

    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];
        int64_t period = low ? m->low_ns : m->high_ns;

        if (m->clock && sadd64_overflow(now, period, &deadlines[i])) {
            for (unsigned j = 0; j < wire->bus->master_count; j++) {
                if (wire->masters[j].clock) {
                    wire->masters[j].deadline = -1;
                }
            }
            wire->error = -ERANGE;
            return false;
        }
    }
    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if (m->clock) {
            m->deadline = deadlines[i];
        }
    }
    return true;
}

/* Join the low period at the same falling edge as an existing clock. */
static int i2c_wire_join_low_deadline(I2CWire *wire,
                                      I2CWireMaster *joining,
                                      int64_t *deadline)
{
    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];
        int64_t low_started;

        if (m == joining || !m->clock || !m->scl_low || m->deadline < 0) {
            continue;
        }
        low_started = m->deadline - m->low_ns;
        if (sadd64_overflow(low_started, joining->low_ns, deadline)) {
            return -ERANGE;
        }
        return 0;
    }
    return -ENOTSUP;
}

static void i2c_wire_tick(void *opaque)
{
    I2CWire *wire = opaque;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    bool old_scl = wire->scl, old_sda = wire->sda;
    uint64_t old_contenders = wire->contenders;
    bool scl = true, sda;
    uint64_t lost = 0;
    uint64_t restart_attempts = 0, stop_attempts = 0;
    int old_error = wire->error;
    BusChild *kid;

    QTAILQ_FOREACH(kid, &wire->bus->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);

        if (slave->wire_hold_pending && slave->wire_hold_at <= now) {
            slave->wire_hold_pending = false;
            slave->wire_hold = slave->wire_hold_value;
            if (!slave->wire_hold) {
                slave->wire_hold_phase = 0;
            }
        }
        scl &= !slave->wire_hold;
    }
    i2c_wire_prepare_read(wire);
    sda = i2c_wire_slave_sda(wire, wire->scl);
    /* Apply the complete deadline batch before resolving either line. */
    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if (m->pending && m->staged_at <= now) {
            bool join_low = !m->clock && m->staged_clock && !old_scl &&
                            !m->paused;
            int64_t join_deadline = -1;

            m->pending = false;
            if (join_low) {
                if (!(old_contenders & (1ULL << i))) {
                    wire->error = -ENOTSUP;
                    continue;
                }
                wire->error = i2c_wire_join_low_deadline(wire, m,
                                                         &join_deadline);
                if (wire->error) {
                    continue;
                }
            }
            if ((old_contenders & (1ULL << i)) && wire->occupied &&
                old_scl && !m->staged_clock) {
                if (m->sda && !m->staged_sda) {
                    restart_attempts |= 1ULL << i;
                } else if (m->staged_sda) {
                    stop_attempts |= 1ULL << i;
                }
            }
            m->sda = m->staged_sda;
            if ((!wire->occupied || wire->start_hold) && !m->sda) {
                wire->contenders |= 1ULL << i;
            }
            if (m->clock != m->staged_clock) {
                bool resume = m->paused && m->staged_clock;

                m->clock = m->staged_clock;
                m->paused = false;
                /* A resumed master already holds SCL: run its high phase. */
                if (join_low) {
                    m->scl_low = join_deadline > now;
                    m->deadline = m->scl_low ? join_deadline : -1;
                } else {
                    m->scl_low = resume ? false : m->clock;
                    m->deadline = -1;
                }
            }
        }
        if (m->clock && m->deadline >= 0 && m->deadline <= now) {
            m->scl_low = wire->scl;
            m->deadline = -1;
        }
        scl &= !m->scl_low;
        if (wire->contenders & (1ULL << i)) {
            sda &= m->sda;
        }
    }
    if (wire->error) {
        goto publish;
    }
    if ((restart_attempts && restart_attempts != old_contenders) ||
        (stop_attempts && stop_attempts != old_contenders)) {
        wire->error = -ENOTSUP;
        goto publish;
    }
    if (old_scl && scl && old_sda && !sda) {
        if (wire->occupied) {
            I2CNode *node;

            QLIST_FOREACH(node, &wire->bus->current_devs, next) {
                I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

                if (sc->event) {
                    sc->event(node->elt, I2C_RESTART);
                }
            }
            i2c_clear_nodes(&wire->bus->current_devs);
            wire->bus->address_valid = false;
        }
        wire->occupied = true;
        wire->start_hold = true;
        wire->address_byte = true;
        wire->bits = wire->byte = 0;
        wire->ack = wire->read_valid = false;
        wire->read_prepare_pending = false;
        wire->read_prepare_phase = 0;
    } else if (old_scl && scl && !old_sda && sda) {
        wire->occupied = false;
        wire->start_hold = false;
        wire->contenders = 0;
        wire->bits = wire->byte = 0;
        wire->ack = wire->read_valid = false;
        wire->read_prepare_pending = false;
        wire->read_prepare_phase = 0;
        i2c_do_end_transfer(wire->bus);
        wire->bus->address_valid = false;
    }
    if (old_scl && !scl) {
        wire->start_hold = false;
        if (!i2c_wire_set_deadlines(wire, now, true)) {
            goto publish;
        }
        for (unsigned i = 0; i < wire->bus->master_count; i++) {
            if (wire->masters[i].clock) {
                wire->masters[i].scl_low = true;
            }
        }
    } else if (!old_scl && scl) {
        if (!i2c_wire_set_deadlines(wire, now, false)) {
            goto publish;
        }
        for (unsigned i = 0; i < wire->bus->master_count; i++) {
            I2CWireMaster *m = &wire->masters[i];
            uint64_t bit = 1ULL << i;

            if (((wire->bits < 8 &&
                  !(wire->bus->address_valid && wire->bus->master_recv &&
                    !wire->address_byte)) ||
                 (wire->bits == 8 && wire->bus->address_valid &&
                  wire->bus->master_recv && !wire->address_byte)) &&
                (wire->contenders & bit) &&
                m->sda && !sda) {
                lost |= bit;
                wire->contenders &= ~bit;
                if (!m->clock_after_loss) {
                    m->clock = false;
                }
            }
        }
    }
    if (!old_scl && scl && wire->occupied) {
        if (wire->bits < 8 &&
            (wire->address_byte || !wire->bus->master_recv)) {
            wire->byte = (wire->byte << 1) | sda;
        }
        wire->bits++;
    }
    if (old_scl && !scl && wire->occupied) {
        if (wire->bits == 8) {
            int ret;

            if (wire->phase == UINT64_MAX) {
                wire->error = -ERANGE;
                goto publish;
            }
            wire->phase++;
            if (wire->address_byte) {
                ret = i2c_resolved_start_transfer(wire->bus, wire->byte >> 1,
                                                   wire->byte & 1);
                if (!ret) {
                    ret = i2c_wire_queue_selected(
                        wire, wire->bus->master_recv ?
                        I2C_WIRE_ADDRESS_RECV : I2C_WIRE_ADDRESS_SEND,
                        false);
                }
            } else if (wire->bus->master_recv) {
                ret = 1;
                wire->read_valid = false;
                if (!QLIST_EMPTY(&wire->bus->current_devs)) {
                    ret = i2c_wire_queue_selected(
                        wire, I2C_WIRE_READ_ACK, true);
                }
            } else if (QLIST_EMPTY(&wire->bus->current_devs)) {
                ret = 1;
            } else {
                ret = i2c_resolved_send(wire->bus, wire->byte, wire);
            }
            wire->error = MIN(ret, 0);
            wire->ack = !wire->bus->master_recv && ret == 0;
            if (wire->address_byte) {
                wire->ack = ret == 0;
            }
        } else if (wire->bits == 9) {
            bool was_address = wire->address_byte;
            bool target_acked = wire->ack;
            bool master_acked = !old_sda;
            uint8_t address_byte = wire->byte;

            if (was_address) {
                BusChild *child;

                QTAILQ_FOREACH(child, &wire->bus->qbus.children, sibling) {
                    I2CSlave *slave = I2C_SLAVE(child->child);
                    I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(slave);

                    if (sc->wire_address) {
                        sc->wire_address(slave, address_byte);
                    }
                }
            }
            wire->bits = wire->byte = 0;
            wire->address_byte = wire->ack = false;
            if (wire->bus->master_recv &&
                !QLIST_EMPTY(&wire->bus->current_devs)) {
                if (was_address && target_acked) {
                    wire->read_prepare_pending = true;
                    wire->read_prepare_phase = wire->phase;
                } else if (!was_address && master_acked) {
                    wire->read_prepare_pending = true;
                    wire->read_prepare_phase = wire->phase;
                } else if (!was_address) {
                    i2c_do_nack(wire->bus);
                }
            }
            i2c_wire_complete_services(wire, master_acked);
            if (wire->error) {
                goto publish;
            }
            i2c_wire_prepare_read(wire);
            for (unsigned i = 0; i < wire->bus->master_count; i++) {
                I2CWireMaster *m = &wire->masters[i];

                if (!(wire->contenders & (1ULL << i))) {
                    m->clock = m->scl_low = false;
                    m->deadline = -1;
                }
            }
        }
    }
publish:
    /* Target ACK is a separate line drive, never a master arbitration bit. */
    sda = i2c_wire_slave_sda(wire, scl);
    for (unsigned i = 0; i < wire->bus->master_count; i++) {
        if (wire->contenders & (1ULL << i)) {
            sda &= wire->masters[i].sda;
        }
    }
    wire->scl = scl;
    wire->sda = sda;
    if (old_scl != scl || old_sda != sda ||
        old_contenders != wire->contenders || old_error != wire->error) {
        I2CWireEvent event = {
            .time = now, .contenders = wire->contenders, .lost = lost,
            .scl = scl, .sda = sda, .error = wire->error,
        };

        if (wire->observe) {
            wire->observe(wire->opaque, &event);
        }
        /*
         * Each controller reads the shared lines for itself, so the same
         * event goes to every attached observer before anything is
         * rescheduled: one of them may pause its clock or stage the next
         * transition from inside the callback.
         */
        for (unsigned i = 0; i < wire->bus->master_count; i++) {
            if (wire->master_observe[i]) {
                wire->master_observe[i](wire->master_opaque[i], &event);
            }
        }
    }
    i2c_wire_schedule(wire);
}

int i2c_bus_enable_wire(I2CBus *bus, uint32_t count,
                         I2CWireObserver observe, void *opaque, Error **errp)
{
    BusChild *kid;
    int ret;

    if (!count || count > I2C_WIRE_MAX_MASTERS) {
        error_setg(errp, "a timed I2C wire takes 1 to %u participants",
                   I2C_WIRE_MAX_MASTERS);
        return -EINVAL;
    }
    if (bus->wire || i2c_bus_busy(bus)) {
        error_setg(errp, "the bus already has a wire or an open transfer");
        return -EBUSY;
    }
    if (bus->master_count && bus->master_count != count) {
        error_setg(errp, "the bus already has %u participants, not %u",
                   bus->master_count, count);
        return -EINVAL;
    }
    /*
     * Before anything is changed: the attach-time refusal cannot see a slave
     * that was already here, and a half-enabled bus is worse than a refused
     * one.
     */
    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        if (!i2c_slave_state_migrated(kid->child)) {
            error_setg(errp, "%s is already on this bus and carries no I2C "
                       "slave migration state",
                       object_get_typename(OBJECT(kid->child)));
            return -EINVAL;
        }
    }
    if (!bus->master_count) {
        ret = i2c_bus_enable_master_ids(bus, count);
        if (ret) {
            error_setg(errp, "cannot give the bus %u participants", count);
            return ret;
        }
    }
    bus->wire = g_new0(I2CWire, 1);
    bus->wire->bus = bus;
    bus->wire->observe = observe;
    bus->wire->opaque = opaque;
    bus->wire->scl = bus->wire->sda = true;
    for (unsigned i = 0; i < count; i++) {
        bus->wire->masters[i].sda = true;
        bus->wire->masters[i].deadline = -1;
    }
    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        I2C_SLAVE(kid->child)->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
    }
    bus->wire->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      i2c_wire_tick, bus->wire);
    return 0;
}

int i2c_wire_configure(I2CBus *bus, uint32_t master, int64_t low_ns,
                       int64_t high_ns, bool clock_after_loss)
{
    I2CWireMaster *m;

    if (!bus->wire || master >= bus->master_count || low_ns <= 0 ||
        high_ns <= 0) {
        return -EINVAL;
    }
    m = &bus->wire->masters[master];
    if (m->clock || m->pending) {
        return -EBUSY;
    }
    m->configured = true;
    m->low_ns = low_ns;
    m->high_ns = high_ns;
    m->clock_after_loss = clock_after_loss;
    return 0;
}

static int i2c_wire_stage_check(I2CBus *bus, uint32_t master)
{
    if (!bus->wire || master >= bus->master_count) {
        return -EINVAL;
    }
    return bus->wire->error;
}

static int i2c_wire_stage_at(I2CBus *bus, uint32_t master, int64_t at,
                             int64_t now, bool sda, bool clock)
{
    I2CWireMaster *m;

    if (at <= now) {
        return -EINVAL;
    }
    m = &bus->wire->masters[master];
    if (!m->configured ||
        at > INT64_MAX - MAX(m->low_ns, m->high_ns)) {
        return -EINVAL;
    }
    if (m->pending) {
        return -EBUSY;
    }
    m->pending = true;
    m->staged_at = at;
    m->staged_sda = sda;
    m->staged_clock = clock;
    i2c_wire_schedule(bus->wire);
    return 0;
}

int i2c_wire_stage(I2CBus *bus, uint32_t master, int64_t at,
                   bool sda, bool clock)
{
    int ret = i2c_wire_stage_check(bus, master);

    if (ret) {
        return ret;
    }
    return i2c_wire_stage_at(bus, master, at,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL),
                             sda, clock);
}

int i2c_wire_stage_at_or_after(I2CBus *bus, uint32_t master, int64_t at,
                               int64_t min_delay_ns, bool sda, bool clock)
{
    int64_t now;
    int ret = i2c_wire_stage_check(bus, master);

    if (ret) {
        return ret;
    }
    if (min_delay_ns <= 0) {
        return -EINVAL;
    }
    now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
    if (now > INT64_MAX - min_delay_ns) {
        return -EINVAL;
    }
    at = MAX(at, now + min_delay_ns);
    return i2c_wire_stage_at(bus, master, at, now, sda, clock);
}

int i2c_wire_stage_after(I2CBus *bus, uint32_t master, int64_t delay_ns,
                         bool sda, bool clock)
{
    return i2c_wire_stage_at_or_after(bus, master, INT64_MIN, delay_ns,
                                      sda, clock);
}

static int i2c_wire_hold_at(I2CSlave *slave, int64_t at, int64_t now,
                            bool hold)
{
    I2CBus *bus = I2C_BUS(qdev_get_parent_bus(DEVICE(slave)));

    if (!bus->wire || at <= now) {
        return -EINVAL;
    }
    if (slave->wire_hold_pending) {
        return -EBUSY;
    }
    slave->wire_hold_pending = true;
    slave->wire_hold_at = at;
    slave->wire_hold_value = hold;
    i2c_wire_schedule(bus->wire);
    return 0;
}

int i2c_wire_hold(I2CSlave *slave, int64_t at, bool hold)
{
    return i2c_wire_hold_at(slave, at,
                            qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL), hold);
}

int i2c_wire_hold_after(I2CSlave *slave, int64_t delay_ns, bool hold)
{
    int64_t at;
    int64_t now = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);

    if (delay_ns <= 0 || sadd64_overflow(now, delay_ns, &at)) {
        return -EINVAL;
    }
    return i2c_wire_hold_at(slave, at, now, hold);
}

int i2c_wire_reset_master(I2CBus *bus, uint32_t master)
{
    I2CWireMaster *m;

    if (!bus->wire || master >= bus->master_count) {
        return -EINVAL;
    }
    m = &bus->wire->masters[master];
    if ((bus->wire->contenders & (1ULL << master)) || m->clock) {
        return -ENOTSUP;
    }
    m->pending = false;
    m->paused = false;
    m->sda = true;
    m->scl_low = false;
    m->deadline = -1;
    i2c_wire_schedule(bus->wire);
    return 0;
}

int i2c_wire_pause_master(I2CBus *bus, uint32_t master)
{
    I2CWireMaster *m;

    if (!bus->wire || master >= bus->master_count) {
        return -EINVAL;
    }
    m = &bus->wire->masters[master];
    /*
     * Only a master that is currently driving the low phase can pause: it
     * goes on holding SCL down while its clock generator stops, which is how
     * a controller waits for its host between bytes. Pausing on a high phase
     * would release a line the caller believes it still owns.
     */
    if (!m->clock || !m->scl_low || m->pending || bus->wire->scl) {
        return -EINVAL;
    }
    m->clock = false;
    m->paused = true;
    m->deadline = -1;
    i2c_wire_schedule(bus->wire);
    return 0;
}

int i2c_wire_set_observer(I2CBus *bus, uint32_t master,
                          I2CWireObserver observe, void *opaque)
{
    if (!bus->wire || master >= bus->master_count || !observe) {
        return -EINVAL;
    }
    if (bus->wire->master_observe[master]) {
        return -EBUSY;
    }
    bus->wire->master_observe[master] = observe;
    bus->wire->master_opaque[master] = opaque;
    return 0;
}

int i2c_wire_clear_observer(I2CBus *bus, uint32_t master,
                            I2CWireObserver observe, void *opaque)
{
    if (!bus->wire || master >= bus->master_count) {
        return -EINVAL;
    }
    if (bus->wire->master_observe[master] != observe ||
        bus->wire->master_opaque[master] != opaque) {
        return -ENOENT;
    }
    bus->wire->master_observe[master] = NULL;
    bus->wire->master_opaque[master] = NULL;
    return 0;
}

bool i2c_wire_scl(I2CBus *bus)
{
    return bus->wire && bus->wire->scl;
}

bool i2c_wire_sda(I2CBus *bus)
{
    return bus->wire && bus->wire->sda;
}

uint64_t i2c_wire_contenders(I2CBus *bus)
{
    return bus->wire ? bus->wire->contenders : 0;
}
