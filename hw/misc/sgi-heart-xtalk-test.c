/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Test-only Crossbow connection to a HEART endpoint at widget 8. */
#include "qemu/osdep.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "hw/misc/sgi-heart.h"
#include "hw/misc/sgi-xbow.h"
#include "hw/mips/sgi-xio-port.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define TYPE_SGI_HEART_XTALK_TEST "sgi-heart-xtalk-test"
OBJECT_DECLARE_SIMPLE_TYPE(SGIHeartXtalkTest, SGI_HEART_XTALK_TEST)

struct SGIHeartXtalkTest {
    SysBusDevice parent_obj;
    SGIHeartState heart;
    SGIXbowState xbow;
    MemoryRegion registers;
    MemoryRegion small;
    MemoryRegion large;
    MemoryRegion packet;
    MemoryRegion missing_packet;
    MemoryRegion ram_packet;
    qemu_irq processor[3];
};

static void heart_test_processor(void *opaque, int n, int level)
{
    SGIHeartXtalkTest *s = opaque;

    qemu_set_irq(s->processor[n], level);
}

static void heart_test_realize(DeviceState *dev, Error **errp)
{
    SGIHeartXtalkTest *s = SGI_HEART_XTALK_TEST(dev);
    MemoryRegion *cpu;
    MemoryRegion *packets;

    qdev_prop_set_uint8(DEVICE(&s->heart), "widget-revision", 4);
    qdev_prop_set_uint64(DEVICE(&s->heart), "bank0-size", 128 * MiB);
    qdev_prop_set_uint64(DEVICE(&s->heart), "bank1-size", 128 * MiB);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->heart), errp)) {
        return;
    }
    qdev_init_gpio_in_named(dev, heart_test_processor, "processor-in", 3);
    for (unsigned i = 0; i < 3; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->heart), "processor", i,
            qdev_get_gpio_in_named(dev, "processor-in", i));
    }

    qdev_prop_set_uint64(DEVICE(&s->xbow), "span", 16 * MiB);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "links-present", 0x8100);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "startup-links-alive", 0x8100);
    if (!sgi_xbow_attach_endpoint(&s->xbow, 8,
            sgi_xio_port_cpu_face(OBJECT(&s->heart), 0),
            sgi_xio_port_packet_face(OBJECT(&s->heart), 0), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->xbow), errp)) {
        return;
    }

    cpu = sgi_xbow_ingress(&s->xbow, 15);
    packets = sgi_xbow_packet_ingress(&s->xbow, 15);
    memory_region_init_alias(&s->registers, OBJECT(dev), "heart-test.regs",
                             sysbus_mmio_get_region(SYS_BUS_DEVICE(&s->heart),
                                                    0), 0, 0x60008);
    memory_region_init_alias(&s->small, OBJECT(dev), "heart-test.small",
                             cpu, 0, 16 * 16 * MiB);
    memory_region_init_alias(&s->large, OBJECT(dev), "heart-test.large",
                             cpu, 8 * 16 * MiB, 16 * MiB);
    memory_region_init_alias(&s->packet, OBJECT(dev), "heart-test.packet",
                             packets, 8ULL << 48, 0x1000);
    memory_region_init_alias(&s->missing_packet, OBJECT(dev),
                             "heart-test.missing-packet", packets,
                             9ULL << 48, 0x1000);
    memory_region_init_alias(&s->ram_packet, OBJECT(dev),
                             "heart-test.ram-packet", packets,
                             (8ULL << 48) + 0x20000000, 256 * MiB);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->registers);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->small);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->large);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->packet);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->missing_packet);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->ram_packet);
}

static const VMStateDescription vmstate_heart_test = {
    .name = TYPE_SGI_HEART_XTALK_TEST,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void heart_test_init(Object *obj)
{
    SGIHeartXtalkTest *s = SGI_HEART_XTALK_TEST(obj);

    object_initialize_child(obj, "heart", &s->heart, TYPE_SGI_HEART);
    object_initialize_child(obj, "xbow", &s->xbow, TYPE_SGI_XBOW);
    qdev_init_gpio_out_named(DEVICE(obj), s->processor, "processor", 3);
}

static void heart_test_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "HEART and Crossbow routing test fixture";
    dc->realize = heart_test_realize;
    dc->vmsd = &vmstate_heart_test;
    dc->user_creatable = true;
}

static const TypeInfo heart_test_info = {
    .name = TYPE_SGI_HEART_XTALK_TEST,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIHeartXtalkTest),
    .instance_init = heart_test_init,
    .class_init = heart_test_class_init,
};

static void heart_test_register_types(void)
{
    type_register_static(&heart_test_info);
}

type_init(heart_test_register_types)
