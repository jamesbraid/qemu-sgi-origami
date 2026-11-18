/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The IDE core's register interface for the SGI IOC4's ATA channel.
 */

#include "qemu/osdep.h"
#include "hw/ide/sgi-ioc4-ata.h"
#include "ide-internal.h"

void sgi_ioc4_ata_init(IDEBus *bus, DeviceState *owner, qemu_irq irq)
{
    ide_bus_init(bus, sizeof(*bus), owner, 0, 2);
    ide_bus_init_output_irq(bus, irq);
}

void sgi_ioc4_ata_reset(IDEBus *bus)
{
    ide_bus_reset(bus, IDE_RESET_HARDWARE);
}

uint32_t sgi_ioc4_ata_read(IDEBus *bus, unsigned reg)
{
    switch (reg) {
    case 0:
        return ide_data_readw(bus, 0);
    case 8:
        return ide_status_read(bus, 0);
    default:
        return ide_ioport_read(bus, reg);
    }
}

void sgi_ioc4_ata_write(IDEBus *bus, unsigned reg, uint32_t val)
{
    switch (reg) {
    case 0:
        ide_data_writew(bus, 0, val);
        break;
    case 8:
        ide_ctrl_write(bus, 0, val);
        break;
    default:
        ide_ioport_write(bus, reg, val);
    }
}

/*
 * Version 3: VMSTATE_IDE_DRIVES skips older descriptions. The bus entry is the
 * IDEBus itself, so its offset is zero.
 */
const VMStateDescription vmstate_sgi_ioc4_ata = {
    .name = "sgi-ioc4/ata-bus",
    .version_id = 3,
    .minimum_version_id = 3,
    .fields = (const VMStateField[]) {
        {
            .name = "bus",
            .version_id = 1,
            .size = sizeof(IDEBus),
            .vmsd = &vmstate_ide_bus,
            .flags = VMS_STRUCT,
            .offset = 0,
        },
        VMSTATE_IDE_DRIVES(ifs, IDEBus),
        VMSTATE_END_OF_LIST()
    }
};
