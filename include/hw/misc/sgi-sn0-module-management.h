/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 2000 module management: PCF8584 arbitration, one-wire identity and
 * shared NVRAM behind one module's node boards.
 */

#ifndef HW_MISC_SGI_SN0_MODULE_MANAGEMENT_H
#define HW_MISC_SGI_SN0_MODULE_MANAGEMENT_H

#include "hw/core/qdev.h"
#include "hw/core/irq.h"
#include "hw/i2c/i2c.h"
#include "hw/1wire/ds2502.h"
#include "hw/1wire/onewire.h"
#include "hw/misc/sgi-board-record.h"
#include "hw/misc/sgi-elsc.h"
#include "hw/nvram/sgi-elsc-nvram.h"
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
 * The module controller's configuration NVRAM (hw/nvram/sgi-elsc-nvram.h):
 * one 2048-byte store shared by the module's four Hubs, eight consecutive
 * slave addresses from 0x50, the top three bits of the eleven-bit byte
 * address choosing among them.
 */
#define SGI_O2000_NVRAM_FIRST_ADDR  SGI_ELSC_NVRAM_FIRST_ADDR
#define SGI_O2000_NVRAM_ADDRESSES   SGI_ELSC_NVRAM_BLOCKS
#define SGI_O2000_NVRAM_PAGE_BYTES  SGI_ELSC_NVRAM_BLOCK_BYTES
#define SGI_O2000_NVRAM_BYTES       SGI_ELSC_NVRAM_BYTES

/* The controller's own reserved region, and the fields a boot reads from it. */
#define SGI_O2000_NVRAM_RESERVED_PAGE   SGI_ELSC_NVRAM_CONTROLLER_BLOCK
#define SGI_O2000_NVRAM_VALIDITY_OFFSET 0
#define SGI_O2000_NVRAM_VALIDITY_VALUE  SGI_ELSC_NVRAM_MARKER_VALUE
#define SGI_O2000_NVRAM_MODULE_OFFSET   8

/* The logical MSC endpoint. */
#define SGI_O2000_MSC_ADDRESS       0x08

struct SN0ModuleManagementState {
    DeviceState parent_obj;

    /*
     * Model-derived identity. The module number also seeds the NVRAM's
     * module byte when no backing file supplies the store.
     */
    uint8_t module_number;
    /* The block node or drive backing the NVRAM, or NULL. */
    char *nvram_drive;

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
    /* The midplane one-wire net the controller's nic line drives. */
    OneWireBus *onewire_bus;

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
 * The MODULEID part's two pages, from the module's ten-character serial;
 * false for a serial that does not fit the record.
 */
bool sgi_sn0_module_management_identity_pages(
    const char *serial, uint8_t page[2][DS2502_PAGE_SIZE]);
/*
 * Fit the module's MODULEID part, ROM serial @identity_serial, and, unless
 * @midplane is NULL, its midplane part holding @midplane, ROM serial
 * @midplane_serial, on the controller's midplane net. Call after realize.
 */
bool sgi_sn0_module_management_attach_parts(SN0ModuleManagementState *s,
                                            const char *module_serial,
                                            uint64_t identity_serial,
                                            const SGIBoardRecord *midplane,
                                            uint64_t midplane_serial,
                                            Error **errp);
/* The midplane net, for wiring the n1 node's Hub line to it. */
OneWireBus *sgi_sn0_module_management_midplane_onewire(
    SN0ModuleManagementState *s);

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
 * false, on a module that declares no path for that action, and while a pulse
 * is already running: nothing settles that overlap.
 */
bool sgi_sn0_module_management_pulse_reset(SN0ModuleManagementState *s);
bool sgi_sn0_module_management_pulse_nmi(SN0ModuleManagementState *s);

#endif /* HW_MISC_SGI_SN0_MODULE_MANAGEMENT_H */
