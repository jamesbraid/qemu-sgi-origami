/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * SGI I2C wire bus.
 *
 * A TYPE_I2C_BUS subclass carrying the resolved-master byte API and the timed
 * open-drain SDA/SCL transport that the SGI Origin module-management and
 * system-controller paths need: stable participant ids, bit-level arbitration,
 * clock stretching, repeated-START and the migration of all of that. Stock
 * hw/i2c/core.c keeps modelling a single anonymous master and immediate
 * transfers; everything here lives off that shared file.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/i2c-wire.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/host-utils.h"
#include "qemu/module.h"
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
    I2cWire *bus;
    QEMUTimer *timer;
    I2CWireObserver observe;
    void *opaque;
    I2CWireObserver master_observe[I2C_WIRE_MAX_MASTERS];
    void *master_opaque[I2C_WIRE_MAX_MASTERS];
    I2CWireDeadline master_next[I2C_WIRE_MAX_MASTERS];
    I2CWireRun master_run[I2C_WIRE_MAX_MASTERS];
    void *master_timeline_opaque[I2C_WIRE_MAX_MASTERS];
    I2CWireMaster masters[I2C_WIRE_MAX_MASTERS];
    uint64_t contenders;
    /*
     * The instant being settled while the timeline advances. Neither field
     * outlives one advance, so neither migrates.
     */
    int64_t now;
    bool advancing;
    /*
     * A line re-evaluation asked for while a tick is publishing runs once
     * that tick has told every observer, so none of them sees the later
     * event first. Neither field outlives the tick that set it.
     */
    bool ticking, retick;
    bool scl, sda, occupied, start_hold;
    bool address_byte, ack, read_valid, read_prepare_pending;
    uint8_t bits, byte;
    uint64_t phase, read_prepare_phase;
    int32_t error;
};

static void i2c_wire_schedule(I2CWire *wire);
static bool i2c_wire_slave_sda(const I2CWire *wire, bool scl);
static int i2c_wire_finish_load(I2cWire *bus);
static int i2c_wire_queue_service(I2CWire *wire, I2CSlave *slave,
                                  enum i2c_wire_service service);

static inline I2CBus *base_bus(I2cWire *bus)
{
    return &bus->parent_obj;
}

static void i2c_clear_nodes(I2CNodeList *nodes)
{
    I2CNode *node, *next;

    QLIST_FOREACH_SAFE(node, nodes, next, next) {
        QLIST_REMOVE(node, next);
        g_free(node);
    }
}

/* The runtime wire state for one child slave, created on first use. */
static I2CWireSlave *ws(I2cWire *bus, I2CSlave *slave)
{
    I2CWireSlave *st = g_hash_table_lookup(bus->slaves, slave);

    if (!st) {
        st = g_new0(I2CWireSlave, 1);
        /*
         * A slave created after the wire was enabled would otherwise start on
         * the first service kind rather than on "none", which the wire's own
         * invariants do not accept.
         */
        st->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
        g_hash_table_insert(bus->slaves, slave, st);
        /* Migration already refuses a bus with more children than this. */
        assert(bus->slave_count < I2C_WIRE_MAX_SLAVES);
        bus->slave_list[bus->slave_count++] = st;
    }
    return st;
}

I2CWireSlave *i2c_wire_slave_state(I2CSlave *slave)
{
    I2cWire *bus = I2C_WIRE(qdev_get_parent_bus(DEVICE(slave)));

    return ws(bus, slave);
}

static bool is_wire_endpoint(I2CSlave *slave)
{
    return object_dynamic_cast(OBJECT(slave), TYPE_I2C_WIRE_ENDPOINT) != NULL;
}

static I2CWireEndpointClass *wire_endpoint(I2CSlave *slave)
{
    if (!is_wire_endpoint(slave)) {
        return NULL;
    }
    return I2C_WIRE_ENDPOINT_GET_CLASS(slave);
}

static uint32_t i2c_wire_slave_count(I2cWire *bus)
{
    BusChild *kid;
    uint32_t count = 0;

    QTAILQ_FOREACH(kid, &base_bus(bus)->qbus.children, sibling) {
        count++;
    }
    return count;
}

static bool slave_selected(I2cWire *bus, I2CSlave *slave)
{
    I2CNode *node;

    QLIST_FOREACH(node, &base_bus(bus)->current_devs, next) {
        if (node->elt == slave) {
            return true;
        }
    }
    return false;
}

/* Bus lifecycle and class hooks. */

static void sgi_i2c_wire_bus_reset_hold(Object *obj, ResetType type)
{
    I2cWire *bus = I2C_WIRE(obj);
    I2CBus *base = base_bus(bus);
    I2CWire *wire = bus->wire;
    BusChild *kid;

    if (!wire) {
        return;
    }
    timer_del(wire->timer);
    i2c_clear_nodes(&base->current_devs);
    bus->owner = UINT32_MAX;
    bus->address_valid = false;
    bus->requested_address = 0;
    bus->master_recv = base->broadcast = false;
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
    QTAILQ_FOREACH(kid, &base->qbus.children, sibling) {
        I2CWireSlave *st = ws(bus, I2C_SLAVE(kid->child));

        st->wire_hold = st->wire_hold_pending = false;
        st->wire_hold_value = false;
        st->wire_hold_at = 0;
        st->wire_hold_phase = st->wire_service_phase = 0;
        st->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
        st->wire_service_pending = false;
    }
    bus->wire_loading = false;
}

/*
 * A timed wire finishes loading only when every slave on it has restored,
 * counted here from the per-child state the bus section carries. A slave that
 * carries no I2C slave migration state cannot restore its own device state, so
 * attaching it to a wired bus is refused rather than left to come up on the
 * destination with the wire's clock stopped and nothing to say why.
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

static bool sgi_i2c_wire_bus_check_address(BusState *bus, DeviceState *dev,
                                           Error **errp)
{
    if (I2C_WIRE(bus)->wire && !i2c_slave_state_migrated(dev)) {
        error_setg(errp, "%s carries no I2C slave migration state and cannot "
                   "join a timed I2C wire",
                   object_get_typename(OBJECT(dev)));
        return false;
    }
    return true;
}

static int sgi_wire_bus_busy(I2CBus *base)
{
    I2cWire *bus = I2C_WIRE(base);

    if (bus->wire) {
        return bus->wire->occupied;
    }
    if (bus->master_count) {
        return bus->owner != UINT32_MAX;
    }
    return !QLIST_EMPTY(&base->current_devs) || base->bh;
}

/*
 * Migration reconstruction. On a resolved or wire bus the bus section rebuilds
 * current_devs from its own per-child state, so the stock per-slave rebuild is
 * suppressed; an untimed subclass bus falls back to the stock address rebuild.
 */
/*
 * A START that matched no receiver, reported to every endpoint that observes
 * one. The untimed grant protocol reads the last such address back from a
 * controller that watches the bus.
 */
static void sgi_wire_unmatched_address(I2CBus *base, uint8_t address, bool recv)
{
    BusChild *kid;

    QTAILQ_FOREACH(kid, &base->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);
        I2CWireEndpointClass *ec = wire_endpoint(slave);

        if (ec && ec->unmatched_address) {
            ec->unmatched_address(slave, address, recv);
        }
    }
}

static int sgi_wire_slave_loaded(I2CBus *base, I2CSlave *slave)
{
    I2cWire *bus = I2C_WIRE(base);
    I2CNode *node;

    if (bus->master_count) {
        return 0;
    }
    if ((base->saved_address == slave->address) ||
        (base->saved_address == I2C_BROADCAST)) {
        node = g_new(struct I2CNode, 1);
        node->elt = slave;
        QLIST_INSERT_HEAD(&base->current_devs, node, next);
    }
    return 0;
}

/* Resolved-master byte API. */

int i2c_bus_enable_master_ids(I2CBus *base, uint32_t count)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!count) {
        return -EINVAL;
    }
    if (bus->master_count || sgi_wire_bus_busy(base) ||
        !QSIMPLEQ_EMPTY(&base->pending_masters)) {
        return -EBUSY;
    }
    bus->master_count = count;
    bus->owner = UINT32_MAX;
    return 0;
}

int i2c_bus_start_resolved(I2CBus *base, uint32_t master)
{
    I2cWire *bus = I2C_WIRE(base);

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

static bool i2c_master_owns(I2cWire *bus, uint32_t master)
{
    return !bus->wire && bus->master_count && master < bus->master_count &&
           bus->owner == master;
}

static int i2c_resolved_start_transfer(I2cWire *bus, uint8_t address,
                                       bool recv)
{
    I2CBus *base = base_bus(bus);
    I2CNodeList matches = QLIST_HEAD_INITIALIZER(matches);
    I2CNode *node, *next;
    bool broadcast = address == I2C_BROADCAST && !recv;

    if (address > 0x7f) {
        return -EINVAL;
    }
    /* Address byte 0x01 is the START byte; no target acknowledges it. */
    if (address != I2C_BROADCAST || !recv) {
        i2c_scan_bus(base, address, broadcast, &matches);
    }
    QLIST_FOREACH(node, &matches, next) {
        if (!is_wire_endpoint(node->elt)) {
            i2c_clear_nodes(&matches);
            return -ENOTSUP;
        }
    }
    if (bus->address_valid) {
        QLIST_FOREACH(node, &base->current_devs, next) {
            I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

            if (sc->event) {
                sc->event(node->elt, I2C_RESTART);
            }
        }
    }
    i2c_clear_nodes(&base->current_devs);
    QLIST_SWAP(&base->current_devs, &matches, next);
    bus->requested_address = address;
    bus->master_recv = recv;
    bus->address_valid = true;
    base->broadcast = broadcast;
    QLIST_FOREACH_SAFE(node, &base->current_devs, next, next) {
        I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

        if (sc->event && sc->event(node->elt, recv ? I2C_START_RECV :
                                                   I2C_START_SEND)) {
            /* Address NAK removes only this receiver, without STOP. */
            QLIST_REMOVE(node, next);
            g_free(node);
        }
    }
    return QLIST_EMPTY(&base->current_devs) ? 1 : 0;
}

int i2c_master_start_transfer(I2CBus *base, uint32_t master,
                              uint8_t address, bool recv)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    return i2c_resolved_start_transfer(bus, address, recv);
}

static int i2c_resolved_send(I2cWire *bus, uint8_t data, I2CWire *wire)
{
    I2CBus *base = base_bus(bus);
    I2CNode *node;
    bool ack = false;

    if (!bus->address_valid || bus->master_recv) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&base->current_devs)) {
        return -ENXIO;
    }
    QLIST_FOREACH(node, &base->current_devs, next) {
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

int i2c_master_send(I2CBus *base, uint32_t master, uint8_t data)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    return i2c_resolved_send(bus, data, NULL);
}

int i2c_master_recv(I2CBus *base, uint32_t master, uint8_t *data)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    if (!bus->address_valid || !bus->master_recv || !data || base->broadcast) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&base->current_devs)) {
        return -ENXIO;
    }
    *data = i2c_recv(base);
    return 0;
}

int i2c_master_nack(I2CBus *base, uint32_t master)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    if (!bus->address_valid || !bus->master_recv) {
        return -EINVAL;
    }
    if (QLIST_EMPTY(&base->current_devs)) {
        return -ENXIO;
    }
    i2c_nack(base);
    return 0;
}

int i2c_master_end_transfer(I2CBus *base, uint32_t master)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!i2c_master_owns(bus, master)) {
        return -EPERM;
    }
    i2c_end_transfer(base);
    bus->owner = UINT32_MAX;
    bus->address_valid = false;
    return 0;
}

/* Timed open-drain transport. */

static int i2c_wire_queue_service(I2CWire *wire, I2CSlave *slave,
                                  enum i2c_wire_service service)
{
    I2CWireSlave *st = ws(wire->bus, slave);

    if (st->wire_service_pending || service >= I2C_WIRE_SERVICE_COUNT) {
        wire->error = -EINVAL;
        return wire->error;
    }
    st->wire_service_pending = true;
    st->wire_service_phase = wire->phase;
    st->wire_service_kind = service;
    return 0;
}

static int i2c_wire_queue_selected(I2CWire *wire,
                                   enum i2c_wire_service service,
                                   bool first_only)
{
    I2CNode *node;

    QLIST_FOREACH(node, &base_bus(wire->bus)->current_devs, next) {
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

    QLIST_FOREACH(node, &base_bus(wire->bus)->current_devs, next) {
        I2CSlave *slave = node->elt;
        I2CWireSlave *st = ws(wire->bus, slave);
        I2CWireEndpointClass *ec = wire_endpoint(slave);
        enum i2c_wire_service service = st->wire_service_kind;
        bool hold = false;

        if (!st->wire_service_pending ||
            st->wire_service_phase != wire->phase) {
            continue;
        }
        if (service == I2C_WIRE_READ_ACK) {
            service = read_acked ? I2C_WIRE_READ_ACK : I2C_WIRE_READ_NACK;
        }
        st->wire_service_pending = false;
        st->wire_service_phase = 0;
        st->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
        if (ec && ec->wire_service) {
            hold = ec->wire_service(slave, service);
        }
        if (hold) {
            if (st->wire_hold || st->wire_hold_pending) {
                wire->error = -ENOTSUP;
                continue;
            }
            st->wire_hold = true;
            st->wire_hold_phase = wire->phase;
        }
    }
}

static bool i2c_wire_read_prepare_held(I2CWire *wire)
{
    I2CNode *node;

    QLIST_FOREACH(node, &base_bus(wire->bus)->current_devs, next) {
        I2CWireSlave *st = ws(wire->bus, node->elt);

        if (st->wire_hold &&
            st->wire_hold_phase == wire->read_prepare_phase) {
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
    wire->byte = i2c_recv(base_bus(wire->bus));
    wire->read_valid = true;
    wire->read_prepare_pending = false;
    wire->read_prepare_phase = 0;
}

/* The wire's own earliest deadline, if it has one. */
static bool i2c_wire_next_deadline(I2CWire *wire, int64_t *deadline)
{
    int64_t next = 0;
    bool found = false;

    for (unsigned int n = 0; n < wire->bus->slave_count; n++) {
        I2CWireSlave *st = wire->bus->slave_list[n];

        if (st->wire_hold_pending) {
            if (!found || st->wire_hold_at < next) {
                next = st->wire_hold_at;
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
    *deadline = next;
    return found;
}

static void i2c_wire_schedule(I2CWire *wire)
{
    int64_t next;

    /* An advance re-arms the timer once, when it has finished. */
    if (wire->advancing) {
        return;
    }
    if (wire->error || wire->bus->wire_loading ||
        !i2c_wire_next_deadline(wire, &next)) {
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
static int i2c_wire_join_low_deadline(I2CWireMaster *joining,
                                      int64_t low_started,
                                      int64_t *deadline)
{
    if (sadd64_overflow(low_started, joining->low_ns, deadline)) {
        return -ERANGE;
    }
    return 0;
}

/* Settle everything the wire has due at @now, which is that deadline. */
static void i2c_wire_tick(I2CWire *wire, int64_t now)
{
    I2cWire *bus = wire->bus;
    I2CBus *base = base_bus(bus);
    bool old_scl = wire->scl, old_sda = wire->sda;
    uint64_t old_contenders = wire->contenders;
    bool scl = true, sda;
    uint64_t lost = 0;
    uint64_t restart_attempts = 0, stop_attempts = 0;
    int old_error = wire->error;
    int64_t low_started = 0;
    bool running_low = false;

    wire->ticking = true;
    for (unsigned i = 0; i < bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if ((old_contenders & (1ULL << i)) && m->clock && m->scl_low &&
            m->deadline >= 0) {
            low_started = m->deadline - m->low_ns;
            running_low = true;
            break;
        }
    }

    for (unsigned int n = 0; n < bus->slave_count; n++) {
        I2CWireSlave *st = bus->slave_list[n];

        if (st->wire_hold_pending && st->wire_hold_at <= now) {
            st->wire_hold_pending = false;
            st->wire_hold = st->wire_hold_value;
            if (!st->wire_hold) {
                st->wire_hold_phase = 0;
            }
        }
        scl &= !st->wire_hold;
    }
    i2c_wire_prepare_read(wire);
    sda = i2c_wire_slave_sda(wire, wire->scl);
    /* Apply the complete deadline batch before resolving either line. */
    for (unsigned i = 0; i < bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if (m->pending && m->staged_at <= now) {
            bool join_low = !m->clock && m->staged_clock && !old_scl &&
                            !m->paused;
            int64_t join_deadline = -1;

            m->pending = false;
            if (join_low) {
                int ret;

                if (!(old_contenders & (1ULL << i)) || !running_low) {
                    wire->error = -ENOTSUP;
                    continue;
                }
                ret = i2c_wire_join_low_deadline(m, low_started,
                                                 &join_deadline);
                if (ret) {
                    wire->error = ret;
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

            QLIST_FOREACH(node, &base->current_devs, next) {
                I2CSlaveClass *sc = I2C_SLAVE_GET_CLASS(node->elt);

                if (sc->event) {
                    sc->event(node->elt, I2C_RESTART);
                }
            }
            i2c_clear_nodes(&base->current_devs);
            bus->address_valid = false;
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
        i2c_end_transfer(base);
        bus->address_valid = false;
    }
    if (old_scl && !scl) {
        wire->start_hold = false;
        if (!i2c_wire_set_deadlines(wire, now, true)) {
            goto publish;
        }
        for (unsigned i = 0; i < bus->master_count; i++) {
            if (wire->masters[i].clock) {
                wire->masters[i].scl_low = true;
            }
        }
    } else if (!old_scl && scl) {
        if (!i2c_wire_set_deadlines(wire, now, false)) {
            goto publish;
        }
        for (unsigned i = 0; i < bus->master_count; i++) {
            I2CWireMaster *m = &wire->masters[i];
            uint64_t bit = 1ULL << i;

            if (((wire->bits < 8 &&
                  !(bus->address_valid && bus->master_recv &&
                    !wire->address_byte)) ||
                 (wire->bits == 8 && bus->address_valid &&
                  bus->master_recv && !wire->address_byte)) &&
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
            (wire->address_byte || !bus->master_recv)) {
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
                ret = i2c_resolved_start_transfer(bus, wire->byte >> 1,
                                                   wire->byte & 1);
                if (!ret) {
                    ret = i2c_wire_queue_selected(
                        wire, bus->master_recv ?
                        I2C_WIRE_ADDRESS_RECV : I2C_WIRE_ADDRESS_SEND,
                        false);
                }
            } else if (bus->master_recv) {
                ret = 1;
                wire->read_valid = false;
                if (!QLIST_EMPTY(&base->current_devs)) {
                    ret = i2c_wire_queue_selected(
                        wire, I2C_WIRE_READ_ACK, true);
                }
            } else if (QLIST_EMPTY(&base->current_devs)) {
                ret = 1;
            } else {
                ret = i2c_resolved_send(bus, wire->byte, wire);
            }
            wire->error = MIN(ret, 0);
            wire->ack = !bus->master_recv && ret == 0;
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

                QTAILQ_FOREACH(child, &base->qbus.children, sibling) {
                    I2CSlave *slave = I2C_SLAVE(child->child);
                    I2CWireEndpointClass *ec = wire_endpoint(slave);

                    if (ec && ec->wire_address) {
                        ec->wire_address(slave, address_byte);
                    }
                }
            }
            wire->bits = wire->byte = 0;
            wire->address_byte = wire->ack = false;
            if (bus->master_recv &&
                !QLIST_EMPTY(&base->current_devs)) {
                if (was_address && target_acked) {
                    wire->read_prepare_pending = true;
                    wire->read_prepare_phase = wire->phase;
                } else if (!was_address && master_acked) {
                    wire->read_prepare_pending = true;
                    wire->read_prepare_phase = wire->phase;
                } else if (!was_address) {
                    i2c_nack(base);
                }
            }
            i2c_wire_complete_services(wire, master_acked);
            if (wire->error) {
                goto publish;
            }
            i2c_wire_prepare_read(wire);
            for (unsigned i = 0; i < bus->master_count; i++) {
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
    for (unsigned i = 0; i < bus->master_count; i++) {
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
        for (unsigned i = 0; i < bus->master_count; i++) {
            if (wire->master_observe[i]) {
                wire->master_observe[i](wire->master_opaque[i], &event);
            }
        }
    }
    wire->ticking = false;
    if (wire->retick) {
        wire->retick = false;
        i2c_wire_tick(wire, now);
        return;
    }
    i2c_wire_schedule(wire);
}

/* The instant being settled, or the present outside an advance. */
static int64_t i2c_wire_instant(I2CWire *wire)
{
    return wire->advancing ? wire->now : qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

/*
 * Bring the wire and every participant timed against it up to @until, one
 * deadline at a time in time order, running each at its own instant. A host
 * that gets round to this late has not moved any deadline, so the waveform is
 * the one the participants asked for, not one stretched by the delay.
 *
 * A participant due at the same instant as the wire runs first. That is the
 * order separate timers gave them in practice: the wire re-arms its timer
 * microseconds before each of its deadlines, a participant's timer for the
 * same instant was nearly always armed earlier, and QEMU runs equal deadlines
 * in the order they were armed.
 */
static void i2c_wire_advance_to(I2CWire *wire, int64_t until)
{
    bool ran = false;

    if (wire->advancing || wire->bus->wire_loading) {
        return;
    }
    wire->advancing = true;
    for (;;) {
        int64_t wire_next = 0, next = until;
        bool wire_due = !wire->error &&
                        i2c_wire_next_deadline(wire, &wire_next) &&
                        wire_next <= until;
        uint32_t participant = UINT32_MAX;

        for (uint32_t i = 0; i < wire->bus->master_count; i++) {
            int64_t at;

            if (!wire->master_run[i]) {
                continue;
            }
            at = wire->master_next[i](wire->master_timeline_opaque[i]);
            if (at < next || (at == next && participant == UINT32_MAX)) {
                next = at;
                participant = i;
            }
        }
        if (participant != UINT32_MAX && (!wire_due || next <= wire_next)) {
            wire->now = next;
            wire->master_run[participant](
                wire->master_timeline_opaque[participant], next);
            /* A participant that leaves its own instant due never finishes. */
            assert(!wire->master_run[participant] ||
                   wire->master_next[participant](
                       wire->master_timeline_opaque[participant]) > next);
        } else if (wire_due) {
            wire->now = wire_next;
            i2c_wire_tick(wire, wire_next);
        } else {
            break;
        }
        ran = true;
    }
    wire->advancing = false;
    if (ran) {
        i2c_wire_schedule(wire);
    }
}

/* The wire's timer only stands in for a guest that is not polling. */
static void i2c_wire_timer_expired(void *opaque)
{
    i2c_wire_advance_to(opaque, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
}

void i2c_wire_advance(I2CBus *base)
{
    I2cWire *bus = I2C_WIRE(base);

    if (bus->wire) {
        i2c_wire_advance_to(bus->wire, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }
}

int64_t i2c_wire_now(I2CBus *base)
{
    I2cWire *bus = I2C_WIRE(base);

    return bus->wire ? i2c_wire_instant(bus->wire) :
                       qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

int i2c_bus_enable_wire(I2CBus *base, uint32_t count,
                         I2CWireObserver observe, void *opaque, Error **errp)
{
    I2cWire *bus = I2C_WIRE(base);
    BusChild *kid;
    int ret;

    if (!count || count > I2C_WIRE_MAX_MASTERS) {
        error_setg(errp, "a timed I2C wire takes 1 to %u participants",
                   I2C_WIRE_MAX_MASTERS);
        return -EINVAL;
    }
    if (bus->wire || sgi_wire_bus_busy(base)) {
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
    QTAILQ_FOREACH(kid, &base->qbus.children, sibling) {
        if (!i2c_slave_state_migrated(kid->child)) {
            error_setg(errp, "%s is already on this bus and carries no I2C "
                       "slave migration state",
                       object_get_typename(OBJECT(kid->child)));
            return -EINVAL;
        }
    }
    if (!bus->master_count) {
        ret = i2c_bus_enable_master_ids(base, count);
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
    QTAILQ_FOREACH(kid, &base->qbus.children, sibling) {
        ws(bus, I2C_SLAVE(kid->child))->wire_service_kind =
            I2C_WIRE_SERVICE_COUNT;
    }
    bus->wire->timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                      i2c_wire_timer_expired, bus->wire);
    return 0;
}

int i2c_wire_configure(I2CBus *base, uint32_t master, int64_t low_ns,
                       int64_t high_ns, bool clock_after_loss)
{
    I2cWire *bus = I2C_WIRE(base);
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

static int i2c_wire_stage_check(I2cWire *bus, uint32_t master)
{
    if (!bus->wire || master >= bus->master_count) {
        return -EINVAL;
    }
    return bus->wire->error;
}

static int i2c_wire_stage_at(I2cWire *bus, uint32_t master, int64_t at,
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

int i2c_wire_stage(I2CBus *base, uint32_t master, int64_t at,
                   bool sda, bool clock)
{
    I2cWire *bus = I2C_WIRE(base);
    int ret = i2c_wire_stage_check(bus, master);

    if (ret) {
        return ret;
    }
    return i2c_wire_stage_at(bus, master, at, i2c_wire_instant(bus->wire),
                             sda, clock);
}

int i2c_wire_stage_at_or_after(I2CBus *base, uint32_t master, int64_t at,
                               int64_t min_delay_ns, bool sda, bool clock)
{
    I2cWire *bus = I2C_WIRE(base);
    int64_t now;
    int ret = i2c_wire_stage_check(bus, master);

    if (ret) {
        return ret;
    }
    if (min_delay_ns <= 0) {
        return -EINVAL;
    }
    now = i2c_wire_instant(bus->wire);
    if (now > INT64_MAX - min_delay_ns) {
        return -EINVAL;
    }
    at = MAX(at, now + min_delay_ns);
    return i2c_wire_stage_at(bus, master, at, now, sda, clock);
}

int i2c_wire_stage_after(I2CBus *base, uint32_t master, int64_t delay_ns,
                         bool sda, bool clock)
{
    return i2c_wire_stage_at_or_after(base, master, INT64_MIN, delay_ns,
                                      sda, clock);
}

static int i2c_wire_hold_at(I2CSlave *slave, int64_t at, int64_t now,
                            bool hold)
{
    I2cWire *bus = I2C_WIRE(qdev_get_parent_bus(DEVICE(slave)));
    I2CWireSlave *st;

    if (!bus->wire || at <= now) {
        return -EINVAL;
    }
    st = ws(bus, slave);
    if (st->wire_hold_pending) {
        return -EBUSY;
    }
    st->wire_hold_pending = true;
    st->wire_hold_at = at;
    st->wire_hold_value = hold;
    i2c_wire_schedule(bus->wire);
    return 0;
}

int i2c_wire_hold(I2CSlave *slave, int64_t at, bool hold)
{
    I2cWire *bus = I2C_WIRE(qdev_get_parent_bus(DEVICE(slave)));

    if (!bus->wire) {
        return -EINVAL;
    }
    return i2c_wire_hold_at(slave, at, i2c_wire_instant(bus->wire), hold);
}

int i2c_wire_hold_after(I2CSlave *slave, int64_t delay_ns, bool hold)
{
    I2cWire *bus = I2C_WIRE(qdev_get_parent_bus(DEVICE(slave)));
    int64_t at;
    int64_t now;

    if (!bus->wire) {
        return -EINVAL;
    }
    now = i2c_wire_instant(bus->wire);
    if (delay_ns <= 0 || sadd64_overflow(now, delay_ns, &at)) {
        return -EINVAL;
    }
    return i2c_wire_hold_at(slave, at, now, hold);
}

int i2c_wire_reset_master(I2CBus *base, uint32_t master)
{
    I2cWire *bus = I2C_WIRE(base);
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

int i2c_wire_release_slave(I2CSlave *slave)
{
    BusState *parent = qdev_get_parent_bus(DEVICE(slave));
    I2cWire *bus = parent ?
        (I2cWire *)object_dynamic_cast(OBJECT(parent), TYPE_I2C_WIRE) : NULL;
    I2CBus *base;
    I2CWire *wire;
    I2CWireSlave *st;
    I2CNode *node, *next;

    if (!bus || !bus->wire) {
        return -EINVAL;
    }
    wire = bus->wire;
    base = base_bus(bus);
    if (wire->error) {
        return wire->error;
    }
    /* Everything already due happens before the slave lets go. */
    if (!wire->advancing && !wire->ticking) {
        i2c_wire_advance_to(wire, qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL));
    }

    st = ws(bus, slave);
    st->wire_hold = st->wire_hold_pending = false;
    st->wire_hold_phase = 0;
    st->wire_service_pending = false;
    st->wire_service_phase = 0;
    st->wire_service_kind = I2C_WIRE_SERVICE_COUNT;
    QLIST_FOREACH_SAFE(node, &base->current_devs, next, next) {
        if (node->elt == slave) {
            QLIST_REMOVE(node, next);
            g_free(node);
        }
    }
    if (QLIST_EMPTY(&base->current_devs)) {
        /* Nothing selected is left to acknowledge or to send a bit. */
        wire->ack = false;
        wire->read_valid = false;
        wire->read_prepare_pending = false;
        wire->read_prepare_phase = 0;
    }

    /* The lines follow at once; a STOP they now form is a STOP. */
    if (wire->ticking) {
        wire->retick = true;
    } else {
        i2c_wire_tick(wire, i2c_wire_instant(wire));
    }
    return 0;
}

int i2c_wire_pause_master(I2CBus *base, uint32_t master)
{
    I2cWire *bus = I2C_WIRE(base);
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

int i2c_wire_set_observer(I2CBus *base, uint32_t master,
                          I2CWireObserver observe, void *opaque)
{
    I2cWire *bus = I2C_WIRE(base);

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

int i2c_wire_clear_observer(I2CBus *base, uint32_t master,
                            I2CWireObserver observe, void *opaque)
{
    I2cWire *bus = I2C_WIRE(base);

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

int i2c_wire_set_timeline(I2CBus *base, uint32_t master, I2CWireDeadline next,
                          I2CWireRun run, void *opaque)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!bus->wire || master >= bus->master_count || !next || !run) {
        return -EINVAL;
    }
    if (bus->wire->master_run[master]) {
        return -EBUSY;
    }
    bus->wire->master_next[master] = next;
    bus->wire->master_run[master] = run;
    bus->wire->master_timeline_opaque[master] = opaque;
    return 0;
}

int i2c_wire_clear_timeline(I2CBus *base, uint32_t master, I2CWireRun run,
                            void *opaque)
{
    I2cWire *bus = I2C_WIRE(base);

    if (!bus->wire || master >= bus->master_count) {
        return -EINVAL;
    }
    if (bus->wire->master_run[master] != run ||
        bus->wire->master_timeline_opaque[master] != opaque) {
        return -ENOENT;
    }
    bus->wire->master_next[master] = NULL;
    bus->wire->master_run[master] = NULL;
    bus->wire->master_timeline_opaque[master] = NULL;
    return 0;
}

bool i2c_wire_scl(I2CBus *base)
{
    I2cWire *bus = I2C_WIRE(base);

    return bus->wire && bus->wire->scl;
}

bool i2c_wire_sda(I2CBus *base)
{
    I2cWire *bus = I2C_WIRE(base);

    return bus->wire && bus->wire->sda;
}

uint64_t i2c_wire_contenders(I2CBus *base)
{
    I2cWire *bus = I2C_WIRE(base);

    return bus->wire ? bus->wire->contenders : 0;
}

uint32_t i2c_bus_master_count(I2CBus *base)
{
    I2cWire *bus =
        (I2cWire *)object_dynamic_cast(OBJECT(base),
                                             TYPE_I2C_WIRE);

    return bus ? bus->master_count : 0;
}

uint32_t i2c_bus_resolved_owner(I2CBus *base)
{
    I2cWire *bus =
        (I2cWire *)object_dynamic_cast(OBJECT(base),
                                             TYPE_I2C_WIRE);

    return bus ? bus->owner : UINT32_MAX;
}

/* Migration. */

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
    I2cWire *bus = wire->bus;
    uint64_t members = (1ULL << bus->master_count) - 1;

    if (wire->bits > 9 || wire->error > 0 ||
        (wire->contenders & ~members) ||
        (!wire->occupied && (wire->contenders || bus->address_valid ||
                             wire->bits || wire->byte)) ||
        (wire->start_hold && (!wire->occupied || !wire->scl)) ||
        (wire->ack && (!wire->occupied || wire->bits < 8)) ||
        (wire->ack && bus->master_recv && !wire->address_byte) ||
        (wire->address_byte && !wire->occupied) ||
        (wire->bits < 8 &&
         (wire->address_byte ||
          (bus->address_valid && !bus->master_recv)) &&
         wire->byte >= (1U << wire->bits)) ||
        (wire->read_valid &&
         (!wire->occupied || !bus->address_valid ||
          !bus->master_recv || wire->address_byte || wire->bits > 8)) ||
        (wire->read_prepare_pending &&
         (!wire->occupied || !bus->address_valid ||
          !bus->master_recv || wire->address_byte || wire->read_valid ||
          wire->bits || !wire->read_prepare_phase ||
          wire->read_prepare_phase != wire->phase)) ||
        (!wire->read_prepare_pending && wire->read_prepare_phase)) {
        return -EINVAL;
    }
    for (unsigned i = 0; i < bus->master_count; i++) {
        I2CWireMaster *m = &wire->masters[i];

        if ((m->configured && (m->low_ns <= 0 || m->high_ns <= 0)) ||
            (!m->configured && (m->clock || m->pending || m->paused)) ||
            (!m->clock && m->scl_low && !m->paused) ||
            (m->paused && (m->clock || !m->scl_low)) ||
            m->deadline < -1) {
            return -EINVAL;
        }
    }
    bus->wire_loaded = true;
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
    return ((I2cWire *)opaque)->wire != NULL;
}

static const VMStateDescription vmstate_sgi_wire = {
    /* Pre-rename stream name; kept for migration compatibility. */
    .name = "sgi-i2c-wire-bus/wire",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_wire_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_POINTER(wire, I2cWire, vmstate_i2c_wire_state,
                               I2CWire),
        VMSTATE_END_OF_LIST()
    },
};

static bool i2c_masters_needed(void *opaque)
{
    return ((I2cWire *)opaque)->master_count != 0;
}

static int i2c_masters_post_load(void *opaque, int version_id)
{
    I2cWire *bus = opaque;

    bus->masters_loaded = true;
    base_bus(bus)->broadcast = bus->address_valid && !bus->master_recv &&
                     bus->requested_address == I2C_BROADCAST;
    if ((bus->owner != UINT32_MAX && bus->owner >= bus->master_count) ||
        (bus->address_valid && bus->owner == UINT32_MAX && !bus->wire) ||
        bus->requested_address > 0x7f) {
        return -EINVAL;
    }
    return 0;
}

static const VMStateDescription vmstate_sgi_masters = {
    /* Pre-rename stream name; kept for migration compatibility. */
    .name = "sgi-i2c-wire-bus/masters",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_masters_needed,
    .post_load = i2c_masters_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(master_count, I2cWire),
        VMSTATE_UINT32(owner, I2cWire),
        VMSTATE_UINT8(requested_address, I2cWire),
        VMSTATE_BOOL(master_recv, I2cWire),
        VMSTATE_BOOL(address_valid, I2cWire),
        VMSTATE_END_OF_LIST()
    },
};

static bool i2c_wire_service_state_valid(I2cWire *bus, I2CSlave *slave)
{
    I2CWire *wire = bus->wire;
    I2CWireSlave *st = ws(bus, slave);
    enum i2c_wire_service service = st->wire_service_kind;

    if ((!st->wire_hold && st->wire_hold_phase) ||
        (st->wire_hold_phase && st->wire_hold_phase != wire->phase) ||
        (st->wire_hold_phase &&
         (!slave_selected(bus, slave) || wire->bits)) ||
        (st->wire_hold_pending &&
         st->wire_hold_at < qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL))) {
        return false;
    }
    if (!st->wire_service_pending) {
        return !st->wire_service_phase &&
               service == I2C_WIRE_SERVICE_COUNT;
    }
    if (!slave_selected(bus, slave) || st->wire_hold ||
        !st->wire_service_phase ||
        st->wire_service_phase != wire->phase ||
        wire->bits < 8 || service >= I2C_WIRE_SERVICE_COUNT) {
        return false;
    }
    switch (service) {
    case I2C_WIRE_ADDRESS_SEND:
        return wire->address_byte && bus->address_valid &&
               !bus->master_recv;
    case I2C_WIRE_ADDRESS_RECV:
        return wire->address_byte && bus->address_valid &&
               bus->master_recv;
    case I2C_WIRE_WRITE:
        return !wire->address_byte && bus->address_valid &&
               !bus->master_recv;
    case I2C_WIRE_READ_ACK:
        return !wire->address_byte && bus->address_valid &&
               bus->master_recv;
    case I2C_WIRE_READ_NACK:
    case I2C_WIRE_SERVICE_COUNT:
        return false;
    }
    return false;
}

static int i2c_wire_finish_load(I2cWire *bus)
{
    I2CBus *base = base_bus(bus);
    I2CWire *wire = bus->wire;
    BusChild *kid;
    bool scl = true;
    bool sda;
    bool read_prepare_held = false;

    if (!wire) {
        /* A resolved-only bus has no timed lines to reconstruct. */
        return 0;
    }
    if (!bus->wire_loaded) {
        return -EINVAL;
    }
    QTAILQ_FOREACH(kid, &base->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);
        I2CWireSlave *st = ws(bus, slave);

        if (!i2c_wire_service_state_valid(bus, slave)) {
            return -EINVAL;
        }
        if (wire->read_prepare_pending && slave_selected(bus, slave) &&
            st->wire_hold &&
            st->wire_hold_phase == wire->read_prepare_phase) {
            read_prepare_held = true;
        }
        scl &= !st->wire_hold;
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

static bool i2c_slaves_needed(void *opaque)
{
    return ((I2cWire *)opaque)->master_count != 0;
}

static int i2c_slaves_pre_save(void *opaque)
{
    I2cWire *bus = opaque;
    BusChild *kid;
    uint32_t n = 0;

    /* The whole array serialises; zero the tail past the live children. */
    memset(bus->migrated, 0, sizeof(bus->migrated));
    QTAILQ_FOREACH(kid, &base_bus(bus)->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);
        I2CWireSlave st;

        if (n >= I2C_WIRE_MAX_SLAVES) {
            return -EINVAL;
        }
        st = *ws(bus, slave);
        st.selected = slave_selected(bus, slave);
        bus->migrated[n++] = st;
    }
    bus->migrated_count = n;
    return 0;
}

static int i2c_slaves_post_load(void *opaque, int version_id)
{
    I2cWire *bus = opaque;
    I2CBus *base = base_bus(bus);
    BusChild *kid;
    uint32_t n = 0;

    if (bus->migrated_count != i2c_wire_slave_count(bus)) {
        return -EINVAL;
    }
    QTAILQ_FOREACH(kid, &base->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);
        I2CWireSlave m = bus->migrated[n++];

        if (m.selected &&
            (!bus->address_valid ||
             (bus->owner == UINT32_MAX && !bus->wire) ||
             !is_wire_endpoint(slave))) {
            return -EINVAL;
        }
        *ws(bus, slave) = m;
        if (m.selected) {
            I2CNode *node = g_new(struct I2CNode, 1);

            node->elt = slave;
            QLIST_INSERT_HEAD(&base->current_devs, node, next);
        }
    }
    return i2c_wire_finish_load(bus);
}

static const VMStateDescription vmstate_i2c_wire_slave = {
    /* Pre-rename stream name; kept for migration compatibility. */
    .name = "sgi-i2c-wire-slave",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BOOL(selected, I2CWireSlave),
        VMSTATE_BOOL(wire_hold, I2CWireSlave),
        VMSTATE_BOOL(wire_hold_pending, I2CWireSlave),
        VMSTATE_BOOL(wire_hold_value, I2CWireSlave),
        VMSTATE_INT64(wire_hold_at, I2CWireSlave),
        VMSTATE_UINT64(wire_hold_phase, I2CWireSlave),
        VMSTATE_UINT64(wire_service_phase, I2CWireSlave),
        VMSTATE_UINT8(wire_service_kind, I2CWireSlave),
        VMSTATE_BOOL(wire_service_pending, I2CWireSlave),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_sgi_slaves = {
    /* Pre-rename stream name; kept for migration compatibility. */
    .name = "sgi-i2c-wire-bus/slaves",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = i2c_slaves_needed,
    .pre_save = i2c_slaves_pre_save,
    .post_load = i2c_slaves_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(migrated_count, I2cWire),
        VMSTATE_STRUCT_ARRAY(migrated, I2cWire, I2C_WIRE_MAX_SLAVES, 0,
                             vmstate_i2c_wire_slave, I2CWireSlave),
        VMSTATE_END_OF_LIST()
    },
};

static int sgi_wire_bus_pre_load(void *opaque)
{
    I2cWire *bus = opaque;

    bus->masters_loaded = false;
    bus->wire_loaded = false;
    bus->wire_loading = bus->wire != NULL;
    if (bus->wire) {
        timer_del(bus->wire->timer);
    }
    if (bus->master_count) {
        /* Loading a replacement state is not a physical STOP. */
        i2c_clear_nodes(&base_bus(bus)->current_devs);
        bus->owner = UINT32_MAX;
    }
    return 0;
}

static int sgi_wire_bus_post_load(void *opaque, int version_id)
{
    I2cWire *bus = opaque;

    if ((bus->master_count && !bus->masters_loaded) ||
        (bus->wire && !bus->wire_loaded)) {
        return -EINVAL;
    }
    return 0;
}

const VMStateDescription vmstate_sgi_i2c_wire_bus = {
    /* Pre-rename stream name; kept for migration compatibility. */
    .name = "sgi-i2c-wire-bus",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_load = sgi_wire_bus_pre_load,
    .post_load = sgi_wire_bus_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_sgi_masters,
        &vmstate_sgi_wire,
        &vmstate_sgi_slaves,
        NULL
    },
};

/* QOM registration. */

static void sgi_i2c_wire_bus_init(Object *obj)
{
    I2cWire *bus = I2C_WIRE(obj);

    bus->owner = UINT32_MAX;
    bus->slaves = g_hash_table_new_full(NULL, NULL, NULL, g_free);
    vmstate_register_any(NULL, &vmstate_sgi_i2c_wire_bus, bus);
}

static void sgi_i2c_wire_bus_finalize(Object *obj)
{
    I2cWire *bus = I2C_WIRE(obj);

    vmstate_unregister(NULL, &vmstate_sgi_i2c_wire_bus, bus);
    if (bus->wire) {
        timer_free(bus->wire->timer);
        g_free(bus->wire);
    }
    if (bus->slaves) {
        g_hash_table_destroy(bus->slaves);
    }
}

static void sgi_i2c_wire_bus_class_init(ObjectClass *oc, const void *data)
{
    I2CBusClass *bc = I2C_BUS_CLASS(oc);

    RESETTABLE_CLASS(oc)->phases.hold = sgi_i2c_wire_bus_reset_hold;
    BUS_CLASS(oc)->check_address = sgi_i2c_wire_bus_check_address;
    bc->slave_loaded = sgi_wire_slave_loaded;
    bc->bus_busy = sgi_wire_bus_busy;
    bc->unmatched_address = sgi_wire_unmatched_address;
}

static const TypeInfo sgi_i2c_wire_types[] = {
    {
        .name = TYPE_I2C_WIRE,
        .parent = TYPE_I2C_BUS,
        .instance_size = sizeof(I2cWire),
        .instance_init = sgi_i2c_wire_bus_init,
        .instance_finalize = sgi_i2c_wire_bus_finalize,
        .class_init = sgi_i2c_wire_bus_class_init,
    },
    {
        .name = TYPE_I2C_WIRE_ENDPOINT,
        .parent = TYPE_INTERFACE,
        .class_size = sizeof(I2CWireEndpointClass),
    },
};

DEFINE_TYPES(sgi_i2c_wire_types)
