/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * The module controller's grant scheduler, destructive actions and reset,
 * driven against the real adapters, controller and shared wire.
 */
#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/i2c.h"
#include "hw/i2c/pcf8584.h"
#include "hw/i2c/sgi-i2c-wire.h"
#include "hw/misc/sgi-o2000-module-mgmt.h"
#include "qapi/error.h"
#include "qemu/main-loop.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "system/cpu-timers.h"
#include "system/reset.h"
#include "migration/vmstate.h"
#include "../migration/qemu-file.h"
#include "../migration/savevm.h"
#include "io/channel-file.h"

#define TYPE_MODULE_PROBE "sgi-o2000-module-test-probe"
/* A bare slave with no migration state, to be refused by the wire. */
#define TYPE_MODULE_MUTE "sgi-o2000-module-test-mute"

/* One millisecond of service tick plus room for a grant to finish. */
#define SERVICE_PERIOD_NS   1000000
#define GRANT_SETTLE_NS     500000
#define PULSE_NS            100000000
#define RECOVERY_TICKS      5000

#define GRANT_BASE          0xe0
#define GRANT_MESSAGE_BIT   0x08

/* An address no module endpoint answers, so the probe is never selected. */
#define PROBE_ADDRESS       0x7e

static int64_t virtual_time;
static int64_t virtual_time_step;

int64_t cpus_get_virtual_clock(void)
{
    int64_t now = virtual_time;

    virtual_time += virtual_time_step;
    return now;
}

void cpus_set_virtual_clock(int64_t value)
{
    virtual_time = value;
}

typedef struct Probe {
    I2CSlave parent_obj;
    /* Counters only the test reads; nothing here needs to migrate. */
    unsigned addresses, selections, writes;
    uint8_t last_address;
} Probe;

static void probe_wire_address(I2CSlave *slave, uint8_t address_byte)
{
    Probe *p = (Probe *)slave;

    p->addresses++;
    p->last_address = address_byte;
}

static int probe_event(I2CSlave *slave, enum i2c_event event)
{
    Probe *p = (Probe *)slave;

    if (event == I2C_START_SEND || event == I2C_START_RECV) {
        p->selections++;
    }
    return 0;
}

static int probe_send(I2CSlave *slave, uint8_t data)
{
    ((Probe *)slave)->writes++;
    return 0;
}

/* A wired bus refuses a slave that cannot report in when the wire loads. */
static const VMStateDescription vmstate_probe = {
    .name = "sgi-o2000-module-test-probe",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_I2C_SLAVE(parent_obj, Probe),
        VMSTATE_END_OF_LIST()
    },
};

static void probe_class_init(ObjectClass *oc, const void *data)
{
    I2CSlaveClass *sc = I2C_SLAVE_CLASS(oc);
    I2CWireEndpointClass *ec = I2C_WIRE_ENDPOINT_CLASS(oc);

    DEVICE_CLASS(oc)->vmsd = &vmstate_probe;
    sc->event = probe_event;
    sc->send = probe_send;
    ec->wire_address = probe_wire_address;
}

/* The device takes its serial from the machine's model; any ten bytes do. */
static DeviceState *module_new(void)
{
    DeviceState *dev = qdev_new(TYPE_SGI_O2000_MODULE_MGMT);

    qdev_prop_set_string(dev, "model-serial", "KORIGAMI01");
    return dev;
}

enum { LINE_RESET, LINE_NMI, LINE_POWER };

typedef struct Fixture {
    SGIO2000ModuleMgmtState *module;
    Probe *probe;
    int level[3];
    unsigned raises[3];
} Fixture;

static void line_handler(void *opaque, int line, int level)
{
    Fixture *f = opaque;

    if (level && !f->level[line]) {
        f->raises[line]++;
    }
    f->level[line] = level;
}

static void advance_to(int64_t time)
{
    g_assert_cmpint(qemu_clock_advance_virtual_time(time), ==, time);
}

static void setup(Fixture *f, gconstpointer unused)
{
    DeviceState *dev = module_new();
    static const char *const names[] = { "reset", "nmi", "power" };
    unsigned i;

    /*
     * No cold reset here on purpose; test_machine_reset drives that path
     * explicitly, and leaving setup unreset keeps the freshly realized
     * complex under test.
     */
    virtual_time_step = 0;
    memset(f, 0, sizeof(*f));
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        g_autofree char *capability = g_strdup_printf("can-%s", names[i]);

        qdev_prop_set_bit(dev, capability, true);
        qdev_connect_gpio_out_named(dev, names[i], 0,
                                    qemu_allocate_irq(line_handler, f, i));
    }
    qdev_realize(dev, NULL, &error_abort);
    f->module = SGI_O2000_MODULE_MGMT(dev);
    f->probe = (Probe *)i2c_slave_create_simple(
        sgi_o2000_module_mgmt_get_bus(f->module), TYPE_MODULE_PROBE,
        PROBE_ADDRESS);
}

static void teardown(Fixture *f, gconstpointer unused)
{
    object_unparent(OBJECT(f->module));
    object_unref(OBJECT(f->module));
}

/*
 * A node's own address, written the way its firmware writes it, so a test can
 * tell whether something reached over and reset that node's adapter.
 */
static void give_adapter_an_address(Fixture *f, unsigned slot, uint8_t address)
{
    PCF8584State *adapter =
        PCF8584(sgi_o2000_module_mgmt_get_adapter(f->module, slot));

    pcf8584_write_register(adapter, PCF8584_A0_STRIDE, PCF8584_CTL_PIN, 1);
    pcf8584_write_register(adapter, 0, address, 1);
    g_assert_cmphex(pcf8584_read_register(adapter, 0, 1), ==, address);
}

static uint8_t adapter_address(Fixture *f, unsigned slot)
{
    return PCF8584(sgi_o2000_module_mgmt_get_adapter(f->module, slot))
        ->own_address;
}

/* One service tick, then long enough for any grant it starts to finish. */
static void run_tick(Fixture *f)
{
    advance_to(f->module->service_deadline + GRANT_SETTLE_NS);
}

static void run_ticks(Fixture *f, unsigned count)
{
    while (count--) {
        run_tick(f);
    }
}

/* A clock advance while the first grant is staged must not abort it. */
static void test_cold_start_clock_progress(Fixture *f, gconstpointer unused)
{
    cpus_set_virtual_clock(f->module->service_deadline);
    virtual_time_step = 1;
    f->module->service_timer->cb(f->module->service_timer->opaque);
    virtual_time_step = 0;
    advance_to(virtual_time + GRANT_SETTLE_NS);

    g_assert_cmpuint(f->probe->addresses, ==, 1);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);
}

static void test_relative_deadline_range(Fixture *f, gconstpointer unused)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    int64_t saved_time = virtual_time;

    cpus_set_virtual_clock(1);
    g_assert_cmpint(i2c_wire_stage_after(bus, SGI_O2000_WIRE_MASTERS - 1,
                                         INT64_MAX, true, false), ==, -EINVAL);
    cpus_set_virtual_clock(INT64_MAX - 1);
    g_assert_cmpint(i2c_wire_stage_after(bus, SGI_O2000_WIRE_MASTERS - 1,
                                         1, true, false), ==, -EINVAL);
    cpus_set_virtual_clock(saved_time);
}

/*
 * The published grant is an address phase and nothing else: no endpoint
 * answers it, no data byte follows, and the bus is free again afterwards.
 */
static void test_grant_wire_shape(Fixture *f, gconstpointer unused)
{
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 1);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);
    g_assert_cmpuint(f->probe->selections, ==, 0);
    g_assert_cmpuint(f->probe->writes, ==, 0);
    g_assert_false(i2c_bus_busy(sgi_o2000_module_mgmt_get_bus(f->module)));
    g_assert_cmphex(i2c_wire_contenders(
                        sgi_o2000_module_mgmt_get_bus(f->module)), ==, 0);
    g_assert_true(i2c_wire_scl(sgi_o2000_module_mgmt_get_bus(f->module)));
    g_assert_true(i2c_wire_sda(sgi_o2000_module_mgmt_get_bus(f->module)));
}

/* Code zero is published first, then all eight in turn, then zero again. */
static void test_grant_rotation(Fixture *f, gconstpointer unused)
{
    unsigned i;

    for (i = 0; i < 8; i++) {
        run_tick(f);
        g_assert_cmpuint(f->probe->addresses, ==, i + 1);
        g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + i);
    }
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 9);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);
}

/* At most one grant per tick, whatever else is pending. */
static void test_one_grant_per_tick(Fixture *f, gconstpointer unused)
{
    run_ticks(f, 3);
    g_assert_cmpuint(f->probe->addresses, ==, 3);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 2);
}

/*
 * An attempt that never reaches the wire still consumed its slot: the code
 * advances before the attempt and a failure does not roll it back.
 */
static void test_failed_attempt_advances(Fixture *f, gconstpointer unused)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);

    run_tick(f);
    g_assert_cmpuint(f->module->processor_code, ==, 0);
    g_assert_cmpuint(f->module->recovery_ticks, ==, RECOVERY_TICKS);

    /* Occupy the controller's own wire request, so its next one is refused. */
    g_assert_cmpint(i2c_wire_stage(bus, SGI_O2000_WIRE_MASTERS - 1,
                                   f->module->service_deadline + 1,
                                   true, false), ==, 0);
    run_tick(f);
    g_assert_cmpuint(f->module->processor_code, ==, 1);
    g_assert_cmpuint(f->probe->addresses, ==, 1);
    g_assert_false(f->module->grant_active);
    g_assert_cmpuint(f->module->recovery_ticks, <, RECOVERY_TICKS);

    run_tick(f);
    g_assert_cmpuint(f->module->processor_code, ==, 2);
    g_assert_cmpuint(f->probe->addresses, ==, 2);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 2);
}

/*
 * A controller transfer takes the tick ahead of a serial command, and a
 * handoff skips publication without advancing the processor code.
 */
static void test_handoff_priority(Fixture *f, gconstpointer unused)
{
    run_tick(f);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);

    sgi_o2000_module_mgmt_set_controller_request(f->module, true);
    sgi_o2000_module_mgmt_set_serial_request(f->module, true);
    run_tick(f);
    g_assert_true(sgi_o2000_module_mgmt_controller_granted(f->module));
    g_assert_false(sgi_o2000_module_mgmt_serial_granted(f->module));
    g_assert_cmpuint(f->probe->addresses, ==, 1);

    /* Still granted: the tick neither publishes nor advances. */
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 1);

    sgi_o2000_module_mgmt_release_controller(f->module);
    run_tick(f);
    g_assert_true(sgi_o2000_module_mgmt_serial_granted(f->module));
    g_assert_cmpuint(f->probe->addresses, ==, 1);

    /* A serial command owning the path suppresses publication. */
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 1);

    sgi_o2000_module_mgmt_release_serial(f->module);
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 2);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 1);
}

/*
 * A pending controller message blocks both handoffs while the grant it
 * belongs to is still published, with its message bit set.
 */
static void test_pending_message_blocks_handoff(Fixture *f,
                                                gconstpointer unused)
{
    sgi_o2000_module_mgmt_set_message(f->module, 1, true);
    sgi_o2000_module_mgmt_set_controller_request(f->module, true);
    sgi_o2000_module_mgmt_set_serial_request(f->module, true);

    run_tick(f);
    g_assert_false(sgi_o2000_module_mgmt_controller_granted(f->module));
    g_assert_false(sgi_o2000_module_mgmt_serial_granted(f->module));
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);

    run_tick(f);
    g_assert_cmphex(f->probe->last_address, ==,
                    GRANT_BASE + 1 + GRANT_MESSAGE_BIT);

    /* Fetching clears that processor's indication and nobody else's. */
    sgi_o2000_module_mgmt_set_message(f->module, 2, true);
    sgi_o2000_module_mgmt_message_fetched(f->module, 1);
    run_tick(f);
    g_assert_cmphex(f->probe->last_address, ==,
                    GRANT_BASE + 2 + GRANT_MESSAGE_BIT);
    /* Round the rotation back to code one, which is clear again. */
    run_ticks(f, 7);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 1);
}

/* The global pending state expires on its own about a second later. */
static void test_message_expiry(Fixture *f, gconstpointer unused)
{
    sgi_o2000_module_mgmt_set_message(f->module, 3, true);
    run_ticks(f, 4);
    g_assert_cmphex(f->probe->last_address, ==,
                    GRANT_BASE + 3 + GRANT_MESSAGE_BIT);

    advance_to(f->module->message_deadline);
    g_assert_cmpuint(f->module->pending_messages, ==, 0);
    for (unsigned i = 0; i < 8; i++) {
        run_tick(f);
        g_assert_cmphex(f->probe->last_address & GRANT_MESSAGE_BIT, ==, 0);
    }
}

/*
 * Five seconds of ineligible ticks reinitialise the controller, which resets
 * the processor counter while leaving the enable latch alone, so the next
 * eligible tick publishes code zero again.
 */
static void test_recovery_reinitialises(Fixture *f, gconstpointer unused)
{
    give_adapter_an_address(f, 0, 0x09);
    give_adapter_an_address(f, 3, 0x0c);
    run_ticks(f, 3);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 2);

    sgi_o2000_module_mgmt_set_controller_request(f->module, true);
    run_tick(f);
    g_assert_true(sgi_o2000_module_mgmt_controller_granted(f->module));
    /* An already granted tick decrements the recovery counter. */
    run_ticks(f, RECOVERY_TICKS - 1);
    g_assert_true(sgi_o2000_module_mgmt_controller_granted(f->module));
    g_assert_cmpuint(f->module->recovery_ticks, ==, 1);
    run_tick(f);
    g_assert_false(sgi_o2000_module_mgmt_controller_granted(f->module));
    g_assert_cmpuint(f->module->recovery_ticks, ==, RECOVERY_TICKS);
    g_assert_true(f->module->publication_enabled);

    run_tick(f);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);

    /*
     * The controller reinitialised itself and reached no further. A node may
     * hold the bus for as long as it likes, so a recovery that took its
     * peers' adapters down would lose three nodes' configuration because one
     * was slow.
     */
    g_assert_cmphex(adapter_address(f, 0), ==, 0x09);
    g_assert_cmphex(adapter_address(f, 3), ==, 0x0c);
}

/* A serial command holding the path leaves the recovery counter alone. */
static void test_recovery_counter_holds(Fixture *f, gconstpointer unused)
{
    sgi_o2000_module_mgmt_set_serial_request(f->module, true);
    run_tick(f);
    g_assert_true(sgi_o2000_module_mgmt_serial_granted(f->module));
    run_ticks(f, 10);
    g_assert_cmpuint(f->module->recovery_ticks, ==, RECOVERY_TICKS);

    sgi_o2000_module_mgmt_set_publication(f->module, false);
    sgi_o2000_module_mgmt_release_serial(f->module);
    run_ticks(f, 10);
    g_assert_cmpuint(f->module->recovery_ticks, ==, RECOVERY_TICKS);
    g_assert_cmpuint(f->probe->addresses, ==, 0);
}

/*
 * A reset request drives its line for the reviewed hundred milliseconds, and
 * grants go on being published throughout: the pulse is a blocking delay on
 * the same periodic interrupt, not a stopped scheduler.
 */
static void test_reset_pulse(Fixture *f, gconstpointer unused)
{
    int64_t pulse_end;

    run_tick(f);
    sgi_o2000_module_mgmt_pulse_reset(f->module);
    pulse_end = f->module->pulse_deadline;
    g_assert_cmpint(pulse_end - virtual_time, ==, PULSE_NS);
    g_assert_cmpint(f->level[LINE_RESET], ==, 1);
    g_assert_cmpint(f->level[LINE_NMI], ==, 0);
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 1);

    advance_to(pulse_end - SERVICE_PERIOD_NS);
    g_assert_cmpint(f->level[LINE_RESET], ==, 1);
    /* Grant attempts continue through the pulse. */
    g_assert_cmpuint(f->probe->addresses, >, 1);

    advance_to(pulse_end);
    g_assert_cmpint(f->level[LINE_RESET], ==, 0);
    g_assert_true(f->module->publication_enabled);
    /* Reinitialisation reset the counter; code zero comes round again. */
    run_tick(f);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);
}

static void test_nmi_pulse(Fixture *f, gconstpointer unused)
{
    int64_t pulse_end;

    sgi_o2000_module_mgmt_pulse_nmi(f->module);
    pulse_end = f->module->pulse_deadline;
    g_assert_cmpint(f->level[LINE_NMI], ==, 1);
    g_assert_cmpint(f->level[LINE_RESET], ==, 0);
    advance_to(pulse_end - 1);
    g_assert_cmpint(f->level[LINE_NMI], ==, 1);
    advance_to(pulse_end);
    g_assert_cmpint(f->level[LINE_NMI], ==, 0);
    g_assert_cmpuint(f->raises[LINE_NMI], ==, 1);
    g_assert_cmpuint(f->raises[LINE_RESET], ==, 0);
}

/*
 * Power-down stops publication before the power line drops; power-up brings
 * the interface back and re-enables publication from code zero.
 */
static void test_power_cycle(Fixture *f, gconstpointer unused)
{
    run_ticks(f, 3);
    g_assert_cmpint(f->level[LINE_POWER], ==, 1);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 2);

    g_assert_true(sgi_o2000_module_mgmt_set_power(f->module, false));
    g_assert_cmpint(f->level[LINE_POWER], ==, 0);
    g_assert_false(f->module->publication_enabled);
    run_ticks(f, 5);
    g_assert_cmpuint(f->probe->addresses, ==, 3);

    g_assert_true(sgi_o2000_module_mgmt_set_power(f->module, true));
    g_assert_cmpint(f->level[LINE_POWER], ==, 1);
    g_assert_cmpuint(f->raises[LINE_POWER], ==, 2);
    g_assert_true(f->module->publication_enabled);
    run_tick(f);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);
}

/*
 * The coordinator sits on no bus. A whole-machine reset must still reach it,
 * its shared wire and its adapters together, leaving no scheduler state, no
 * pending message and no half-driven pulse behind.
 */
static void test_machine_reset(Fixture *f, gconstpointer unused)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);

    run_ticks(f, 3);
    sgi_o2000_module_mgmt_set_message(f->module, 5, true);
    sgi_o2000_module_mgmt_set_controller_request(f->module, true);
    sgi_o2000_module_mgmt_pulse_reset(f->module);
    g_assert_cmpint(f->level[LINE_RESET], ==, 1);
    g_assert_cmpuint(f->module->pending_messages, !=, 0);
    /*
     * Stop inside a grant, so the wire is genuinely occupied when the reset
     * lands. Resetting from an idle bus asserts nothing about the wire.
     */
    advance_to(f->module->service_deadline + 20000);
    g_assert_true(f->module->grant_active);
    g_assert_true(i2c_bus_busy(bus));
    g_assert_cmphex(i2c_wire_contenders(bus), !=, 0);

    qemu_devices_reset(RESET_TYPE_COLD);

    g_assert_cmpint(f->level[LINE_RESET], ==, 0);
    g_assert_cmpint(f->level[LINE_NMI], ==, 0);
    g_assert_cmpint(f->level[LINE_POWER], ==, 1);
    g_assert_false(f->module->pulse_active);
    g_assert_cmpuint(f->module->pending_messages, ==, 0);
    g_assert_false(f->module->controller_request);
    g_assert_false(f->module->controller_granted);
    g_assert_false(f->module->grant_active);
    g_assert_cmpuint(f->module->recovery_ticks, ==, RECOVERY_TICKS);
    g_assert_false(i2c_bus_busy(bus));
    g_assert_cmphex(i2c_wire_contenders(bus), ==, 0);
    g_assert_true(i2c_wire_scl(bus));
    g_assert_true(i2c_wire_sda(bus));

    /* And the rotation restarts rather than resuming mid-cycle. */
    f->probe->addresses = 0;
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 1);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE);
}

/*
 * Declaring an action without wiring it is the fictional sink the whole
 * refusal exists to prevent, so it fails at realize instead of reaching a
 * guest as an acknowledgement, and the message names the capability that
 * was left unwired rather than whichever one the check happened to reach
 * first.
 */
static void test_declared_action_needs_its_line(Fixture *f,
                                                gconstpointer connected)
{
    DeviceState *dev = module_new();
    static const char *const names[] = { "reset", "nmi", "power" };
    /* The first capability left unwired for each parametrisation. */
    static const char *const names_missing[] = { "can-reset", "can-nmi",
                                                 "can-power" };
    unsigned present = GPOINTER_TO_UINT(connected);
    Error *err = NULL;
    unsigned i;

    g_assert_cmpuint(present, <, ARRAY_SIZE(names));
    for (i = 0; i < ARRAY_SIZE(names); i++) {
        g_autofree char *capability = g_strdup_printf("can-%s", names[i]);

        qdev_prop_set_bit(dev, capability, true);
        if (i < present) {
            qdev_connect_gpio_out_named(dev, names[i], 0,
                                        qemu_allocate_irq(line_handler, f, i));
        }
    }
    g_assert_false(qdev_realize(dev, NULL, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), names_missing[present]));
    error_free(err);
    if (OBJECT(dev)->parent) {
        object_unparent(OBJECT(dev));
    }
    object_unref(OBJECT(dev));
}

/*
 * The other direction of the same rule. A line arriving without its
 * declaration would be driven by a module that refuses every command that
 * drives it, so it is a build error rather than a quiet mismatch.
 */
static void test_connected_line_needs_its_capability(Fixture *f,
                                                     gconstpointer unused)
{
    DeviceState *dev = module_new();
    Error *err = NULL;

    qdev_connect_gpio_out_named(dev, "power", 0,
                                qemu_allocate_irq(line_handler, f,
                                                  LINE_POWER));
    g_assert_false(qdev_realize(dev, NULL, &err));
    g_assert_nonnull(err);
    g_assert_nonnull(strstr(error_get_pretty(err), "can-power"));
    error_free(err);
    if (OBJECT(dev)->parent) {
        object_unparent(OBJECT(dev));
    }
    object_unref(OBJECT(dev));
}

/*
 * A module that declares nothing still builds. An installation whose reset,
 * NMI and power paths are somebody else's is a real configuration, and every
 * destructive command it is given is refused rather than acknowledged.
 */
static void test_undeclared_actions_refused(void)
{
    DeviceState *dev = module_new();
    SGIO2000ModuleMgmtState *module;

    qdev_realize(dev, NULL, &error_abort);
    module = SGI_O2000_MODULE_MGMT(dev);

    g_assert_false(sgi_o2000_module_mgmt_can_execute(module,
                                                     SGI_ELSC_ACTION_RESET));
    g_assert_false(sgi_o2000_module_mgmt_can_execute(module,
                                                     SGI_ELSC_ACTION_CYCLE));
    g_assert_false(sgi_o2000_module_mgmt_can_execute(
                       module, SGI_ELSC_ACTION_SHUTDOWN));
    g_assert_false(sgi_o2000_module_mgmt_pulse_reset(module));
    g_assert_false(sgi_o2000_module_mgmt_pulse_nmi(module));
    g_assert_false(sgi_o2000_module_mgmt_set_power(module, false));
    g_assert_false(sgi_o2000_module_mgmt_set_power(module, true));
    /*
     * A refused call reports that it did nothing and leaves the module as it
     * was: no pulse to finish, the module still powered, and the scheduler
     * still publishing.
     */
    g_assert_false(module->pulse_active);
    g_assert_true(module->power_on);
    g_assert_true(module->publication_enabled);

    /* A reset cannot hand a module a capability it was not given. */
    qemu_devices_reset(RESET_TYPE_COLD);
    g_assert_false(sgi_o2000_module_mgmt_can_execute(module,
                                                     SGI_ELSC_ACTION_RESET));
    g_assert_false(sgi_o2000_module_mgmt_can_execute(module,
                                                     SGI_ELSC_ACTION_CYCLE));

    object_unparent(OBJECT(dev));
    object_unref(OBJECT(dev));
}

/*
 * Capabilities are answered one at a time. A module wired for reset alone
 * accepts a reset and refuses the power commands, rather than the two
 * travelling together because one line happens to be connected.
 */
static void test_partial_capability(Fixture *f, gconstpointer unused)
{
    DeviceState *dev = module_new();
    SGIO2000ModuleMgmtState *module;

    qdev_prop_set_bit(dev, "can-reset", true);
    qdev_connect_gpio_out_named(dev, "reset", 0,
                                qemu_allocate_irq(line_handler, f,
                                                  LINE_RESET));
    qdev_realize(dev, NULL, &error_abort);
    module = SGI_O2000_MODULE_MGMT(dev);

    g_assert_true(sgi_o2000_module_mgmt_can_execute(module,
                                                    SGI_ELSC_ACTION_RESET));
    g_assert_false(sgi_o2000_module_mgmt_can_execute(module,
                                                     SGI_ELSC_ACTION_CYCLE));
    g_assert_false(sgi_o2000_module_mgmt_can_execute(
                       module, SGI_ELSC_ACTION_SHUTDOWN));
    /*
     * No level to check on the refused line: an undeclared output cannot be
     * connected, so the refusal is what makes it unobservable.
     */
    g_assert_false(sgi_o2000_module_mgmt_pulse_nmi(module));
    g_assert_false(module->pulse_active);

    g_assert_true(sgi_o2000_module_mgmt_pulse_reset(module));
    g_assert_cmpint(f->level[LINE_RESET], ==, 1);
    advance_to(module->pulse_deadline);
    g_assert_cmpint(f->level[LINE_RESET], ==, 0);

    object_unparent(OBJECT(dev));
    object_unref(OBJECT(dev));
}

/*
 * The wire's load protocol counts slaves in by their migration state, so a
 * slave carrying none would leave the count short for good and the
 * destination's clock stopped. It is refused when it tries to join instead.
 */
static void test_wire_refuses_a_mute_slave(Fixture *f, gconstpointer unused)
{
    I2CSlave *mute = I2C_SLAVE(object_new(TYPE_MODULE_MUTE));
    Error *err = NULL;

    g_assert_false(i2c_slave_realize_and_unref(
                       mute, sgi_o2000_module_mgmt_get_bus(f->module), &err));
    g_assert_nonnull(err);
    error_free(err);
}

/* A second destructive action while a pulse runs is refused, not swallowed. */
static void test_overlapping_pulse_refused(Fixture *f, gconstpointer unused)
{
    g_assert_true(sgi_o2000_module_mgmt_can_execute(f->module,
                                                    SGI_ELSC_ACTION_RESET));
    sgi_o2000_module_mgmt_pulse_reset(f->module);
    g_assert_false(sgi_o2000_module_mgmt_can_execute(f->module,
                                                     SGI_ELSC_ACTION_RESET));
    advance_to(f->module->pulse_deadline);
    g_assert_cmpint(f->level[LINE_RESET], ==, 0);
    g_assert_true(sgi_o2000_module_mgmt_can_execute(f->module,
                                                    SGI_ELSC_ACTION_RESET));
}

static QEMUFile *state_file(int fd, bool save)
{
    QIOChannelFile *ioc;
    QEMUFile *file;

    g_assert_cmpint(lseek(fd, 0, SEEK_SET), ==, 0);
    ioc = qio_channel_file_new_fd(dup(fd));
    file = save ? qemu_file_new_output(QIO_CHANNEL(ioc)) :
                  qemu_file_new_input(QIO_CHANNEL(ioc));
    object_unref(OBJECT(ioc));
    return file;
}

static const VMStateDescription *module_vmsd(Fixture *f)
{
    return DEVICE_GET_CLASS(DEVICE(f->module))->vmsd;
}

/*
 * The wire only finishes loading once every endpoint on it has, so the whole
 * complex moves together: the coordinator, the shared bus, each endpoint and
 * each adapter.
 */
static void transfer_complex(Fixture *f, QEMUFile *file, bool save)
{
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    BusChild *kid;
    unsigned i;

    if (save) {
        g_assert_cmpint(vmstate_save_state(file, module_vmsd(f), f->module,
                                           NULL, &error_abort), ==, 0);
        g_assert_cmpint(vmstate_save_state(file, &vmstate_sgi_i2c_wire_bus, bus,
                                           NULL, &error_abort), ==, 0);
    } else {
        g_assert_cmpint(vmstate_load_state(file, module_vmsd(f), f->module,
                                           module_vmsd(f)->version_id,
                                           &error_abort), ==, 0);
        g_assert_cmpint(vmstate_load_state(file, &vmstate_sgi_i2c_wire_bus, bus,
                                            1, &error_abort), ==, 0);
    }
    QTAILQ_FOREACH(kid, &bus->qbus.children, sibling) {
        I2CSlave *slave = I2C_SLAVE(kid->child);
        const VMStateDescription *own = DEVICE_GET_CLASS(kid->child)->vmsd;

        if (save) {
            g_assert_cmpint(vmstate_save_state(file, &vmstate_i2c_slave,
                                               slave, NULL, &error_abort),
                            ==, 0);
        } else {
            g_assert_cmpint(vmstate_load_state(file, &vmstate_i2c_slave,
                                               slave, 1, &error_abort),
                            ==, 0);
        }
        /* An endpoint whose own state is just its bus state moves once. */
        if (!own || own == &vmstate_i2c_slave) {
            continue;
        }
        if (save) {
            g_assert_cmpint(vmstate_save_state(file, own, kid->child, NULL,
                                               &error_abort), ==, 0);
        } else {
            g_assert_cmpint(vmstate_load_state(file, own, kid->child,
                                               own->version_id,
                                               &error_abort), ==, 0);
        }
    }
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        DeviceState *adapter = sgi_o2000_module_mgmt_get_adapter(f->module, i);
        const VMStateDescription *vmsd = DEVICE_GET_CLASS(adapter)->vmsd;

        if (save) {
            g_assert_cmpint(vmstate_save_state(file, vmsd, adapter, NULL,
                                               &error_abort), ==, 0);
        } else {
            g_assert_cmpint(vmstate_load_state(file, vmsd, adapter,
                                               vmsd->version_id,
                                               &error_abort), ==, 0);
        }
    }
}

/*
 * Saved between grants and restored, the scheduler carries on from the code
 * it had reached, publishing one grant per tick and no duplicates.
 */
static void test_migration(Fixture *f, gconstpointer mid_grant)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-o2000-module-XXXXXX", &path, NULL);
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    int64_t saved_time;
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);

    run_ticks(f, 3);
    sgi_o2000_module_mgmt_set_message(f->module, 4, true);
    if (mid_grant) {
        /* Stop inside the fourth grant, before its acknowledgement clock. */
        advance_to(f->module->service_deadline + 20000);
        g_assert_true(f->module->grant_active);
        g_assert_true(i2c_bus_busy(bus));
    }
    saved_time = virtual_time;

    file = state_file(fd, true);
    transfer_complex(f, file, true);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    qemu_devices_reset(RESET_TYPE_COLD);
    f->probe->addresses = 0;
    f->probe->last_address = 0;
    g_assert_cmpint(virtual_time, ==, saved_time);

    file = state_file(fd, false);
    transfer_complex(f, file, false);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    if (mid_grant) {
        /* The interrupted grant finishes, exactly once. */
        advance_to(virtual_time + 200000);
        g_assert_cmpuint(f->probe->addresses, ==, 1);
        g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 3);
        f->probe->addresses = 0;
    } else {
        run_tick(f);
        g_assert_cmpuint(f->probe->addresses, ==, 1);
        g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 3);
        f->probe->addresses = 0;
    }
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 1);
    g_assert_cmphex(f->probe->last_address, ==,
                    GRANT_BASE + 4 + GRANT_MESSAGE_BIT);
    run_tick(f);
    g_assert_cmpuint(f->probe->addresses, ==, 2);
    g_assert_cmphex(f->probe->last_address, ==, GRANT_BASE + 5);
    close(fd);
}

/* A grant observed before STOP remains pending across migration. */
static void test_migration_pending_grant(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-o2000-module-grant-XXXXXX", &path, NULL);
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    PCF8584State *adapters[SGI_O2000_ADAPTER_COUNT];
    uint8_t old_data[SGI_O2000_ADAPTER_COUNT];
    QEMUFile *file;
    unsigned i, attempts;
    bool all_pending = false;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);

    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        adapters[i] = PCF8584(
            sgi_o2000_module_mgmt_get_adapter(f->module, i));
        give_adapter_an_address(f, i, 0x09 + i);
        pcf8584_write_register(adapters[i], PCF8584_A0_STRIDE,
                               PCF8584_CTL_PIN | PCF8584_CTL_ESO, 1);
        old_data[i] = 0x20 + i;
        pcf8584_write_register(adapters[i], 0, old_data[i], 1);
    }

    advance_to(f->module->service_deadline + 1);
    for (attempts = 0; attempts < 500; attempts++) {
        all_pending = true;
        for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
            all_pending &= adapters[i]->wire_grant_pending;
        }
        if (all_pending) {
            break;
        }
        advance_to(virtual_time + 1000);
    }
    g_assert_true(all_pending);
    g_assert_true(i2c_bus_busy(bus));
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_assert_cmphex(adapters[i]->data, ==, old_data[i]);
        g_assert_cmphex(adapters[i]->wire_grant, ==, GRANT_BASE);
    }

    file = state_file(fd, true);
    transfer_complex(f, file, true);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    qemu_devices_reset(RESET_TYPE_COLD);
    f->probe->addresses = 0;
    f->probe->last_address = 0;
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_assert_cmphex(adapters[i]->data, ==, 0);
        g_assert_false(adapters[i]->wire_grant_pending);
    }

    file = state_file(fd, false);
    transfer_complex(f, file, false);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    g_assert_true(i2c_bus_busy(bus));
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_assert_true(adapters[i]->wire_grant_pending);
        g_assert_cmphex(adapters[i]->wire_grant, ==, GRANT_BASE);
        g_assert_cmphex(adapters[i]->data, ==, old_data[i]);
    }

    advance_to(virtual_time + 200000);
    g_assert_false(i2c_bus_busy(bus));
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_assert_false(adapters[i]->wire_grant_pending);
        g_assert_cmphex(adapters[i]->data, ==, GRANT_BASE);
        adapters[i]->data = 0x5a;
    }
    /* A cleared pending flag must not publish the saved grant a second time. */
    advance_to(virtual_time + 100000);
    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_assert_false(adapters[i]->wire_grant_pending);
        g_assert_cmphex(adapters[i]->data, ==, 0x5a);
    }
    g_assert_cmpuint(f->probe->addresses, ==, 0);
    close(fd);
}

/* A START restored behind a controller grant loses when the grant stops. */
static void test_migration_waiting_start(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-o2000-module-start-XXXXXX", &path, NULL);
    I2CBus *bus = sgi_o2000_module_mgmt_get_bus(f->module);
    PCF8584State *adapter = PCF8584(
        sgi_o2000_module_mgmt_get_adapter(f->module, 0));
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);

    pcf8584_write_register(adapter, PCF8584_A0_STRIDE,
                           PCF8584_CTL_PIN | PCF8584_CTL_ES1, 1);
    pcf8584_write_register(adapter, 0, 0x1c, 1);
    pcf8584_write_register(adapter, PCF8584_A0_STRIDE, PCF8584_CTL_PIN, 1);
    pcf8584_write_register(adapter, 0, 0x09, 1);
    pcf8584_write_register(adapter, PCF8584_A0_STRIDE,
                           PCF8584_CTL_PIN | PCF8584_CTL_ESO, 1);

    run_tick(f);
    g_assert_cmphex(pcf8584_read_register(adapter, PCF8584_A0_STRIDE, 1) &
                    PCF8584_STS_BB, ==, PCF8584_STS_BB);
    advance_to(f->module->service_deadline + 12000);
    g_assert_true(f->module->grant_active);
    g_assert_true(i2c_bus_busy(bus));
    pcf8584_write_register(adapter, 0, 0xa0, 1);
    pcf8584_write_register(adapter, PCF8584_A0_STRIDE,
                           PCF8584_CTL_PIN | PCF8584_CTL_ESO |
                           PCF8584_CTL_STA | PCF8584_CTL_ACK, 1);
    g_assert_true(adapter->wire_start_waiting);

    file = state_file(fd, true);
    transfer_complex(f, file, true);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);

    qemu_devices_reset(RESET_TYPE_COLD);
    g_assert_false(adapter->wire_start_waiting);
    file = state_file(fd, false);
    transfer_complex(f, file, false);
    g_assert_cmpint(qemu_get_byte(file), ==, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    g_assert_true(adapter->wire_start_waiting);
    g_assert_false(adapter->wire_active);

    advance_to(virtual_time + GRANT_SETTLE_NS);
    g_assert_false(adapter->wire_active);
    g_assert_false(adapter->wire_start_waiting);
    g_assert_false(adapter->start_address_staged);
    g_assert_false(adapter->wire_grant_pending);
    g_assert_cmphex(adapter->status & PCF8584_STS_LAB, ==,
                    PCF8584_STS_LAB);
    g_assert_cmphex(adapter->status & PCF8584_STS_PIN, ==, 0);
    close(fd);
}

static void test_migration_rejects_bad_state(Fixture *f, gconstpointer unused)
{
    g_autofree char *path = NULL;
    int fd = g_file_open_tmp("test-o2000-module-bad-XXXXXX", &path, NULL);
    Error *err = NULL;
    QEMUFile *file;

    g_assert_cmpint(fd, >=, 0);
    g_assert_cmpint(unlink(path), ==, 0);

    /* An idle controller cannot be in the middle of a grant. */
    f->module->grant_active = false;
    f->module->grant_state = 2;
    file = state_file(fd, true);
    g_assert_cmpint(vmstate_save_state(file, module_vmsd(f), f->module,
                                       NULL, &error_abort), ==, 0);
    qemu_put_byte(file, QEMU_VM_EOF);
    g_assert_cmpint(qemu_fclose(file), ==, 0);
    f->module->grant_state = 0;

    file = state_file(fd, false);
    g_assert_cmpint(vmstate_load_state(file, module_vmsd(f), f->module,
                                       module_vmsd(f)->version_id,
                                       &err), ==, -EINVAL);
    error_free(err);
    qemu_fclose(file);
    close(fd);
}

int main(int argc, char **argv)
{
    const TypeInfo probe = {
        .name = TYPE_MODULE_PROBE, .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(Probe), .class_init = probe_class_init,
        .interfaces = (const InterfaceInfo[]) {
            { TYPE_I2C_WIRE_ENDPOINT },
            { }
        },
    };
    const TypeInfo mute = {
        .name = TYPE_MODULE_MUTE, .parent = TYPE_I2C_SLAVE,
        .instance_size = sizeof(I2CSlave),
    };
    Object *machine;

    g_test_init(&argc, &argv, NULL);
    qemu_init_main_loop(&error_abort);
    qemu_clock_enable(QEMU_CLOCK_VIRTUAL, true);
    module_call_init(MODULE_INIT_QOM);
    type_register_static(&probe);
    type_register_static(&mute);
    machine = object_property_add_new_container(object_get_root(), "machine");
    object_property_add_new_container(machine, "unattached");
    /* What a machine does, so the adapters share the machine reset tree. */
    qemu_register_resettable(OBJECT(sysbus_get_default()));

    g_test_add("/sgi-o2000-module-management/grant/wire-shape", Fixture, NULL,
               setup, test_grant_wire_shape, teardown);
    g_test_add("/sgi-o2000-module-management/grant/cold-start-clock-progress",
               Fixture, NULL, setup, test_cold_start_clock_progress, teardown);
    g_test_add("/sgi-o2000-module-management/wire/relative-deadline-range",
               Fixture, NULL, setup, test_relative_deadline_range, teardown);
    g_test_add("/sgi-o2000-module-management/grant/rotation", Fixture, NULL,
               setup, test_grant_rotation, teardown);
    g_test_add("/sgi-o2000-module-management/grant/one-per-tick", Fixture,
               NULL, setup, test_one_grant_per_tick, teardown);
    g_test_add("/sgi-o2000-module-management/grant/failed-attempt", Fixture,
               NULL, setup, test_failed_attempt_advances, teardown);
    g_test_add("/sgi-o2000-module-management/grant/handoff-priority", Fixture,
               NULL, setup, test_handoff_priority, teardown);
    g_test_add("/sgi-o2000-module-management/grant/message-blocks-handoff",
               Fixture, NULL, setup, test_pending_message_blocks_handoff,
               teardown);
    g_test_add("/sgi-o2000-module-management/message/expiry", Fixture, NULL,
               setup, test_message_expiry, teardown);
    g_test_add("/sgi-o2000-module-management/recovery/reinitialise", Fixture,
               NULL, setup, test_recovery_reinitialises, teardown);
    g_test_add("/sgi-o2000-module-management/recovery/counter-holds", Fixture,
               NULL, setup, test_recovery_counter_holds, teardown);
    g_test_add("/sgi-o2000-module-management/action/reset-pulse", Fixture,
               NULL, setup, test_reset_pulse, teardown);
    g_test_add("/sgi-o2000-module-management/action/nmi-pulse", Fixture, NULL,
               setup, test_nmi_pulse, teardown);
    g_test_add("/sgi-o2000-module-management/action/power-cycle", Fixture,
               NULL, setup, test_power_cycle, teardown);
    g_test_add("/sgi-o2000-module-management/action/overlapping-pulse",
               Fixture, NULL, setup, test_overlapping_pulse_refused, teardown);
    g_test_add("/sgi-o2000-module-management/reset/machine", Fixture, NULL,
               setup, test_machine_reset, teardown);
    g_test_add("/sgi-o2000-module-management/action/partial-declaration",
               Fixture, NULL, setup, test_partial_capability, teardown);
    g_test_add_func("/sgi-o2000-module-management/action/declares-nothing",
                    test_undeclared_actions_refused);
    g_test_add("/sgi-o2000-module-management/realize/needs-declared-lines",
               Fixture, GUINT_TO_POINTER(0), setup,
               test_declared_action_needs_its_line, teardown);
    g_test_add("/sgi-o2000-module-management/realize/needs-power-line",
               Fixture, GUINT_TO_POINTER(2), setup,
               test_declared_action_needs_its_line, teardown);
    g_test_add("/sgi-o2000-module-management/wire/refuses-mute-slave",
               Fixture, NULL, setup, test_wire_refuses_a_mute_slave,
               teardown);
    g_test_add("/sgi-o2000-module-management/realize/needs-capability",
               Fixture, NULL, setup,
               test_connected_line_needs_its_capability, teardown);
    g_test_add("/sgi-o2000-module-management/migration", Fixture, NULL,
               setup, test_migration, teardown);
    g_test_add("/sgi-o2000-module-management/migration-mid-grant", Fixture,
               GUINT_TO_POINTER(1), setup, test_migration, teardown);
    g_test_add("/sgi-o2000-module-management/migration-pending-grant", Fixture,
               NULL, setup, test_migration_pending_grant, teardown);
    g_test_add("/sgi-o2000-module-management/migration-waiting-start", Fixture,
               NULL, setup, test_migration_waiting_start, teardown);
    g_test_add("/sgi-o2000-module-management/migration-rejects-bad-state",
               Fixture, NULL, setup, test_migration_rejects_bad_state,
               teardown);
    return g_test_run();
}
