/*
 * SGI Origin 2000 shared module management
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 *
 * Modelled from the module wiring and PCF8584/ELSC contracts in the Origami
 * project tree's datasheets.
 */

#include "qemu/osdep.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/pcf8584.h"
#include "hw/misc/sgi-o2000-module-mgmt.h"
#include "hw/nvram/eeprom_at24c.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "system/reset.h"

/* The controller drives the wire as the participant after the adapters. */
#define GRANT_MASTER            SGI_O2000_ADAPTER_COUNT

#define SERVICE_PERIOD_NS       1000000     /* one-millisecond service tick */
/* The global pending state expires about a second after the last message. */
#define MESSAGE_LIFETIME_NS     1000000000
#define PULSE_NS                100000000   /* 100 ms reset and NMI pulses */
#define RECOVERY_TICKS          5000        /* five seconds of service ticks */

/*
 * 90 kHz split into halves differing by at most a nanosecond. A deterministic
 * approximation from the published nominal rate, not a measured duty cycle.
 */
#define WIRE_HALF_PERIOD_NS     5556

#define PROCESSOR_CODES         8
#define GRANT_BASE              0xe0
#define GRANT_MESSAGE_BIT       0x08

/* Address-only grant transaction, from START to the STOP that ends it. */
enum {
    GRANT_IDLE,
    GRANT_START,
    GRANT_BITS,
    GRANT_ACK,
    GRANT_STOP_CLOCK,
    GRANT_STOP_RELEASE,
};

static void mgmt_reset_state(SGIO2000ModuleMgmtState *s)
{
    s->processor_code = PROCESSOR_CODES - 1;
    s->pending_messages = 0;
    s->controller_request = false;
    s->controller_granted = false;
    s->serial_request = false;
    s->serial_granted = false;
    s->slave_active = false;
    s->console_input_pending = false;
    s->console_processor = 0;
    s->grant_active = false;
    s->grant_state = GRANT_IDLE;
    s->grant_bit = 0;
    s->grant_byte = 0;
    s->grant_bit_sampled = false;
    s->wire_scl = s->wire_sda = true;
    s->recovery_ticks = RECOVERY_TICKS;
    timer_del(s->message_timer);
}

/*
 * The controller's own reinitialisation: its wire interface and its protocol
 * queues go back to reset, and the publication enable latch is left alone.
 * It reaches no further. A reset does not establish a reset of the peers on
 * a shared bus, and what an owner's reset does to a peer's transfer is not
 * settled, so the node adapters and the shared NVRAM stay untouched here. A
 * module reset reaches the nodes through the reset line the machine wires.
 * The endpoint loses its framing queues and keeps everything that is not a
 * queue, which is as much as the datasheet gives this path.
 */
static void mgmt_reinitialize(SGIO2000ModuleMgmtState *s)
{
    mgmt_reset_state(s);
    if (i2c_wire_reset_master(s->bus, GRANT_MASTER) == -ENOTSUP) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: reinitialised while "
                      "driving the shared wire\n");
    }
    sgi_elsc_reinitialize(s->msc);
}

static void mgmt_recovery_tick(SGIO2000ModuleMgmtState *s)
{
    if (s->recovery_ticks && --s->recovery_ticks == 0) {
        mgmt_reinitialize(s);
    }
}

static bool mgmt_stage(SGIO2000ModuleMgmtState *s, bool sda, bool clock,
                       int64_t delay_ns)
{
    int ret = i2c_wire_stage(s->bus, GRANT_MASTER,
                             qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + delay_ns,
                             sda, clock);

    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2000-module-management: grant transfer refused by "
                      "the shared wire: %d\n", ret);
        s->grant_active = false;
        s->grant_state = GRANT_IDLE;
        mgmt_recovery_tick(s);
        return false;
    }
    return true;
}

static void mgmt_wire_event(void *opaque, const I2CWireEvent *event)
{
    SGIO2000ModuleMgmtState *s = opaque;
    bool old_scl = s->wire_scl;
    bool old_sda = s->wire_sda;

    s->wire_scl = event->scl;
    s->wire_sda = event->sda;
    if (!s->grant_active) {
        return;
    }
    if (event->error || (event->lost & (1ULL << GRANT_MASTER))) {
        s->grant_active = false;
        s->grant_state = GRANT_IDLE;
        mgmt_recovery_tick(s);
        return;
    }

    switch (s->grant_state) {
    case GRANT_START:
        if (!(old_scl && event->scl && old_sda && !event->sda)) {
            break;
        }
        if (!(i2c_wire_contenders(s->bus) & (1ULL << GRANT_MASTER))) {
            /*
             * A peer opened the bus between the eligibility check and this
             * controller's own line transition, so the START belongs to it.
             * Drop the grant rather than clock a byte onto its transaction.
             */
            qemu_log_mask(LOG_UNIMP,
                          "sgi-o2000-module-management: a peer's START "
                          "overtook the controller's grant\n");
            s->grant_active = false;
            s->grant_state = GRANT_IDLE;
            i2c_wire_reset_master(s->bus, GRANT_MASTER);
            mgmt_recovery_tick(s);
            break;
        }
        s->grant_state = GRANT_BITS;
        s->grant_bit = 0;
        s->grant_bit_sampled = false;
        /* One high interval of START hold before the first clock. */
        mgmt_stage(s, s->grant_byte & 0x80, true, WIRE_HALF_PERIOD_NS);
        break;
    case GRANT_BITS:
        if (!old_scl && event->scl) {
            s->grant_bit_sampled = true;
        } else if (old_scl && !event->scl && s->grant_bit_sampled) {
            s->grant_bit_sampled = false;
            s->grant_bit++;
            if (s->grant_bit < 8) {
                mgmt_stage(s, s->grant_byte & (0x80 >> s->grant_bit),
                           true, 1);
            } else {
                s->grant_state = GRANT_ACK;
                mgmt_stage(s, true, true, 1);
            }
        }
        break;
    case GRANT_ACK:
        if (!old_scl && event->scl) {
            s->grant_bit_sampled = true;
        } else if (old_scl && !event->scl && s->grant_bit_sampled) {
            s->grant_bit_sampled = false;
            s->grant_state = GRANT_STOP_CLOCK;
            mgmt_stage(s, false, false, 1);
        }
        break;
    case GRANT_STOP_CLOCK:
        if (!old_scl && event->scl && !event->sda) {
            s->grant_state = GRANT_STOP_RELEASE;
            mgmt_stage(s, true, false, WIRE_HALF_PERIOD_NS);
        }
        break;
    case GRANT_STOP_RELEASE:
        if (old_scl && event->scl && !old_sda && event->sda) {
            s->grant_active = false;
            s->grant_state = GRANT_IDLE;
            /* A completed attempt reloads the recovery counter. */
            s->recovery_ticks = RECOVERY_TICKS;
        }
        break;
    default:
        break;
    }
}

/* The indication is recomputed for the code about to be published. */
static bool mgmt_message_for(SGIO2000ModuleMgmtState *s, uint8_t code)
{
    if (s->pending_messages & (1U << code)) {
        return true;
    }
    return s->console_input_pending && s->console_processor == code;
}

static void mgmt_publish(SGIO2000ModuleMgmtState *s)
{
    s->processor_code = (s->processor_code + 1) % PROCESSOR_CODES;
    s->grant_byte = GRANT_BASE | s->processor_code |
                    (mgmt_message_for(s, s->processor_code) ?
                     GRANT_MESSAGE_BIT : 0);
    s->grant_active = true;
    s->grant_state = GRANT_START;
    s->wire_scl = i2c_wire_scl(s->bus);
    s->wire_sda = i2c_wire_sda(s->bus);
    mgmt_stage(s, false, false, 1);
}

static void mgmt_service(void *opaque)
{
    SGIO2000ModuleMgmtState *s = opaque;

    s->service_deadline += SERVICE_PERIOD_NS;
    timer_mod_ns(s->service_timer, s->service_deadline);

    /*
     * A serial command owning the protocol path, or publication switched
     * off, leaves the recovery counter alone.
     */
    if (!s->publication_enabled || s->serial_granted) {
        return;
    }
    if (s->controller_granted || s->slave_active || s->grant_active ||
        i2c_bus_busy(s->bus) || i2c_wire_contenders(s->bus)) {
        mgmt_recovery_tick(s);
        return;
    }
    /*
     * A pending controller interrupt suppresses both handoffs, and a handoff
     * that happens skips publication without advancing the processor code.
     */
    if (!s->pending_messages && s->controller_request) {
        s->controller_request = false;
        s->controller_granted = true;
        return;
    }
    if (!s->pending_messages && s->serial_request) {
        s->serial_request = false;
        s->serial_granted = true;
        return;
    }
    mgmt_publish(s);
}

static void mgmt_message_expired(void *opaque)
{
    SGIO2000ModuleMgmtState *s = opaque;

    s->pending_messages = 0;
}

/*
 * Every line this device drives goes through here, so a line it must not
 * drive stays undriven whatever order a machine connected it in. Realize
 * already refuses a declaration without its output and an output without
 * its declaration, and this keeps the two from depending on that order.
 */
static void mgmt_drive_pulse_lines(SGIO2000ModuleMgmtState *s, bool reset,
                                   bool nmi)
{
    if (s->can_reset) {
        qemu_set_irq(s->reset_out, reset);
    }
    if (s->can_nmi) {
        qemu_set_irq(s->nmi_out, nmi);
    }
}

static void mgmt_drive_power(SGIO2000ModuleMgmtState *s)
{
    if (s->can_power) {
        qemu_set_irq(s->power_out, s->power_on);
    }
}

static void mgmt_pulse_complete(void *opaque)
{
    SGIO2000ModuleMgmtState *s = opaque;

    mgmt_drive_pulse_lines(s, false, false);
    s->pulse_active = false;
    s->pulse_nmi = false;
    mgmt_reinitialize(s);
    if (s->power_on) {
        s->publication_enabled = true;
    }
}

static bool mgmt_start_pulse(SGIO2000ModuleMgmtState *s, bool nmi)
{
    if (!(nmi ? s->can_nmi : s->can_reset)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: %s requested on a module "
                      "that declares no %s path\n", nmi ? "NMI" : "reset",
                      nmi ? "NMI" : "reset");
        return false;
    }
    if (s->pulse_active) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: %s requested during a %s "
                      "pulse\n", nmi ? "NMI" : "reset",
                      s->pulse_nmi ? "NMI" : "reset");
        return false;
    }
    s->pulse_active = true;
    s->pulse_nmi = nmi;
    mgmt_drive_pulse_lines(s, !nmi, nmi);
    s->pulse_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) + PULSE_NS;
    timer_mod_ns(s->pulse_timer, s->pulse_deadline);
    return true;
}

bool sgi_o2000_module_mgmt_pulse_reset(SGIO2000ModuleMgmtState *s)
{
    return mgmt_start_pulse(s, false);
}

bool sgi_o2000_module_mgmt_pulse_nmi(SGIO2000ModuleMgmtState *s)
{
    return mgmt_start_pulse(s, true);
}

bool sgi_o2000_module_mgmt_set_power(SGIO2000ModuleMgmtState *s, bool on)
{
    if (!s->can_power) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: power %s requested on a "
                      "module that declares no power path\n",
                      on ? "up" : "down");
        return false;
    }
    if (!on) {
        /* Publication stops before module reset and power sequencing. */
        s->publication_enabled = false;
        s->power_on = false;
        mgmt_reinitialize(s);
        mgmt_drive_power(s);
        return true;
    }
    s->power_on = true;
    mgmt_drive_power(s);
    /*
     * The interface is initialised once reset is released, then publication
     * is enabled.
     */
    mgmt_reinitialize(s);
    s->publication_enabled = true;
    return true;
}

void sgi_o2000_module_mgmt_set_controller_request(SGIO2000ModuleMgmtState *s,
                                                  bool pending)
{
    s->controller_request = pending;
}

void sgi_o2000_module_mgmt_set_serial_request(SGIO2000ModuleMgmtState *s,
                                              bool pending)
{
    s->serial_request = pending;
}

void sgi_o2000_module_mgmt_release_controller(SGIO2000ModuleMgmtState *s)
{
    s->controller_granted = false;
}

void sgi_o2000_module_mgmt_release_serial(SGIO2000ModuleMgmtState *s)
{
    s->serial_granted = false;
}

bool sgi_o2000_module_mgmt_controller_granted(SGIO2000ModuleMgmtState *s)
{
    return s->controller_granted;
}

bool sgi_o2000_module_mgmt_serial_granted(SGIO2000ModuleMgmtState *s)
{
    return s->serial_granted;
}

void sgi_o2000_module_mgmt_set_message(SGIO2000ModuleMgmtState *s,
                                       unsigned processor, bool pending)
{
    if (processor >= PROCESSOR_CODES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2000-module-management: message for processor %u, "
                      "outside the eight published codes\n", processor);
        return;
    }
    if (pending) {
        s->pending_messages |= 1U << processor;
        s->message_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                              MESSAGE_LIFETIME_NS;
        timer_mod_ns(s->message_timer, s->message_deadline);
    } else {
        s->pending_messages &= ~(1U << processor);
        if (!s->pending_messages) {
            timer_del(s->message_timer);
        }
    }
}

void sgi_o2000_module_mgmt_message_fetched(SGIO2000ModuleMgmtState *s,
                                           unsigned processor)
{
    sgi_o2000_module_mgmt_set_message(s, processor, false);
}

void sgi_o2000_module_mgmt_set_publication(SGIO2000ModuleMgmtState *s,
                                           bool enabled)
{
    s->publication_enabled = enabled;
}

I2CBus *sgi_o2000_module_mgmt_get_bus(SGIO2000ModuleMgmtState *s)
{
    return s->bus;
}

DeviceState *sgi_o2000_module_mgmt_get_adapter(SGIO2000ModuleMgmtState *s,
                                               unsigned index)
{
    return index < SGI_O2000_ADAPTER_COUNT ? s->adapters[index] : NULL;
}

/*
 * A destructive command must not be acknowledged unless it will happen. A
 * module declares each action it can carry out, so an installation whose
 * reset or power path belongs to something outside this model refuses the
 * command instead of answering for a line that reaches nothing.
 *
 * A declared action is still refused while a pulse runs. No source settles
 * that overlap, so every destructive action is refused for the duration
 * rather than each being given a different answer. That is a model choice,
 * not a fact.
 */
bool sgi_o2000_module_mgmt_can_execute(SGIO2000ModuleMgmtState *s,
                                       SGIElscAction action)
{
    bool declared;

    switch (action) {
    case SGI_ELSC_ACTION_RESET:
        declared = s->can_reset;
        break;
    case SGI_ELSC_ACTION_CYCLE:
    case SGI_ELSC_ACTION_SHUTDOWN:
        declared = s->can_power;
        break;
    default:
        return false;
    }
    if (!declared) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: destructive command "
                      "refused, this module declares no %s path\n",
                      action == SGI_ELSC_ACTION_RESET ? "reset" : "power");
        return false;
    }
    if (s->pulse_active) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: destructive command "
                      "during a %s pulse\n", s->pulse_nmi ? "NMI" : "reset");
        return false;
    }
    return true;
}

static bool mgmt_elsc_can_execute(void *opaque, SGIElscAction action)
{
    return sgi_o2000_module_mgmt_can_execute(opaque, action);
}

static void mgmt_elsc_execute(void *opaque, SGIElscAction action)
{
    SGIO2000ModuleMgmtState *s = opaque;
    bool done;

    switch (action) {
    case SGI_ELSC_ACTION_RESET:
        done = sgi_o2000_module_mgmt_pulse_reset(s);
        break;
    case SGI_ELSC_ACTION_CYCLE:
        done = sgi_o2000_module_mgmt_set_power(s, false) &&
               sgi_o2000_module_mgmt_set_power(s, true);
        break;
    case SGI_ELSC_ACTION_SHUTDOWN:
        done = sgi_o2000_module_mgmt_set_power(s, false);
        break;
    default:
        g_assert_not_reached();
    }
    /*
     * The controller only reaches this after can_execute cleared the action,
     * so a refusal here means the two disagree.
     */
    if (!done) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2000-module-management: an acknowledged action "
                      "did not happen\n");
    }
}

static void mgmt_elsc_slave_active(void *opaque, bool active)
{
    ((SGIO2000ModuleMgmtState *)opaque)->slave_active = active;
}

/*
 * Automatic input selection follows the processor that most recently sent
 * console output. Two model choices sit here, neither of them established:
 * the controller starts pointed at code zero, so input arriving before any
 * processor has spoken is announced to code zero; and the value handed over
 * is a QEMU CPU index treated as a grant processor code, which assumes how
 * the machine enumerates processors.
 */
static void mgmt_elsc_console_owner(void *opaque, unsigned processor)
{
    SGIO2000ModuleMgmtState *s = opaque;

    if (processor >= PROCESSOR_CODES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-o2000-module-management: console output from "
                      "processor %u, outside the eight published codes\n",
                      processor);
        return;
    }
    s->console_processor = processor;
}

static void mgmt_elsc_console_input(void *opaque, bool pending)
{
    ((SGIO2000ModuleMgmtState *)opaque)->console_input_pending = pending;
}

static const SGIElscModuleOps mgmt_elsc_ops = {
    .can_execute = mgmt_elsc_can_execute,
    .execute = mgmt_elsc_execute,
    .slave_active = mgmt_elsc_slave_active,
    .console_owner = mgmt_elsc_console_owner,
    .console_input = mgmt_elsc_console_input,
};

static DeviceState *mgmt_add_child(SGIO2000ModuleMgmtState *s,
                                   const char *name, const char *type)
{
    DeviceState *child = qdev_new(type);

    object_property_add_child(OBJECT(s), name, OBJECT(child));
    object_unref(OBJECT(child));
    return child;
}

static void mgmt_populate_nvram(SGIO2000ModuleMgmtState *s)
{
    uint8_t reserved[SGI_O2000_NVRAM_PAGE_BYTES] = { };
    unsigned page;

    reserved[SGI_O2000_NVRAM_VALIDITY_OFFSET] =
        SGI_O2000_NVRAM_VALIDITY_VALUE;
    reserved[SGI_O2000_NVRAM_MODULE_OFFSET] = s->module_number;

    for (page = 0; page < SGI_O2000_NVRAM_ADDRESSES; page++) {
        uint8_t address = SGI_O2000_NVRAM_FIRST_ADDR + page;

        if (page == SGI_O2000_NVRAM_RESERVED_PAGE) {
            at24c_eeprom_init_rom(s->bus, address, SGI_O2000_NVRAM_PAGE_BYTES,
                                  reserved, sizeof(reserved));
        } else {
            at24c_eeprom_init(s->bus, address, SGI_O2000_NVRAM_PAGE_BYTES);
        }
    }
}

static void mgmt_realize(DeviceState *dev, Error **errp)
{
    SGIO2000ModuleMgmtState *s = SGI_O2000_MODULE_MGMT(dev);
    unsigned i;

    const struct {
        const char *line_name;
        const char *capability;
        bool declared;
        qemu_irq line;
    } actions[] = {
        { "reset", "can-reset", s->can_reset, s->reset_out },
        { "nmi",   "can-nmi",   s->can_nmi,   s->nmi_out   },
        { "power", "can-power", s->can_power, s->power_out },
    };

    /*
     * A declaration and its output go together in both directions. Without
     * the output a declared command would answer "ok" for something nothing
     * carries out; without the declaration a connected line would be driven
     * by a module that refuses every command that drives it. Declaring
     * nothing and connecting nothing is a whole configuration of its own,
     * and those commands are refused.
     */
    for (i = 0; i < ARRAY_SIZE(actions); i++) {
        if (actions[i].declared && !actions[i].line) {
            error_setg(errp, "module management declares %s without "
                       "connecting its %s output",
                       actions[i].capability, actions[i].line_name);
            return;
        }
        if (!actions[i].declared && actions[i].line) {
            error_setg(errp, "module management connects its %s output "
                       "without declaring %s",
                       actions[i].line_name, actions[i].capability);
            return;
        }
    }

    if (i2c_bus_enable_wire(s->bus, SGI_O2000_WIRE_MASTERS, NULL, NULL)) {
        error_setg(errp, "cannot enable the module management I2C wire");
        return;
    }
    /*
     * Each adapter configures its own participant; this one is the
     * controller's.
     */
    if (i2c_wire_configure(s->bus, GRANT_MASTER, WIRE_HALF_PERIOD_NS,
                           WIRE_HALF_PERIOD_NS, false) ||
        i2c_wire_set_observer(s->bus, GRANT_MASTER, mgmt_wire_event, s)) {
        error_setg(errp, "cannot drive the module management I2C wire");
        return;
    }

    if (!qdev_realize(DEVICE(s->msc), BUS(s->bus), errp)) {
        return;
    }
    mgmt_populate_nvram(s);

    for (i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        g_autofree char *name = g_strdup_printf("adapter[%u]", i);

        s->adapters[i] = mgmt_add_child(s, name, TYPE_PCF8584);
        if (!object_property_set_link(OBJECT(s->adapters[i]), "bus",
                                      OBJECT(s->bus), errp)) {
            return;
        }
        qdev_prop_set_uint32(s->adapters[i], "wire-master", i);
        if (!sysbus_realize(SYS_BUS_DEVICE(s->adapters[i]), errp)) {
            return;
        }
    }

    s->service_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL, mgmt_service, s);
    s->message_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    mgmt_message_expired, s);
    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  mgmt_pulse_complete, s);
    mgmt_reset_state(s);
    s->power_on = true;
    s->publication_enabled = true;
    mgmt_drive_power(s);
    s->service_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          SERVICE_PERIOD_NS;
    timer_mod_ns(s->service_timer, s->service_deadline);

    /*
     * The coordinator sits on no bus, so the machine reset tree would miss it
     * and leave its scheduler and shared wire holding stale state while the
     * adapters around them reset.
     */
    qemu_register_resettable(OBJECT(s));
}

static void mgmt_unrealize(DeviceState *dev)
{
    SGIO2000ModuleMgmtState *s = SGI_O2000_MODULE_MGMT(dev);

    qemu_unregister_resettable(OBJECT(s));
    i2c_wire_clear_observer(s->bus, GRANT_MASTER, mgmt_wire_event, s);
    timer_free(s->service_timer);
    timer_free(s->message_timer);
    timer_free(s->pulse_timer);
    s->service_timer = s->message_timer = s->pulse_timer = NULL;
}

static void mgmt_init(Object *obj)
{
    SGIO2000ModuleMgmtState *s = SGI_O2000_MODULE_MGMT(obj);

    s->bus = i2c_init_bus(DEVICE(obj), "i2c");
    s->msc = SGI_ELSC(mgmt_add_child(s, "msc", TYPE_SGI_ELSC));
    sgi_elsc_set_module_ops(s->msc, &mgmt_elsc_ops, s);
    /*
     * The Origin 200 synthesised arbitration broadcast has no place here: on
     * a shared module the grants are real address-only wire traffic.
     */
    qdev_prop_set_bit(DEVICE(s->msc), "arbitration", false);
    i2c_slave_set_address(I2C_SLAVE(s->msc), SGI_O2000_MSC_ADDRESS);
    object_property_add_alias(obj, "chardev", OBJECT(s->msc), "chardev");
    object_property_add_alias(obj, "dip-switches", OBJECT(s->msc),
                              "dip-switches");

    qdev_init_gpio_out_named(DEVICE(obj), &s->reset_out, "reset", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->nmi_out, "nmi", 1);
    qdev_init_gpio_out_named(DEVICE(obj), &s->power_out, "power", 1);
}

static void mgmt_reset_hold(Object *obj, ResetType type)
{
    SGIO2000ModuleMgmtState *s = SGI_O2000_MODULE_MGMT(obj);

    /*
     * The shared bus is a child bus of this device and the adapters are on
     * the system bus, so the reset tree reaches them; resetting them from
     * here would be a reset inside a reset phase.
     */
    timer_del(s->pulse_timer);
    s->pulse_active = false;
    s->pulse_nmi = false;
    mgmt_drive_pulse_lines(s, false, false);
    mgmt_reset_state(s);
    s->power_on = true;
    s->publication_enabled = true;
    mgmt_drive_power(s);
    s->service_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          SERVICE_PERIOD_NS;
    timer_mod_ns(s->service_timer, s->service_deadline);
}

static int mgmt_post_load(void *opaque, int version_id)
{
    SGIO2000ModuleMgmtState *s = opaque;

    if (s->grant_state > GRANT_STOP_RELEASE || s->grant_bit > 8 ||
        s->processor_code >= PROCESSOR_CODES ||
        s->console_processor >= PROCESSOR_CODES ||
        (!s->grant_active && s->grant_state != GRANT_IDLE) ||
        (s->grant_active && s->grant_state == GRANT_IDLE) ||
        s->recovery_ticks > RECOVERY_TICKS) {
        return -EINVAL;
    }
    timer_mod_ns(s->service_timer, s->service_deadline);
    if (s->pending_messages) {
        timer_mod_ns(s->message_timer, s->message_deadline);
    } else {
        timer_del(s->message_timer);
    }
    if (s->pulse_active) {
        mgmt_drive_pulse_lines(s, !s->pulse_nmi, s->pulse_nmi);
        timer_mod_ns(s->pulse_timer, s->pulse_deadline);
    } else {
        mgmt_drive_pulse_lines(s, false, false);
        timer_del(s->pulse_timer);
    }
    mgmt_drive_power(s);
    return 0;
}

static const VMStateDescription vmstate_sgi_o2000_module_mgmt = {
    .name = "sgi-o2000-module-management",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mgmt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(service_deadline, SGIO2000ModuleMgmtState),
        VMSTATE_INT64(message_deadline, SGIO2000ModuleMgmtState),
        VMSTATE_INT64(pulse_deadline, SGIO2000ModuleMgmtState),
        VMSTATE_UINT16(recovery_ticks, SGIO2000ModuleMgmtState),
        VMSTATE_UINT8(processor_code, SGIO2000ModuleMgmtState),
        VMSTATE_UINT8(pending_messages, SGIO2000ModuleMgmtState),
        VMSTATE_UINT8(console_processor, SGIO2000ModuleMgmtState),
        VMSTATE_UINT8(grant_byte, SGIO2000ModuleMgmtState),
        VMSTATE_UINT8(grant_bit, SGIO2000ModuleMgmtState),
        VMSTATE_UINT8(grant_state, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(grant_bit_sampled, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(grant_active, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(wire_scl, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(wire_sda, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(publication_enabled, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(controller_request, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(controller_granted, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(serial_request, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(serial_granted, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(slave_active, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(console_input_pending, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(power_on, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(pulse_active, SGIO2000ModuleMgmtState),
        VMSTATE_BOOL(pulse_nmi, SGIO2000ModuleMgmtState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mgmt_properties[] = {
    DEFINE_PROP_UINT8("module-number", SGIO2000ModuleMgmtState,
                      module_number, 1),
    DEFINE_PROP_BOOL("can-reset", SGIO2000ModuleMgmtState, can_reset, false),
    DEFINE_PROP_BOOL("can-nmi", SGIO2000ModuleMgmtState, can_nmi, false),
    DEFINE_PROP_BOOL("can-power", SGIO2000ModuleMgmtState, can_power, false),
};

static void mgmt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Origin 2000 shared module management";
    dc->realize = mgmt_realize;
    dc->unrealize = mgmt_unrealize;
    dc->vmsd = &vmstate_sgi_o2000_module_mgmt;
    device_class_set_props(dc, mgmt_properties);
    /* A machine builds this complex and wires its outputs; -device cannot. */
    dc->user_creatable = false;
    rc->phases.hold = mgmt_reset_hold;
}

static const TypeInfo sgi_o2000_module_mgmt_info = {
    .name = TYPE_SGI_O2000_MODULE_MGMT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGIO2000ModuleMgmtState),
    .instance_init = mgmt_init,
    .class_init = mgmt_class_init,
};

static void sgi_o2000_module_mgmt_register_types(void)
{
    type_register_static(&sgi_o2000_module_mgmt_info);
}

type_init(sgi_o2000_module_mgmt_register_types)
