/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller.
 */

#ifndef HW_MISC_SGI_IOC3_H
#define HW_MISC_SGI_IOC3_H

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
};

#endif
