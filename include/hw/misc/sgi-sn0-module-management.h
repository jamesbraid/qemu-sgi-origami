/*
 * SGI Origin 2000 shared module management
 *
 * Copyright (c) 2026 Origami contributors
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef HW_MISC_SGI_SN0_MODULE_MANAGEMENT_H
#define HW_MISC_SGI_SN0_MODULE_MANAGEMENT_H

#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/1wire/onewire.h"
#include "hw/misc/sgi-elsc.h"
#include "qemu/timer.h"
#include "qom/object.h"

#define TYPE_SGI_SN0_MODULE_MANAGEMENT "sgi-sn0-module-management"
OBJECT_DECLARE_SIMPLE_TYPE(SN0ModuleManagementState, SGI_SN0_MODULE_MANAGEMENT)

/* One adapter per node board on a four-node module. */
#define SGI_O2000_ADAPTER_COUNT     4
#define SGI_O2000_MODULE_SERIAL_LEN 10
/* The module controller drives the wire as the participant after them. */
#define SGI_O2000_WIRE_MASTERS      (SGI_O2000_ADAPTER_COUNT + 1)

/*
 * 2048 addressable bytes of management NVRAM reached across eight consecutive
 * slave addresses from 0x50, the top three bits of the eleven-bit byte address
 * choosing among them. Whether that is one part or several is unestablished.
 * The guest-visible logical space is the contract.
 *
 * The model builds it from eight 256-byte parts, which decides one thing the
 * contract does not: a sequential read running off the end of a page wraps to
 * the start of that same page, where one 2 KiB part occupying eight addresses
 * would carry on into the next.
 */
#define SGI_O2000_NVRAM_FIRST_ADDR  0x50
#define SGI_O2000_NVRAM_ADDRESSES   8
#define SGI_O2000_NVRAM_PAGE_BYTES  256
#define SGI_O2000_NVRAM_BYTES \
    (SGI_O2000_NVRAM_ADDRESSES * SGI_O2000_NVRAM_PAGE_BYTES)

/* The controller's own reserved region, and the fields a boot reads from it. */
#define SGI_O2000_NVRAM_RESERVED_PAGE   7
#define SGI_O2000_NVRAM_VALIDITY_OFFSET 0
#define SGI_O2000_NVRAM_VALIDITY_VALUE  0x37
#define SGI_O2000_NVRAM_MODULE_OFFSET   8

/* The logical MSC endpoint. */
#define SGI_O2000_MSC_ADDRESS       0x08

struct SN0ModuleManagementState {
    DeviceState parent_obj;

    /* Model-derived identity written into the reserved NVRAM region. */
    uint8_t module_number;
    char *module_serial_text;

    /*
     * Which destructive actions this installation can carry out. Each is off
     * until a machine declares it, and realize requires the declaration and
     * the matching output to arrive together, so an action with nowhere to
     * happen is refused rather than answered.
     */
    bool can_reset;
    bool can_nmi;
    bool can_power;

    I2CBus *bus;
    DeviceState *adapters[SGI_O2000_ADAPTER_COUNT];
    SGIElscState *msc;
    OneWireBus *onewire_bus;
    DeviceState *module_serial;

    qemu_irq reset_out;
    qemu_irq nmi_out;
    qemu_irq power_out;

    QEMUTimer *service_timer;
    QEMUTimer *message_timer;
    QEMUTimer *pulse_timer;
    int64_t service_deadline;
    int64_t message_deadline;
    int64_t pulse_deadline;

    uint16_t recovery_ticks;
    uint8_t processor_code;
    uint8_t pending_messages;   /* One bit per processor code. */
    uint8_t console_processor;
    uint8_t grant_byte;
    uint8_t grant_bit;
    uint8_t grant_state;
    bool grant_bit_sampled;
    bool grant_active;
    bool wire_scl;
    bool wire_sda;
    bool publication_enabled;
    bool controller_request;
    bool controller_granted;
    bool serial_request;
    bool serial_granted;
    bool slave_active;
    bool console_input_pending;
    bool power_on;
    bool pulse_active;
    bool pulse_nmi;
};

I2CBus *sgi_sn0_module_management_get_bus(SN0ModuleManagementState *s);
DeviceState *sgi_sn0_module_management_get_adapter(SN0ModuleManagementState *s,
                                               unsigned index);

/*
 * Controller-side requests the scheduler arbitrates against processor grants.
 * A request stays pending until the scheduler grants it; the caller releases
 * the grant when its transaction ends.
 */
void sgi_sn0_module_management_set_controller_request(
    SN0ModuleManagementState *s, bool pending);
void sgi_sn0_module_management_set_serial_request(SN0ModuleManagementState *s,
                                              bool pending);
void sgi_sn0_module_management_release_controller(SN0ModuleManagementState *s);
void sgi_sn0_module_management_release_serial(SN0ModuleManagementState *s);
bool sgi_sn0_module_management_controller_granted(SN0ModuleManagementState *s);
bool sgi_sn0_module_management_serial_granted(SN0ModuleManagementState *s);

void sgi_sn0_module_management_set_message(SN0ModuleManagementState *s,
                                       unsigned processor, bool pending);
void sgi_sn0_module_management_message_fetched(SN0ModuleManagementState *s,
                                           unsigned processor);
void sgi_sn0_module_management_set_publication(SN0ModuleManagementState *s,
                                           bool enabled);
/*
 * Raise or drop the module power line, answering false on a module that
 * declares no power path.
 */
bool sgi_sn0_module_management_set_power(SN0ModuleManagementState *s, bool on);
/*
 * Whether a destructive controller command can be carried out now. A command
 * this answers false for must be refused rather than acknowledged.
 */
bool sgi_sn0_module_management_can_execute(SN0ModuleManagementState *s,
                                       SGIElscAction action);
/*
 * Drive the reviewed hundred-millisecond pulse. Both refuse, and answer
 * false, on a module that declares no path for that action, and while a
 * pulse is already running: nothing settles that overlap.
 */
bool sgi_sn0_module_management_pulse_reset(SN0ModuleManagementState *s);
bool sgi_sn0_module_management_pulse_nmi(SN0ModuleManagementState *s);

#endif /* HW_MISC_SGI_SN0_MODULE_MANAGEMENT_H */
