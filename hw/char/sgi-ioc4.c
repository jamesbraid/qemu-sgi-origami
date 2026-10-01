/*
 * SGI IOC4 revision 0x53, serial-console subset.
 *
 * The four byte-wide 16550A ports and their local interrupt controller are
 * modeled. ATA, serial DMA, keyboard/mouse, GPIO and byte-bus devices are not.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qapi/error.h"
#include "hw/char/serial.h"
#include "hw/core/irq.h"
#include "hw/core/resettable.h"
#include "hw/pci/pci_device.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"

#define TYPE_SGI_IOC4 "sgi-ioc4"
#define IOC4_PORTS 4
#define IOC4_BAR_SIZE 0x100000
#define IOC4_UART_BASE 0x380

typedef struct SGIIOC4State {
    PCIDevice parent_obj;
    MemoryRegion bar;
    SerialState uart[IOC4_PORTS];
    IRQState uart_irq[IOC4_PORTS];
    uint32_t serial_pending;
    uint32_t serial_enable;
    uint32_t other_enable;
    uint32_t uart_level;
    uint32_t clock_hz;
} SGIIOC4State;

typedef struct SGIIOC4Class {
    PCIDeviceClass parent_class;
    ResettablePhases parent_phases;
} SGIIOC4Class;

static void ioc4_update_irq(SGIIOC4State *s)
{
    pci_set_irq(&s->parent_obj,
                !!((s->serial_pending & s->serial_enable) |
                   (s->uart_level & s->serial_enable)));
}

static void ioc4_uart_irq(void *opaque, int n, int level)
{
    SGIIOC4State *s = opaque;
    uint32_t bit = 0x40u << (8 * n);

    if (level) {
        s->uart_level |= bit;
        s->serial_pending |= bit;
    } else {
        s->uart_level &= ~bit;
    }
    ioc4_update_irq(s);
}

static uint64_t ioc4_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC4State *s = opaque;

    switch (addr) {
    case 0x08:
        return s->serial_pending | s->uart_level;
    case 0x0c:
        return 0;
    case 0x10:
        return s->serial_enable;
    case 0x14:
        return s->other_enable;
    case 0x24:
        return s->clock_hz == 66666667;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: read of unsupported BAR offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void ioc4_write(void *opaque, hwaddr addr, uint64_t val,
                       unsigned size)
{
    SGIIOC4State *s = opaque;
    switch (addr) {
    case 0x08:
        s->serial_pending &= ~(uint32_t)val;
        break;
    case 0x0c:
        /* No other source is implemented. */
        break;
    case 0x10:
        s->serial_enable |= (uint32_t)val;
        break;
    case 0x14:
        s->other_enable |= (uint32_t)val;
        break;
    case 0x18:
        s->serial_enable &= ~(uint32_t)val;
        break;
    case 0x1c:
        s->other_enable &= ~(uint32_t)val;
        break;
    case 0x24:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: unsupported misc control write 0x%"
                      PRIx64 "\n", val);
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-ioc4: write to unsupported BAR offset 0x%"
                      HWADDR_PRIx "\n", addr);
        return;
    }
    ioc4_update_irq(s);
}

static const MemoryRegionOps ioc4_ops = {
    .read = ioc4_read,
    .write = ioc4_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 4, .max_access_size = 4,
               .unaligned = false },
    .impl = { .min_access_size = 4, .max_access_size = 4 },
};

static bool ioc4_uart_accepts(void *opaque, hwaddr addr, unsigned size,
                              bool is_write, MemTxAttrs attrs)
{
    if (size != 1) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc4: unsupported UART %s width %u at 0x%"
                      HWADDR_PRIx "\n", is_write ? "write" : "read",
                      size, addr);
        return false;
    }
    return true;
}

static uint64_t ioc4_uart_read(void *opaque, hwaddr addr, unsigned size)
{
    return serial_io_ops.read(opaque, addr, size);
}

static void ioc4_uart_write(void *opaque, hwaddr addr, uint64_t val,
                            unsigned size)
{
    serial_io_ops.write(opaque, addr, val, size);
}

static const MemoryRegionOps ioc4_uart_ops = {
    .read = ioc4_uart_read,
    .write = ioc4_uart_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    /* Preserve wide cycles until accepts can reject them. */
    .valid = { .min_access_size = 1, .max_access_size = 4,
               .unaligned = true, .accepts = ioc4_uart_accepts },
    .impl = { .min_access_size = 1, .max_access_size = 4,
              .unaligned = true },
};

static void ioc4_reset_hold(Object *obj, ResetType type)
{
    SGIIOC4State *s = (SGIIOC4State *)obj;
    SGIIOC4Class *ic = (SGIIOC4Class *)object_get_class(obj);
    int i;

    if (ic->parent_phases.hold) {
        ic->parent_phases.hold(obj, type);
    }

    s->serial_pending = 0;
    s->serial_enable = 0;
    s->other_enable = 0;
    s->uart_level = 0;
    for (i = 0; i < IOC4_PORTS; i++) {
        s->uart[i].baudbase = s->clock_hz / 16;
    }
    ioc4_update_irq(s);
}

static int ioc4_post_load(void *opaque, int version_id)
{
    SGIIOC4State *s = opaque;
    int i;

    for (i = 0; i < IOC4_PORTS; i++) {
        s->uart[i].baudbase = s->clock_hz / 16;
        serial_io_ops.write(&s->uart[i], 3, s->uart[i].lcr, 1);
    }
    ioc4_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_ioc4 = {
    .name = "sgi-ioc4",
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = ioc4_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_PCI_DEVICE(parent_obj, SGIIOC4State),
        VMSTATE_STRUCT_ARRAY(uart, SGIIOC4State, IOC4_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_UINT32(serial_pending, SGIIOC4State),
        VMSTATE_UINT32(serial_enable, SGIIOC4State),
        VMSTATE_UINT32(other_enable, SGIIOC4State),
        VMSTATE_UINT32(uart_level, SGIIOC4State),
        VMSTATE_UINT32(clock_hz, SGIIOC4State),
        VMSTATE_END_OF_LIST()
    }
};

static void ioc4_exit(PCIDevice *dev)
{
    SGIIOC4State *s = (SGIIOC4State *)dev;
    int i;

    for (i = 0; i < IOC4_PORTS; i++) {
        memory_region_del_subregion(&s->bar, &s->uart[i].io);
        qdev_unrealize(DEVICE(&s->uart[i]));
    }
}

static void ioc4_realize(PCIDevice *dev, Error **errp)
{
    SGIIOC4State *s = (SGIIOC4State *)dev;
    int i;

    if (s->clock_hz != 33333333 && s->clock_hz != 66666667) {
        error_setg(errp, "sgi-ioc4 clock-hz must be 33333333 or 66666667");
        return;
    }

    /* Local PCI IRQ output needs a pin; IO9 board wiring is not modeled. */
    dev->config[PCI_INTERRUPT_PIN] = 1;

    memory_region_init_io(&s->bar, OBJECT(s), &ioc4_ops, s, "sgi-ioc4",
                          IOC4_BAR_SIZE);
    for (i = 0; i < IOC4_PORTS; i++) {
        SerialState *uart = &s->uart[i];

        qdev_prop_set_uint32(DEVICE(uart), "baudbase", s->clock_hz / 16);
        if (!qdev_realize(DEVICE(uart), NULL, errp)) {
            while (i--) {
                memory_region_del_subregion(&s->bar, &s->uart[i].io);
                qdev_unrealize(DEVICE(&s->uart[i]));
            }
            return;
        }
        uart->irq = &s->uart_irq[i];
        memory_region_init_io(&uart->io, OBJECT(s), &ioc4_uart_ops, uart,
                              "sgi-ioc4-uart", 8);
        memory_region_add_subregion(&s->bar, IOC4_UART_BASE + 8 * i,
                                    &uart->io);
    }
    pci_register_bar(dev, 0, PCI_BASE_ADDRESS_SPACE_MEMORY, &s->bar);
}

static const Property ioc4_properties[] = {
    DEFINE_PROP_UINT32("clock-hz", SGIIOC4State, clock_hz, 33333333),
    DEFINE_PROP_CHR("chardev1", SGIIOC4State, uart[0].chr),
    DEFINE_PROP_CHR("chardev2", SGIIOC4State, uart[1].chr),
    DEFINE_PROP_CHR("chardev3", SGIIOC4State, uart[2].chr),
    DEFINE_PROP_CHR("chardev4", SGIIOC4State, uart[3].chr),
};

static void ioc4_instance_init(Object *obj)
{
    SGIIOC4State *s = (SGIIOC4State *)obj;
    int i;

    s->clock_hz = 33333333;
    for (i = 0; i < IOC4_PORTS; i++) {
        qemu_init_irq_child(obj, "uart-irq[*]", &s->uart_irq[i],
                            ioc4_uart_irq, s, i);
        object_initialize_child(obj, "uart[*]", &s->uart[i], TYPE_SERIAL);
    }
}

static void ioc4_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    PCIDeviceClass *pc = PCI_DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);
    SGIIOC4Class *ic = (SGIIOC4Class *)klass;

    pc->realize = ioc4_realize;
    pc->exit = ioc4_exit;
    pc->vendor_id = 0x10a9;
    pc->device_id = 0x100a;
    pc->revision = 0x53;
    pc->class_id = PCI_CLASS_OTHERS;
    dc->vmsd = &vmstate_ioc4;
    device_class_set_props(dc, ioc4_properties);
    resettable_class_set_parent_phases(rc, NULL, ioc4_reset_hold, NULL,
                                       &ic->parent_phases);
}

static const TypeInfo ioc4_info = {
    .name = TYPE_SGI_IOC4,
    .parent = TYPE_PCI_DEVICE,
    .instance_size = sizeof(SGIIOC4State),
    .class_size = sizeof(SGIIOC4Class),
    .instance_init = ioc4_instance_init,
    .class_init = ioc4_class_init,
    .interfaces = (const InterfaceInfo[]) {
        { INTERFACE_CONVENTIONAL_PCI_DEVICE },
        { },
    },
};

static void ioc4_register_types(void)
{
    type_register_static(&ioc4_info);
}
type_init(ioc4_register_types)
