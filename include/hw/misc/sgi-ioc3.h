/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 */

#ifndef HW_MISC_SGI_IOC3_H
#define HW_MISC_SGI_IOC3_H

#include "hw/core/irq.h"
#include "hw/core/sysbus.h"
#include "hw/char/serial.h"
#include "qom/object.h"

#define TYPE_SGI_IOC3 "sgi-ioc3"
OBJECT_DECLARE_SIMPLE_TYPE(SGIIOC3State, SGI_IOC3)

/*
 * One base address register covers the whole chip, and every offset in the
 * datasheet is measured from it. The highest thing in the map is the fourth
 * byte-bus device select at 0x0e0000 with a 32 KB window behind it, so a
 * megabyte is the smallest power of two that covers the chip.
 */
#define SGI_IOC3_BAR_SIZE       (1 * MiB)

/* The two serial ports, at their offsets in the byte-wide SuperIO window. */
#define SGI_IOC3_SERIAL_PORTS   2
#define SGI_IOC3_SERIAL_A       0x020178
#define SGI_IOC3_SERIAL_B       0x020170

/*
 * The SuperIO interrupt block, in the chip's main register window.
 *
 * The chip presents two interrupt outputs rather than one — an ethernet line
 * and a SuperIO line — and each has its own status and enable pair. This is the
 * SuperIO pair, and the register-level reason the two outputs are separate is
 * that this status register has no ethernet bit in it at all. Only the SuperIO
 * line is modelled: the ethernet block does not exist here.
 *
 * The enable mask has two write aliases over one register, so that a guest can
 * turn a source on or off without a read-modify-write.
 */
#define SGI_IOC3_INT_STATUS     0x1c
#define SGI_IOC3_INT_ENABLE_SET 0x20
#define SGI_IOC3_INT_ENABLE_CLR 0x24
#define SGI_IOC3_MAIN_SIZE      0x28

/*
 * The two serial ports' bits, which are pass-throughs: they follow the 16550's
 * own interrupt level rather than latching, so a write cannot clear them and
 * what really quiets one is the register read the guest's serial driver does.
 */
#define SGI_IOC3_INT_SERIAL_A   (1u << 6)
#define SGI_IOC3_INT_SERIAL_B   (1u << 15)
#define SGI_IOC3_INT_PASSTHROUGH (SGI_IOC3_INT_SERIAL_A | SGI_IOC3_INT_SERIAL_B)

struct SGIIOC3State {
    SysBusDevice parent_obj;

    /*
     * The chip's whole window. Everything modelled is a subregion of this, and
     * everything not modelled is a hole rather than a zero: an access to an
     * offset we have not implemented reaches no device and is reported, which
     * is what a guest asking an unanswerable question should get.
     */
    MemoryRegion bar;

    SerialState serial[SGI_IOC3_SERIAL_PORTS];
    MemoryRegion serial_io[SGI_IOC3_SERIAL_PORTS];

    /* The main window's interrupt registers, and the line they drive. */
    MemoryRegion main;
    qemu_irq superio_irq;
    uint32_t int_status;
    uint32_t int_enable;
};

#endif
