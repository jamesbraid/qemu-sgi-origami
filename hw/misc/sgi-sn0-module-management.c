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
#include "hw/1wire/crc.h"
#include "hw/1wire/ds2502.h"
#include "hw/misc/sgi-onewire-slot.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/sysbus.h"
#include "hw/i2c/pcf8584.h"
#include "hw/i2c/i2c-wire.h"
#include "hw/misc/sgi-sn0-module-management.h"
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

static void mgmt_reset_state(SN0ModuleManagementState *s)
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
static void mgmt_reinitialize(SN0ModuleManagementState *s)
{
    mgmt_reset_state(s);
    if (i2c_wire_reset_master(s->bus, GRANT_MASTER) == -ENOTSUP) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-sn0-module-management: reinitialised while "
                      "driving the shared wire\n");
    }
    sgi_elsc_reinitialize(s->msc);
}

static void mgmt_recovery_tick(SN0ModuleManagementState *s)
{
    if (s->recovery_ticks && --s->recovery_ticks == 0) {
        mgmt_reinitialize(s);
    }
}

static bool mgmt_stage(SN0ModuleManagementState *s, bool sda, bool clock,
                       int64_t delay_ns)
{
    int ret = i2c_wire_stage_after(s->bus, GRANT_MASTER, delay_ns,
                                   sda, clock);

    if (ret) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-sn0-module-management: grant transfer refused by "
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
    SN0ModuleManagementState *s = opaque;
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
                          "sgi-sn0-module-management: a peer's START "
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
static bool mgmt_message_for(SN0ModuleManagementState *s, uint8_t code)
{
    if (s->pending_messages & (1U << code)) {
        return true;
    }
    return s->console_input_pending && s->console_processor == code;
}

static void mgmt_publish(SN0ModuleManagementState *s)
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

/* A staged adapter START owns tBUF before its lines make the bus busy. */
static bool mgmt_adapter_active(SN0ModuleManagementState *s)
{
    for (unsigned i = 0; i < SGI_O2000_ADAPTER_COUNT; i++) {
        if (PCF8584(s->adapters[i])->wire_active) {
            return true;
        }
    }
    return false;
}

static void mgmt_service(SN0ModuleManagementState *s)
{
    /*
     * Every millisecond of the timeline gets its tick, at its own instant,
     * however late the host gets round to running it.
     */
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
        mgmt_adapter_active(s) ||
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

static void mgmt_message_expired(SN0ModuleManagementState *s)
{
    s->pending_messages = 0;
}

/*
 * Every line this device drives goes through here, so a line it must not
 * drive stays undriven whatever order a machine connected it in. Realize
 * already refuses a declaration without its output and an output without
 * its declaration, and this keeps the two from depending on that order.
 */
static void mgmt_drive_pulse_lines(SN0ModuleManagementState *s, bool reset,
                                   bool nmi)
{
    if (s->can_reset) {
        qemu_set_irq(s->reset_out, reset);
    }
    if (s->can_nmi) {
        qemu_set_irq(s->nmi_out, nmi);
    }
}

static void mgmt_drive_power(SN0ModuleManagementState *s)
{
    if (s->can_power) {
        qemu_set_irq(s->power_out, s->power_on);
    }
}

static void mgmt_pulse_complete(SN0ModuleManagementState *s)
{
    mgmt_drive_pulse_lines(s, false, false);
    s->pulse_active = false;
    s->pulse_nmi = false;
    mgmt_reinitialize(s);
    if (s->power_on) {
        s->publication_enabled = true;
    }
}

/*
 * The controller's deadlines sit on the wire's timeline, so a node polling its
 * adapter runs them in order with the wire's own edges, each at its own
 * instant. A message expiry is armed a second ahead and a pulse a tenth of a
 * second, the service tick only a millisecond, so where they share an instant
 * the first two go first, as their timers ordinarily did.
 */
static int64_t mgmt_next_deadline(void *opaque)
{
    SN0ModuleManagementState *s = opaque;
    int64_t next = s->service_deadline;

    if (s->pending_messages) {
        next = MIN(next, s->message_deadline);
    }
    if (s->pulse_active) {
        next = MIN(next, s->pulse_deadline);
    }
    return next;
}

static void mgmt_run_deadlines(void *opaque, int64_t now)
{
    SN0ModuleManagementState *s = opaque;

    if (s->pending_messages && s->message_deadline <= now) {
        mgmt_message_expired(s);
    }
    if (s->pulse_active && s->pulse_deadline <= now) {
        mgmt_pulse_complete(s);
    }
    if (s->service_deadline <= now) {
        mgmt_service(s);
    }
}

/* The timers only stand in for a node that is not polling. */
static void mgmt_timer_expired(void *opaque)
{
    SN0ModuleManagementState *s = opaque;

    i2c_wire_advance(s->bus);
}

static bool mgmt_start_pulse(SN0ModuleManagementState *s, bool nmi)
{
    if (!(nmi ? s->can_nmi : s->can_reset)) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-sn0-module-management: %s requested on a module "
                      "that declares no %s path\n", nmi ? "NMI" : "reset",
                      nmi ? "NMI" : "reset");
        return false;
    }
    if (s->pulse_active) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-sn0-module-management: %s requested during a %s "
                      "pulse\n", nmi ? "NMI" : "reset",
                      s->pulse_nmi ? "NMI" : "reset");
        return false;
    }
    s->pulse_active = true;
    s->pulse_nmi = nmi;
    mgmt_drive_pulse_lines(s, !nmi, nmi);
    s->pulse_deadline = i2c_wire_now(s->bus) + PULSE_NS;
    timer_mod_ns(s->pulse_timer, s->pulse_deadline);
    return true;
}

bool sgi_sn0_module_management_pulse_reset(SN0ModuleManagementState *s)
{
    return mgmt_start_pulse(s, false);
}

bool sgi_sn0_module_management_pulse_nmi(SN0ModuleManagementState *s)
{
    return mgmt_start_pulse(s, true);
}

bool sgi_sn0_module_management_set_power(SN0ModuleManagementState *s, bool on)
{
    if (!s->can_power) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-sn0-module-management: power %s requested on a "
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

void sgi_sn0_module_management_set_controller_request(
    SN0ModuleManagementState *s, bool pending)
{
    s->controller_request = pending;
}

void sgi_sn0_module_management_set_serial_request(SN0ModuleManagementState *s,
                                              bool pending)
{
    s->serial_request = pending;
}

void sgi_sn0_module_management_release_controller(SN0ModuleManagementState *s)
{
    s->controller_granted = false;
}

void sgi_sn0_module_management_release_serial(SN0ModuleManagementState *s)
{
    s->serial_granted = false;
}

bool sgi_sn0_module_management_controller_granted(SN0ModuleManagementState *s)
{
    return s->controller_granted;
}

bool sgi_sn0_module_management_serial_granted(SN0ModuleManagementState *s)
{
    return s->serial_granted;
}

void sgi_sn0_module_management_set_message(SN0ModuleManagementState *s,
                                       unsigned processor, bool pending)
{
    if (processor >= PROCESSOR_CODES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-sn0-module-management: message for processor %u, "
                      "outside the eight published codes\n", processor);
        return;
    }
    if (pending) {
        s->pending_messages |= 1U << processor;
        s->message_deadline = i2c_wire_now(s->bus) + MESSAGE_LIFETIME_NS;
        timer_mod_ns(s->message_timer, s->message_deadline);
    } else {
        s->pending_messages &= ~(1U << processor);
        if (!s->pending_messages) {
            timer_del(s->message_timer);
        }
    }
}

void sgi_sn0_module_management_message_fetched(SN0ModuleManagementState *s,
                                           unsigned processor)
{
    sgi_sn0_module_management_set_message(s, processor, false);
}

void sgi_sn0_module_management_set_publication(SN0ModuleManagementState *s,
                                           bool enabled)
{
    s->publication_enabled = enabled;
}

I2CBus *sgi_sn0_module_management_get_bus(SN0ModuleManagementState *s)
{
    return s->bus;
}

DeviceState *sgi_sn0_module_management_get_adapter(SN0ModuleManagementState *s,
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
bool sgi_sn0_module_management_can_execute(SN0ModuleManagementState *s,
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
                      "sgi-sn0-module-management: destructive command "
                      "refused, this module declares no %s path\n",
                      action == SGI_ELSC_ACTION_RESET ? "reset" : "power");
        return false;
    }
    if (s->pulse_active) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-sn0-module-management: destructive command "
                      "during a %s pulse\n", s->pulse_nmi ? "NMI" : "reset");
        return false;
    }
    return true;
}

static bool mgmt_elsc_can_execute(void *opaque, SGIElscAction action)
{
    return sgi_sn0_module_management_can_execute(opaque, action);
}

static void mgmt_elsc_execute(void *opaque, SGIElscAction action)
{
    SN0ModuleManagementState *s = opaque;
    bool done;

    switch (action) {
    case SGI_ELSC_ACTION_RESET:
        done = sgi_sn0_module_management_pulse_reset(s);
        break;
    case SGI_ELSC_ACTION_CYCLE:
        done = sgi_sn0_module_management_set_power(s, false) &&
               sgi_sn0_module_management_set_power(s, true);
        break;
    case SGI_ELSC_ACTION_SHUTDOWN:
        done = sgi_sn0_module_management_set_power(s, false);
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
                      "sgi-sn0-module-management: an acknowledged action "
                      "did not happen\n");
    }
}

static void mgmt_elsc_slave_active(void *opaque, bool active)
{
    ((SN0ModuleManagementState *)opaque)->slave_active = active;
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
    SN0ModuleManagementState *s = opaque;

    if (processor >= PROCESSOR_CODES) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-sn0-module-management: console output from "
                      "processor %u, outside the eight published codes\n",
                      processor);
        return;
    }
    s->console_processor = processor;
}

static void mgmt_elsc_console_input(void *opaque, bool pending)
{
    ((SN0ModuleManagementState *)opaque)->console_input_pending = pending;
}

static bool mgmt_elsc_onewire_slot(void *opaque, uint32_t pulse,
                                   uint32_t sample)
{
    SN0ModuleManagementState *s = opaque;

    return sgi_onewire_slot(s->onewire_bus, pulse, sample);
}

static const SGIElscModuleOps mgmt_elsc_ops = {
    .can_execute = mgmt_elsc_can_execute,
    .execute = mgmt_elsc_execute,
    .slave_active = mgmt_elsc_slave_active,
    .console_owner = mgmt_elsc_console_owner,
    .console_input = mgmt_elsc_console_input,
    .onewire_slot = mgmt_elsc_onewire_slot,
};

/*
 * A module's one-wire identity pages, from its serial alone: page 0 is
 * the family byte plus the ten-character serial, page 1 the fixed
 * "MODULEID" name at offset sixteen, both CRC16-terminated. Fed by the
 * "model-serial" property's value, not by this device's own onewire-bus
 * responder above, which answers the module's separate serial chip.
 */
bool sgi_sn0_module_management_identity_pages(
    const char *serial, uint8_t page[2][DS2502_PAGE_SIZE])
{
    static const char name[] = "MODULEID";
    size_t i;

    if (strlen(serial) != SGI_O2000_MODULE_SERIAL_LEN) {
        return false;
    }
    for (i = 0; i < SGI_O2000_MODULE_SERIAL_LEN; i++) {
        if (!g_ascii_isprint(serial[i]) || serial[i] == ':' ||
            serial[i] == ';') {
            return false;
        }
    }

    memset(page, 0xff, 2 * DS2502_PAGE_SIZE);
    page[0][0] = 0x01;
    memcpy(&page[0][1], serial, SGI_O2000_MODULE_SERIAL_LEN);
    memcpy(&page[1][16], name, sizeof(name) - 1);
    for (i = 0; i < 2; i++) {
        uint16_t crc = ~onewire_crc16(0, page[i], DS2502_PAGE_SIZE - 2);

        page[i][DS2502_PAGE_SIZE - 2] = crc;
        page[i][DS2502_PAGE_SIZE - 1] = crc >> 8;
    }
    return true;
}

/*
 * The module responder's eight-byte one-wire ROM: family code 0x09, a
 * byte unique per node (0x08 plus slot), the module number folded into
 * the next byte's top nibble, three zero bytes, then a CRC8 self-check.
 * Matches the shipped module (slot 1-4, module number one) byte for
 * byte; the module-number fold is inferred from that one sample and not
 * independently confirmed against a second module number.
 */
void sgi_sn0_module_management_identity_rom(unsigned int slot,
                                            uint8_t module_number,
                                            uint8_t rom[8])
{
    rom[0] = 0x09;
    rom[1] = 0x08 + slot;
    rom[2] = rom[3] = rom[4] = rom[5] = 0x00;
    rom[6] = (module_number & 0x0f) << 4;
    rom[7] = onewire_crc8(0, rom, 7);
}

static const uint8_t module_serial_image_template[DS2502_DATA_SIZE] = {
    0x01, 0x4b, 0x4f, 0x52, 0x49, 0x47, 0x41, 0x4d,
    0x49, 0x30, 0x31, 0x4f, 0x52, 0x49, 0x47, 0x41,
    0x4d, 0x49, 0x2d, 0x4d, 0x4f, 0x44, 0x55, 0x4c,
    0x45, 0x2d, 0x53, 0x45, 0x52, 0x49, 0x80, 0x45,
    0x41, 0x4c, 0xff, 0xff, 0xff, 0xff, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x4d, 0x4f, 0x44, 0x55, 0x4c, 0x45, 0x49, 0x44,
    0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x0e, 0xcc,
    [64 ... DS2502_DATA_SIZE - 1] = 0xff,
};

static void mgmt_build_module_serial_image(SN0ModuleManagementState *s,
                                           uint8_t image[DS2502_DATA_SIZE])
{
    uint16_t crc;

    memcpy(image, module_serial_image_template, DS2502_DATA_SIZE);
    memcpy(&image[1], s->module_serial_text, SGI_O2000_MODULE_SERIAL_LEN);
    crc = ~onewire_crc16(0, image, 30);
    image[30] = crc;
    image[31] = crc >> 8;
}

static DeviceState *mgmt_add_child(SN0ModuleManagementState *s,
                                   const char *name, const char *type)
{
    DeviceState *child = qdev_new(type);

    object_property_add_child(OBJECT(s), name, OBJECT(child));
    object_unref(OBJECT(child));
    return child;
}

static void mgmt_populate_nvram(SN0ModuleManagementState *s)
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
    SN0ModuleManagementState *s = SGI_SN0_MODULE_MANAGEMENT(dev);
    uint8_t module_serial_image[DS2502_DATA_SIZE];
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

    if (i2c_bus_enable_wire(s->bus, SGI_O2000_WIRE_MASTERS, NULL, NULL,
                            errp)) {
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
    if (!s->module_serial_text ||
        strlen(s->module_serial_text) != SGI_O2000_MODULE_SERIAL_LEN) {
        error_setg(errp, "module management requires a %u-byte serial",
                   SGI_O2000_MODULE_SERIAL_LEN);
        return;
    }
    mgmt_build_module_serial_image(s, module_serial_image);
    ds2502_set_image(s->module_serial, module_serial_image,
                     sizeof(module_serial_image));
    if (!qdev_realize(s->module_serial, BUS(s->onewire_bus), errp)) {
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

    s->service_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    mgmt_timer_expired, s);
    s->message_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                    mgmt_timer_expired, s);
    s->pulse_timer = timer_new_ns(QEMU_CLOCK_VIRTUAL,
                                  mgmt_timer_expired, s);
    mgmt_reset_state(s);
    s->power_on = true;
    s->publication_enabled = true;
    mgmt_drive_power(s);
    s->service_deadline = qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                          SERVICE_PERIOD_NS;
    timer_mod_ns(s->service_timer, s->service_deadline);
    if (i2c_wire_set_timeline(s->bus, GRANT_MASTER, mgmt_next_deadline,
                              mgmt_run_deadlines, s)) {
        error_setg(errp, "cannot time the module controller on its wire");
        return;
    }

    /*
     * The coordinator sits on no bus, so the machine reset tree would miss it
     * and leave its scheduler and shared wire holding stale state while the
     * adapters around them reset.
     */
    qemu_register_resettable(OBJECT(s));
}

static void mgmt_unrealize(DeviceState *dev)
{
    SN0ModuleManagementState *s = SGI_SN0_MODULE_MANAGEMENT(dev);

    qemu_unregister_resettable(OBJECT(s));
    i2c_wire_clear_timeline(s->bus, GRANT_MASTER, mgmt_run_deadlines, s);
    i2c_wire_clear_observer(s->bus, GRANT_MASTER, mgmt_wire_event, s);
    timer_free(s->service_timer);
    timer_free(s->message_timer);
    timer_free(s->pulse_timer);
    s->service_timer = s->message_timer = s->pulse_timer = NULL;
}

static void mgmt_init(Object *obj)
{
    SN0ModuleManagementState *s = SGI_SN0_MODULE_MANAGEMENT(obj);

    s->bus = i2c_init_bus_type(TYPE_I2C_WIRE, DEVICE(obj), "i2c");
    s->onewire_bus = onewire_create_bus(DEVICE(obj), "module-onewire");
    s->module_serial = mgmt_add_child(s, "module-serial",
                                      TYPE_ONEWIRE_DS2502);
    qdev_prop_set_uint8(s->module_serial, "family", 0x09);
    qdev_prop_set_uint64(s->module_serial, "serial", UINT64_C(0x4d414749524f));
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
    SN0ModuleManagementState *s = SGI_SN0_MODULE_MANAGEMENT(obj);

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
    SN0ModuleManagementState *s = opaque;

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

static const VMStateDescription vmstate_sgi_sn0_module_management = {
    /* Pre-rename stream name; kept for migration compatibility. */
    .name = "sgi-o2000-module-management",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mgmt_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_INT64(service_deadline, SN0ModuleManagementState),
        VMSTATE_INT64(message_deadline, SN0ModuleManagementState),
        VMSTATE_INT64(pulse_deadline, SN0ModuleManagementState),
        VMSTATE_UINT16(recovery_ticks, SN0ModuleManagementState),
        VMSTATE_UINT8(processor_code, SN0ModuleManagementState),
        VMSTATE_UINT8(pending_messages, SN0ModuleManagementState),
        VMSTATE_UINT8(console_processor, SN0ModuleManagementState),
        VMSTATE_UINT8(grant_byte, SN0ModuleManagementState),
        VMSTATE_UINT8(grant_bit, SN0ModuleManagementState),
        VMSTATE_UINT8(grant_state, SN0ModuleManagementState),
        VMSTATE_BOOL(grant_bit_sampled, SN0ModuleManagementState),
        VMSTATE_BOOL(grant_active, SN0ModuleManagementState),
        VMSTATE_BOOL(wire_scl, SN0ModuleManagementState),
        VMSTATE_BOOL(wire_sda, SN0ModuleManagementState),
        VMSTATE_BOOL(publication_enabled, SN0ModuleManagementState),
        VMSTATE_BOOL(controller_request, SN0ModuleManagementState),
        VMSTATE_BOOL(controller_granted, SN0ModuleManagementState),
        VMSTATE_BOOL(serial_request, SN0ModuleManagementState),
        VMSTATE_BOOL(serial_granted, SN0ModuleManagementState),
        VMSTATE_BOOL(slave_active, SN0ModuleManagementState),
        VMSTATE_BOOL(console_input_pending, SN0ModuleManagementState),
        VMSTATE_BOOL(power_on, SN0ModuleManagementState),
        VMSTATE_BOOL(pulse_active, SN0ModuleManagementState),
        VMSTATE_BOOL(pulse_nmi, SN0ModuleManagementState),
        VMSTATE_END_OF_LIST()
    },
};

static const Property mgmt_properties[] = {
    DEFINE_PROP_UINT8("module-number", SN0ModuleManagementState,
                      module_number, 1),
    DEFINE_PROP_STRING("model-serial", SN0ModuleManagementState,
                       module_serial_text),
    DEFINE_PROP_BOOL("can-reset", SN0ModuleManagementState, can_reset, false),
    DEFINE_PROP_BOOL("can-nmi", SN0ModuleManagementState, can_nmi, false),
    DEFINE_PROP_BOOL("can-power", SN0ModuleManagementState, can_power, false),
};

static void mgmt_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI Origin 2000 shared module management";
    dc->realize = mgmt_realize;
    dc->unrealize = mgmt_unrealize;
    dc->vmsd = &vmstate_sgi_sn0_module_management;
    device_class_set_props(dc, mgmt_properties);
    /* A machine builds this complex and wires its outputs; -device cannot. */
    dc->user_creatable = false;
    rc->phases.hold = mgmt_reset_hold;
}

static const TypeInfo sgi_sn0_module_management_info = {
    .name = TYPE_SGI_SN0_MODULE_MANAGEMENT,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SN0ModuleManagementState),
    .instance_init = mgmt_init,
    .class_init = mgmt_class_init,
};

static void sgi_sn0_module_management_register_types(void)
{
    type_register_static(&sgi_sn0_module_management_info);
}

type_init(sgi_sn0_module_management_register_types)
