/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The SHub junk bus 16550, registers at a stride of 8 bytes. An IER write
 * that keeps THRI set toggles it, as the PROM's console code relies on.
 */

#include "qemu/osdep.h"
#include "hw/ia64/sgi-shub-uart.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"

#define REG_SHIFT 3
#define UART_IER 1
#define UART_IER_THRI 0x02
#define UART_LCR_DLAB 0x80

static uint64_t shub_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIShubUartState *s = SGI_SHUB_UART(opaque);

    return serial_io_ops.read(&s->serial, addr >> REG_SHIFT, 1);
}

static void shub_uart_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    SGIShubUartState *s = SGI_SHUB_UART(opaque);
    unsigned reg = addr >> REG_SHIFT;

    value &= 0xff;
    /* With the divisor latch open this offset is the divisor's high byte. */
    if (reg == UART_IER && !(s->serial.lcr & UART_LCR_DLAB) &&
        (value & UART_IER_THRI) && (s->serial.ier & UART_IER_THRI)) {
        serial_io_ops.write(&s->serial, reg, value & ~UART_IER_THRI, 1);
    }
    serial_io_ops.write(&s->serial, reg, value, 1);
}

static const MemoryRegionOps shub_uart_ops = {
    .read = shub_uart_read,
    .write = shub_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.max_access_size = 8,
    .impl.max_access_size = 8,
};

static void shub_uart_realize(DeviceState *dev, Error **errp)
{
    SGIShubUartState *s = SGI_SHUB_UART(dev);

    if (!qdev_realize(DEVICE(&s->serial), NULL, errp)) {
        return;
    }
    memory_region_init_io(&s->serial.io, OBJECT(dev), &shub_uart_ops, s,
                          "sgi-shub-uart", 8 << REG_SHIFT);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->serial.io);
    sysbus_init_irq(SYS_BUS_DEVICE(dev), &s->serial.irq);
}

static void shub_uart_init(Object *obj)
{
    SGIShubUartState *s = SGI_SHUB_UART(obj);

    object_initialize_child(obj, "serial", &s->serial, TYPE_SERIAL);
    qdev_alias_all_properties(DEVICE(&s->serial), obj);
}

static const VMStateDescription vmstate_shub_uart = {
    .name = "sgi-shub-uart",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT(serial, SGIShubUartState, 0, vmstate_serial,
                       SerialState),
        VMSTATE_END_OF_LIST()
    }
};

static void shub_uart_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = shub_uart_realize;
    dc->vmsd = &vmstate_shub_uart;
}

static const TypeInfo shub_uart_types[] = {
    {
        .name = TYPE_SGI_SHUB_UART,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIShubUartState),
        .instance_init = shub_uart_init,
        .class_init = shub_uart_class_init,
    },
};

DEFINE_TYPES(shub_uart_types)
