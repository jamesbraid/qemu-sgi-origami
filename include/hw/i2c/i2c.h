#ifndef QEMU_I2C_H
#define QEMU_I2C_H

#include "hw/core/qdev.h"
#include "qom/object.h"

/* The QEMU I2C implementation only supports simple transfers that complete
   immediately.  It does not support slave devices that need to be able to
   defer their response (eg. CPU slave interfaces where the data is supplied
   by the device driver in response to an interrupt).  */

/* A transfer to this address is a general call and reaches every slave. */
#define I2C_BROADCAST 0x00

enum i2c_event {
    I2C_START_RECV,
    I2C_START_SEND,
    I2C_START_SEND_ASYNC,
    I2C_FINISH,
    I2C_NACK, /* Masker NACKed a receive byte.  */
    I2C_RESTART /* Identified-master reselection, without STOP. */
};

enum i2c_wire_service {
    I2C_WIRE_ADDRESS_SEND,
    I2C_WIRE_ADDRESS_RECV,
    I2C_WIRE_WRITE,
    I2C_WIRE_READ_ACK,
    I2C_WIRE_READ_NACK,
    I2C_WIRE_SERVICE_COUNT,
};

typedef struct I2CNodeList I2CNodeList;

#define TYPE_I2C_SLAVE "i2c-slave"
OBJECT_DECLARE_TYPE(I2CSlave, I2CSlaveClass,
                    I2C_SLAVE)

struct I2CSlaveClass {
    DeviceClass parent_class;
    /* Explicit opt-in: event handles I2C_RESTART without STOP side effects. */
    bool supports_master_ids;

    /* Master to slave. Returns non-zero for a NAK, 0 for success. */
    int (*send)(I2CSlave *s, uint8_t data);

    /* Master to slave (asynchronous). Receiving slave must call i2c_ack(). */
    void (*send_async)(I2CSlave *s, uint8_t data);

    /*
     * Slave to master.  This cannot fail, the device should always
     * return something here.
     */
    uint8_t (*recv)(I2CSlave *s);

    /*
     * Notify the slave of a bus state change.  For start event,
     * returns non-zero to NAK an operation.  For other events the
     * return code is not used and should be zero.
     */
    int (*event)(I2CSlave *s, enum i2c_event event);

    /*
     * Notify a wire-mode endpoint after the ninth clock completes.  Returning
     * true holds SCL low for service; i2c_wire_hold(..., false) releases that
     * endpoint independently of other receivers.
     */
    bool (*wire_service)(I2CSlave *s, enum i2c_wire_service service);

    /*
     * Check if this device matches the address provided.  Returns bool of
     * true if it matches (or broadcast), and updates the device list, false
     * otherwise.
     *
     * If broadcast is true, match should add the device and return true.
     */
    bool (*match_and_add)(I2CSlave *candidate, uint8_t address, bool broadcast,
                          I2CNodeList *current_devs);
};

struct I2CSlave {
    DeviceState qdev;

    /* Remaining fields for internal use by the I2C code.  */
    uint8_t address;
    bool saved_selected;
    bool wire_hold, wire_hold_pending, wire_hold_value;
    int64_t wire_hold_at;
    uint64_t wire_hold_phase, wire_service_phase;
    uint8_t wire_service_kind;
    bool wire_service_pending;
};

#define TYPE_I2C_BUS "i2c-bus"
OBJECT_DECLARE_SIMPLE_TYPE(I2CBus, I2C_BUS)

typedef struct I2CNode I2CNode;

struct I2CNode {
    I2CSlave *elt;
    QLIST_ENTRY(I2CNode) next;
};

typedef struct I2CPendingMaster I2CPendingMaster;

struct I2CPendingMaster {
    QEMUBH *bh;
    QSIMPLEQ_ENTRY(I2CPendingMaster) entry;
};

typedef QLIST_HEAD(I2CNodeList, I2CNode) I2CNodeList;
typedef QSIMPLEQ_HEAD(I2CPendingMasters, I2CPendingMaster) I2CPendingMasters;

typedef struct I2CWire I2CWire;

struct I2CBus {
    BusState qbus;
    I2CNodeList current_devs;
    I2CPendingMasters pending_masters;
    uint8_t saved_address;
    bool broadcast;

    /* Set from slave currently mastering the bus. */
    QEMUBH *bh;

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
    uint32_t wire_slaves_loaded;
};

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
 * participant's configured clock generation. No request elects a sole owner.
 */
#define I2C_WIRE_MAX_MASTERS 32

typedef struct I2CWireEvent {
    int64_t time;
    uint64_t contenders;
    uint64_t lost;
    int error;
    bool scl;
    bool sda;
} I2CWireEvent;

typedef void (*I2CWireObserver)(void *opaque, const I2CWireEvent *event);

int i2c_bus_enable_wire(I2CBus *bus, uint32_t count,
                         I2CWireObserver observe, void *opaque);
int i2c_wire_configure(I2CBus *bus, uint32_t master, int64_t low_ns,
                       int64_t high_ns, bool clock_after_loss);
int i2c_wire_stage(I2CBus *bus, uint32_t master, int64_t at,
                   bool sda, bool clock);
int i2c_wire_hold(I2CSlave *slave, int64_t at, bool hold);
int i2c_wire_reset_master(I2CBus *bus, uint32_t master);
bool i2c_wire_scl(I2CBus *bus);
bool i2c_wire_sda(I2CBus *bus);
uint64_t i2c_wire_contenders(I2CBus *bus);

extern const VMStateDescription vmstate_i2c_bus;

I2CBus *i2c_init_bus(DeviceState *parent, const char *name);
int i2c_bus_busy(I2CBus *bus);

/**
 * i2c_start_transfer: start a transfer on an I2C bus.
 *
 * @bus: #I2CBus to be used
 * @address: address of the slave
 * @is_recv: indicates the transfer direction
 *
 * When @is_recv is a known boolean constant, use the
 * i2c_start_recv() or i2c_start_send() helper instead.
 *
 * Returns: 0 on success, -1 on error
 */
int i2c_start_transfer(I2CBus *bus, uint8_t address, bool is_recv);

/**
 * i2c_start_recv: start a 'receive' transfer on an I2C bus.
 *
 * @bus: #I2CBus to be used
 * @address: address of the slave
 *
 * Returns: 0 on success, -1 on error
 */
int i2c_start_recv(I2CBus *bus, uint8_t address);

/**
 * i2c_start_send: start a 'send' transfer on an I2C bus.
 *
 * @bus: #I2CBus to be used
 * @address: address of the slave
 *
 * Returns: 0 on success, -1 on error
 */
int i2c_start_send(I2CBus *bus, uint8_t address);

/**
 * i2c_start_send_async: start an asynchronous 'send' transfer on an I2C bus.
 *
 * @bus: #I2CBus to be used
 * @address: address of the slave
 *
 * Return: 0 on success, -1 on error
 */
int i2c_start_send_async(I2CBus *bus, uint8_t address);

void i2c_schedule_pending_master(I2CBus *bus);

void i2c_end_transfer(I2CBus *bus);
void i2c_nack(I2CBus *bus);
void i2c_ack(I2CBus *bus);
void i2c_bus_master(I2CBus *bus, QEMUBH *bh);
void i2c_bus_release(I2CBus *bus);
int i2c_send(I2CBus *bus, uint8_t data);
int i2c_send_async(I2CBus *bus, uint8_t data);
uint8_t i2c_recv(I2CBus *bus);
bool i2c_scan_bus(I2CBus *bus, uint8_t address, bool broadcast,
                  I2CNodeList *current_devs);

/**
 * Create an I2C slave device on the heap.
 * @name: a device type name
 * @addr: I2C address of the slave when put on a bus
 *
 * This only initializes the device state structure and allows
 * properties to be set. Type @name must exist. The device still
 * needs to be realized. See qdev-core.h.
 */
I2CSlave *i2c_slave_new(const char *name, uint8_t addr);

/**
 * Create and realize an I2C slave device on the heap.
 * @bus: I2C bus to put it on
 * @name: I2C slave device type name
 * @addr: I2C address of the slave when put on a bus
 *
 * Create the device state structure, initialize it, put it on the
 * specified @bus, and drop the reference to it (the device is realized).
 */
I2CSlave *i2c_slave_create_simple(I2CBus *bus, const char *name, uint8_t addr);

/**
 * Realize and drop a reference an I2C slave device
 * @dev: I2C slave device to realize
 * @bus: I2C bus to put it on
 * @addr: I2C address of the slave on the bus
 * @errp: pointer to NULL initialized error object
 *
 * Returns: %true on success, %false on failure.
 *
 * Call 'realize' on @dev, put it on the specified @bus, and drop the
 * reference to it.
 *
 * This function is useful if you have created @dev via qdev_new(),
 * i2c_slave_new() or i2c_slave_try_new() (which take a reference to
 * the device it returns to you), so that you can set properties on it
 * before realizing it. If you don't need to set properties then
 * i2c_slave_create_simple() is probably better (as it does the create,
 * init and realize in one step).
 *
 * If you are embedding the I2C slave into another QOM device and
 * initialized it via some variant on object_initialize_child() then
 * do not use this function, because that family of functions arrange
 * for the only reference to the child device to be held by the parent
 * via the child<> property, and so the reference-count-drop done here
 * would be incorrect.  (Instead you would want i2c_slave_realize(),
 * which doesn't currently exist but would be trivial to create if we
 * had any code that wanted it.)
 */
bool i2c_slave_realize_and_unref(I2CSlave *dev, I2CBus *bus, Error **errp);

/**
 * Set the I2C bus address of a slave device
 * @dev: I2C slave device
 * @address: I2C address of the slave when put on a bus
 */
void i2c_slave_set_address(I2CSlave *dev, uint8_t address);

extern const VMStateDescription vmstate_i2c_slave;

#define VMSTATE_I2C_SLAVE(_field, _state) {                          \
    .name       = (stringify(_field)),                               \
    .size       = sizeof(I2CSlave),                                  \
    .vmsd       = &vmstate_i2c_slave,                                \
    .flags      = VMS_STRUCT,                                        \
    .offset     = vmstate_offset_value(_state, _field, I2CSlave),    \
}

#endif
