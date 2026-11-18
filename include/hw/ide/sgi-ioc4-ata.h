/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The IDE core's register interface for the SGI IOC4's ATA channel.
 *
 * The IDE core's register functions are private to hw/ide, so the IOC4
 * reaches them through these.
 */

#ifndef HW_IDE_SGI_IOC4_ATA_H
#define HW_IDE_SGI_IOC4_ATA_H

#include "hw/core/irq.h"
#include "hw/ide/ide-bus.h"
#include "migration/vmstate.h"

/* Initialise BUS as a child of OWNER with two units, interrupting IRQ. */
void sgi_ioc4_ata_init(IDEBus *bus, DeviceState *owner, qemu_irq irq);
void sgi_ioc4_ata_reset(IDEBus *bus);

/*
 * Task-file register REG: 0 is the 16-bit data register, 1 to 7 the command
 * block registers (7 is status and command), 8 the control block (alternate
 * status and device control).
 */
uint32_t sgi_ioc4_ata_read(IDEBus *bus, unsigned reg);
void sgi_ioc4_ata_write(IDEBus *bus, unsigned reg, uint32_t val);

/* The bus and its two drives, for VMSTATE_STRUCT of an IDEBus field. */
extern const VMStateDescription vmstate_sgi_ioc4_ata;

#endif
