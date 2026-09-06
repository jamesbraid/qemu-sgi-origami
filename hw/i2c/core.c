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
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qemu/main-loop.h"
#include "trace.h"

static const Property i2c_props[] = {
    DEFINE_PROP_UINT8("address", struct I2CSlave, address, 0),
};

static const TypeInfo i2c_bus_info = {
    .name = TYPE_I2C_BUS,
    .parent = TYPE_BUS,
    .instance_size = sizeof(I2CBus),
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
        (bus->address_valid && bus->owner == UINT32_MAX) ||
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

    return bus->master_count && !bus->masters_loaded ? -EINVAL : 0;
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
        (!bus->address_valid || bus->owner == UINT32_MAX ||
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

static const TypeInfo i2c_slave_type_info = {
    .name = TYPE_I2C_SLAVE,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(I2CSlave),
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
    return bus->master_count && master < bus->master_count &&
           bus->owner == master;
}

int i2c_master_start_transfer(I2CBus *bus, uint32_t master,
                              uint8_t address, bool recv)
{
    I2CNodeList matches = QLIST_HEAD_INITIALIZER(matches);
    I2CNode *node, *next;
    bool broadcast = address == I2C_BROADCAST && !recv;

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
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

int i2c_master_send(I2CBus *bus, uint32_t master, uint8_t data)
{
    I2CNode *node;
    bool ack = false;

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    if (!bus->address_valid || bus->master_recv) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&bus->current_devs)) {
        return -ENXIO;
    }
    QLIST_FOREACH(node, &bus->current_devs, next) {
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

        if (sc->send) {
            trace_i2c_send(node->elt->address, data);
            /* Deliver once to every receiver; any ACK pulls the line low. */
            ack |= sc->send(node->elt, data) == 0;
        }
    }
    return ack ? 0 : 1;
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
