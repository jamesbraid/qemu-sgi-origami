/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/* Synthetic UART input/status for DMA tests, not a physical serial peer. */
#include "qemu/osdep.h"
#include "hw/char/serial.h"
#include "hw/mips/sgi/mace.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qapi/visitor.h"
#include "qemu/module.h"
#include "qemu/timer.h"
#include "qemu/units.h"
#include "system/qtest.h"

#define TYPE_SGI_SERIAL_PEER "sgi-serial-peer"
OBJECT_DECLARE_SIMPLE_TYPE(SGISerialPeer, SGI_SERIAL_PEER)

struct SGISerialPeer {
    DeviceState parent_obj;
    SerialState *uart;
    MemoryRegion *ram;
};

static void peer_set(Object *obj, Visitor *v, const char *name,
                      void *opaque, Error **errp)
{
    SGISerialPeer *s = SGI_SERIAL_PEER(obj);
    uint64_t value;
    uint8_t ier, lcr, byte;

    if (!visit_type_uint64(v, name, &value, errp)) {
        return;
    }
    if (!qtest_enabled() || !s->uart) {
        error_setg(errp, "serial peer requires qtest and a UART link");
        return;
    }
    if (!strcmp(name, "receive")) {
        byte = value;
        qemu_chr_be_write(s->uart->chr.chr, &byte, 1);
        return;
    }
    if (!strcmp(name, "line-errors")) {
        s->uart->lsr |= value & 0x1e;
    } else {
        s->uart->msr = value;
    }
    /* Recompute through the core, preserving bank and guest IRQ enables. */
    ier = s->uart->ier;
    lcr = s->uart->lcr;
    s->uart->lcr &= ~0x80;
    serial_io_ops.write(s->uart, 1, ier ^ 4, 1);
    serial_io_ops.write(s->uart, 1, ier, 1);
    s->uart->lcr = lcr;
    if (!strcmp(name, "modem-status-and-advance") ||
        !strcmp(name, "modem-transient-pause")) {
        /* Keep the service BH deferred while a UART event advances time. */
        qemu_clock_advance_virtual_time(qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL) +
                                        (value >> 8) * SCALE_MS);
        if (!strcmp(name, "modem-transient-pause")) {
            serial_io_ops.read(s->uart, 6, 1);
        }
    }
}

static bool peer_tx_blocked(Object *obj, Error **errp)
{
    SGISerialPeer *s = SGI_SERIAL_PEER(obj);

    return s->uart && s->uart->tsr_retry != 0;
}

static void peer_init(Object *obj)
{
    object_property_add_bool(obj, "transmit-blocked", peer_tx_blocked, NULL);
    object_property_add(obj, "receive", "uint64", NULL, peer_set, NULL, NULL);
    object_property_add(obj, "line-errors", "uint64", NULL, peer_set,
                        NULL, NULL);
    object_property_add(obj, "modem-status", "uint64", NULL, peer_set,
                        NULL, NULL);
    object_property_add(obj, "modem-status-and-advance", "uint64", NULL,
                        peer_set, NULL, NULL);
    object_property_add(obj, "modem-transient-pause", "uint64", NULL,
                        peer_set, NULL, NULL);
}

static void peer_realize(DeviceState *dev, Error **errp)
{
    SGISerialPeer *s = SGI_SERIAL_PEER(dev);

    if (s->ram) {
        if (!qtest_enabled() || !memory_region_is_ram(s->ram)) {
            error_setg(errp, "serial peer RAM requires qtest and RAM backing");
            return;
        }
        /* -M none leaves standalone RAM unregistered for migration. */
        vmstate_register_ram(s->ram, dev);
    }
}

static void peer_unrealize(DeviceState *dev)
{
    SGISerialPeer *s = SGI_SERIAL_PEER(dev);

    if (s->ram) {
        vmstate_unregister_ram(s->ram, dev);
    }
}

static const Property peer_properties[] = {
    DEFINE_PROP_LINK("uart", SGISerialPeer, uart, TYPE_SERIAL, SerialState *),
    DEFINE_PROP_LINK("ram", SGISerialPeer, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
};

static void peer_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = peer_realize;
    dc->unrealize = peer_unrealize;
    device_class_set_props(dc, peer_properties);
}

static const TypeInfo peer_type = {
    .name = TYPE_SGI_SERIAL_PEER,
    .parent = TYPE_DEVICE,
    .instance_size = sizeof(SGISerialPeer),
    .instance_init = peer_init,
    .class_init = peer_class_init,
};

static DeviceRealize mace_offset_parent_realize;

static void mace_offset_realize(DeviceState *dev, Error **errp)
{
    ERRP_GUARD();

    if (!qtest_enabled()) {
        error_setg(errp, "serial ring offset fixture requires qtest");
        return;
    }
    sgi_mace_enable_serial_ring_offsets(dev, errp);
    if (*errp) {
        return;
    }
    mace_offset_parent_realize(dev, errp);
}

static void mace_offset_class_init(ObjectClass *oc, const void *data)
{
    device_class_set_parent_realize(DEVICE_CLASS(oc), mace_offset_realize,
                                    &mace_offset_parent_realize);
}

static const TypeInfo mace_offset_type = {
    .name = "sgi-mace-serial-offset-test",
    .parent = TYPE_SGI_MACE,
    .class_init = mace_offset_class_init,
};

static void peer_register_types(void)
{
    type_register_static(&peer_type);
    type_register_static(&mace_offset_type);
}
type_init(peer_register_types)
