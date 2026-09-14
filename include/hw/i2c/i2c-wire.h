/* SPDX-License-Identifier: LGPL-2.1-or-later */
/*
 * Origami I2C wire bus: a TYPE_I2C_BUS subclass that owns the resolved-master
 * byte API and the timed open-drain SDA/SCL transport used by the SGI Origin
 * module-management and system-controller paths. Stock hw/i2c/core.c models a
 * single anonymous master and immediate transfers; this module adds stable
 * participant ids, bit-level arbitration, clock stretching and repeated-START,
 * all off the shared file.
 */

#ifndef HW_I2C_I2C_WIRE_H
#define HW_I2C_I2C_WIRE_H

#include "hw/i2c/i2c.h"
#include "qom/object.h"

/* A transfer to this address is a general call and reaches every slave. */
#define I2C_BROADCAST 0x00

enum i2c_wire_service {
    I2C_WIRE_ADDRESS_SEND,
    I2C_WIRE_ADDRESS_RECV,
    I2C_WIRE_WRITE,
    I2C_WIRE_READ_ACK,
    I2C_WIRE_READ_NACK,
    I2C_WIRE_SERVICE_COUNT,
};

/*
 * A slave joins a resolved or timed bus by implementing this interface. Stock
 * I2C slaves do not, so the wire never selects one; membership is the explicit
 * opt-in that used to be I2CSlaveClass::supports_master_ids, and it means the
 * slave's event handler copes with I2C_RESTART without STOP side effects. The
 * three callbacks are optional.
 */
#define TYPE_I2C_WIRE_ENDPOINT "i2c-wire-endpoint"
typedef struct I2CWireEndpointClass I2CWireEndpointClass;
DECLARE_CLASS_CHECKERS(I2CWireEndpointClass, I2C_WIRE_ENDPOINT,
                       TYPE_I2C_WIRE_ENDPOINT)

struct I2CWireEndpointClass {
    InterfaceClass parent_class;

    /*
     * Notify a wire-mode endpoint after the ninth clock completes. Returning
     * true holds SCL low for service; i2c_wire_hold(..., false) releases that
     * endpoint independently of other receivers.
     */
    bool (*wire_service)(I2CSlave *s, enum i2c_wire_service service);

    /*
     * A complete address phase, acknowledged or not, reported to every
     * endpoint on a wire-mode bus rather than only to the selected ones.
     * An endpoint that models passive address observation decides for
     * itself what to keep; observing is not selection, and this callback
     * must not acknowledge, hold SCL or change addressed-as-slave state.
     */
    void (*wire_address)(I2CSlave *s, uint8_t address_byte);

    /*
     * Observe an address phase on an untimed bus that selected no receiver.
     * This notification does not acknowledge or select the observer.
     */
    void (*unmatched_address)(I2CSlave *s, uint8_t address, bool recv);
};

/*
 * Per-slave wire state. Stock struct I2CSlave carries none of this; the bus
 * subclass keeps one of these per attached child, so the base class stays
 * stock. Exposed for the white-box unit tests, which read and drive it
 * directly through i2c_wire_slave_state().
 */
typedef struct I2CWireSlave {
    bool selected;              /* member of the bus's current_devs */
    bool wire_hold, wire_hold_pending, wire_hold_value;
    int64_t wire_hold_at;
    uint64_t wire_hold_phase, wire_service_phase;
    uint8_t wire_service_kind;
    bool wire_service_pending;
} I2CWireSlave;

#define TYPE_I2C_WIRE "i2c-wire"
OBJECT_DECLARE_SIMPLE_TYPE(I2cWire, I2C_WIRE)

/* At most this many participants and this many slaves on one wire. */
#define I2C_WIRE_MAX_MASTERS 32
#define I2C_WIRE_MAX_SLAVES  32

typedef struct I2CWire I2CWire;

struct I2cWire {
    I2CBus parent_obj;

    /* Opt-in configuration: stable participant IDs are 0..master_count-1. */
    uint32_t master_count;
    uint32_t owner;             /* UINT32_MAX between STOP and resolved START */
    uint8_t requested_address;
    bool master_recv;
    bool address_valid;
    bool masters_loaded;        /* Migration subsection was present. */
    I2CWire *wire;
    bool wire_loaded;
    bool wire_loading;

    /* Per-child wire state, keyed by the child I2CSlave pointer. */
    GHashTable *slaves;

    /* Migration staging: per-child state serialised in child order. */
    uint32_t migrated_count;
    I2CWireSlave migrated[I2C_WIRE_MAX_SLAVES];
};

/*
 * The runtime per-slave wire state for a slave attached to a wire bus. Created
 * on first use; the entry lives until the bus is finalised. The slave must be
 * a child of a TYPE_I2C_WIRE.
 */
I2CWireSlave *i2c_wire_slave_state(I2CSlave *slave);

/*
 * Transfers for a resolved sole master, not arbitration. Configuration is fixed
 * once enabled. The caller must establish a sole master before delivering a
 * resolved START; this function must not be used to elect one from contenders.
 * Negative errno values report API misuse or unsupported input. They are not
 * hardware flags.
 * Address transfer and send return 0 for ACK, 1 for NAK, or a negative
 * API error. General-call writes select only address-accepting receivers;
 * any receiver ACK produces ACK, and data NAK does not deselect a receiver.
 * Address zero with recv=true is the START byte and receives no ACK.
 * Anonymous transfer APIs must not be used on an opted-in bus.
 */
int i2c_bus_enable_master_ids(I2CBus *bus, uint32_t count);
int i2c_bus_start_resolved(I2CBus *bus, uint32_t master);
int i2c_master_start_transfer(I2CBus *bus, uint32_t master,
                              uint8_t address, bool recv);
int i2c_master_send(I2CBus *bus, uint32_t master, uint8_t data);
int i2c_master_recv(I2CBus *bus, uint32_t master, uint8_t *data);
int i2c_master_nack(I2CBus *bus, uint32_t master);
int i2c_master_end_transfer(I2CBus *bus, uint32_t master);

/*
 * Timed open-drain transport. Durations are caller-supplied nanoseconds, not
 * inferred controller timings. One pending request per participant is allowed.
 * Requests are future virtual deadlines; all requests at one deadline settle
 * before the observer runs. True SDA releases the line. Clock enables that
 * participant's configured clock generation. The relative and minimum-delay
 * entry points derive their deadlines from one internal clock sample, or,
 * while the timeline advances, from the instant being settled. No request
 * elects a sole owner.
 */
typedef struct I2CWireEvent {
    int64_t time;
    uint64_t contenders;
    uint64_t lost;
    int error;
    bool scl;
    bool sda;
} I2CWireEvent;

typedef void (*I2CWireObserver)(void *opaque, const I2CWireEvent *event);

/*
 * A participant with deadlines of its own reports the earliest through its
 * deadline hook, and does everything due at an instant, leaving nothing at or
 * before it, when its run hook is called with that instant.
 */
typedef int64_t (*I2CWireDeadline)(void *opaque);
typedef void (*I2CWireRun)(void *opaque, int64_t now);

/*
 * Every slave on a wired bus must carry I2C slave migration state, directly
 * or through a description that embeds it. The wire finishes loading only
 * once each slave's post-load has reported in, so one that never does leaves
 * a destination with the wire's clock stopped. Attaching such a slave to a
 * wired bus fails, and so does enabling a wire under one.
 */
int i2c_bus_enable_wire(I2CBus *bus, uint32_t count,
                         I2CWireObserver observe, void *opaque, Error **errp);
int i2c_wire_configure(I2CBus *bus, uint32_t master, int64_t low_ns,
                       int64_t high_ns, bool clock_after_loss);
int i2c_wire_stage(I2CBus *bus, uint32_t master, int64_t at,
                   bool sda, bool clock);
int i2c_wire_stage_at_or_after(I2CBus *bus, uint32_t master, int64_t at,
                               int64_t min_delay_ns, bool sda, bool clock);
int i2c_wire_stage_after(I2CBus *bus, uint32_t master, int64_t delay_ns,
                         bool sda, bool clock);
int i2c_wire_hold(I2CSlave *slave, int64_t at, bool hold);
int i2c_wire_hold_after(I2CSlave *slave, int64_t delay_ns, bool hold);
int i2c_wire_reset_master(I2CBus *bus, uint32_t master);
/*
 * Stop a driving master's clock generator while it goes on holding SCL low,
 * for a controller that waits between bytes. Staging its clock again resumes
 * from the high phase. One observer per participant may be attached; it sees
 * every published event alongside the bus-wide observer.
 */
int i2c_wire_pause_master(I2CBus *bus, uint32_t master);
int i2c_wire_set_observer(I2CBus *bus, uint32_t master,
                          I2CWireObserver observe, void *opaque);
int i2c_wire_clear_observer(I2CBus *bus, uint32_t master,
                            I2CWireObserver observe, void *opaque);
/*
 * One timeline for the wire and the participants timed against it.
 * i2c_wire_advance settles every deadline up to the present in time order,
 * each at its own instant, so a device can bring the wire up to date from
 * whatever path its guest reaches it by. The wire's timer, and a participant's
 * own timers, need only call it: a deadline already settled is not settled
 * again. i2c_wire_now reports the instant being settled inside an advance and
 * the present outside one. One timeline per participant may be attached.
 */
int i2c_wire_set_timeline(I2CBus *bus, uint32_t master, I2CWireDeadline next,
                          I2CWireRun run, void *opaque);
int i2c_wire_clear_timeline(I2CBus *bus, uint32_t master, I2CWireRun run,
                            void *opaque);
void i2c_wire_advance(I2CBus *bus);
int64_t i2c_wire_now(I2CBus *bus);
bool i2c_wire_scl(I2CBus *bus);
bool i2c_wire_sda(I2CBus *bus);
uint64_t i2c_wire_contenders(I2CBus *bus);

/* Resolved participant count and current owner, for our controller models. */
uint32_t i2c_bus_master_count(I2CBus *bus);
uint32_t i2c_bus_resolved_owner(I2CBus *bus);

/* The subclass migrates masters, wire and per-slave state in this section. */
extern const VMStateDescription vmstate_sgi_i2c_wire_bus;

#endif /* HW_I2C_SGI_I2C_WIRE_H */
