/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
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
    MemoryRegion heart_small;
    MemoryRegion heart_large;
    MemoryRegion heart_own_packet;
    MemoryRegion heart_large_wide;
    qemu_irq processor[HEART_PROCESSOR_LINES];
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
    SGIXioPeer heart;

    qdev_prop_set_uint8(DEVICE(&s->heart), "widget-revision", 4);
    qdev_prop_set_uint64(DEVICE(&s->heart), "bank0-size", 128 * MiB);
    qdev_prop_set_uint64(DEVICE(&s->heart), "bank1-size", 128 * MiB);
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->heart), errp)) {
        return;
    }
    qdev_init_gpio_in_named(dev, heart_test_processor, "processor-in",
                            HEART_PROCESSOR_LINES);
    for (unsigned i = 0; i < HEART_PROCESSOR_LINES; i++) {
        qdev_connect_gpio_out_named(DEVICE(&s->heart), "processor", i,
            qdev_get_gpio_in_named(dev, "processor-in", i));
    }

    qdev_prop_set_uint64(DEVICE(&s->xbow), "span", 16 * MiB);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "links-present", 0x8100);
    qdev_prop_set_uint16(DEVICE(&s->xbow), "startup-links-alive", 0x8100);
    /* Joined as a link joins it, so the Crossbow reports drops to HEART. */
    heart = (SGIXioPeer) {
        .port = OBJECT(&s->heart),
        .description = sgi_xio_port_describe(OBJECT(&s->heart), 0),
        .cpu_face = sgi_xio_port_cpu_face(OBJECT(&s->heart), 0),
        .packet_face = sgi_xio_port_packet_face(OBJECT(&s->heart), 0),
    };
    if (!sgi_xio_port_connect(OBJECT(&s->xbow), 8, &heart, errp) ||
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

    /*
     * The processor's own windows, entering the Crossbow at HEART's port:
     * all sixteen small windows, and widget 14's large window.
     */
    memory_region_init_alias(&s->heart_small, OBJECT(dev),
                             "heart-test.cpu-small",
                             sgi_xbow_ingress(&s->xbow, 8), 0, 16 * 16 * MiB);
    memory_region_init_alias(&s->heart_large, OBJECT(dev),
                             "heart-test.cpu-large",
                             sgi_xbow_packet_ingress(&s->xbow, 8), 14ULL << 48,
                             1ULL << 36);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->heart_small);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->heart_large);

    /*
     * Packets HEART sends from its own packet face: one naming its own
     * widget, and a window on widget 14 wide enough to carry offsets past
     * the 36 bits a large window holds.
     */
    memory_region_init_alias(&s->heart_own_packet, OBJECT(dev),
                             "heart-test.own-packet",
                             sgi_xbow_packet_ingress(&s->xbow, 8), 8ULL << 48,
                             0x1000);
    memory_region_init_alias(&s->heart_large_wide, OBJECT(dev),
                             "heart-test.cpu-large-wide",
                             sgi_xbow_packet_ingress(&s->xbow, 8), 14ULL << 48,
                             1ULL << 37);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->heart_own_packet);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->heart_large_wide);
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
    qdev_init_gpio_out_named(DEVICE(obj), s->processor, "processor",
                             HEART_PROCESSOR_LINES);
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
