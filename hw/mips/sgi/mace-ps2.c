/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * MACE 2.0 PS/2 frontend. The reviewed O2 PS/2 platform contract selects
 * atomic complete-byte transfers, not serial clock timing. Peripheral
 * commands and host input remain owned by QEMU's PS/2 children.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/input/ps2.h"
#include "hw/mips/sgi/mace-ps2.h"
#include "migration/vmstate.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(MACEPS2State, SGI_MACE_PS2)

struct MACEPS2State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    PS2KbdState keyboard;
    PS2MouseState mouse;
    MACEPS2Port port[2];
    qemu_irq output[4];
    bool pumping;
    bool resetting;
};

static PS2State *mace_ps2_child(MACEPS2State *s, unsigned n)
{
    return n ? PS2_DEVICE(&s->mouse) : PS2_DEVICE(&s->keyboard);
}

static void mace_ps2_update(MACEPS2State *s)
{
    for (unsigned n = 0; n < 2; n++) {
        MACEPS2Port *p = &s->port[n];
        bool ready = !(p->control & MACE_PS2_RESET) && !s->resetting;
        bool tx = ready && !p->tx_pending;
        bool rx = ready && p->rx_full;

        qemu_set_irq(s->output[2 * n],
                     (tx && (p->control & MACE_PS2_TXIE)) ||
                     (rx && (p->control & MACE_PS2_RXIE)));
        qemu_set_irq(s->output[2 * n + 1], tx || rx);
    }
}

static void mace_ps2_progress(MACEPS2State *s)
{
    if (s->pumping || s->resetting) {
        return;
    }
    /* Dequeue and command dispatch can synchronously change the child IRQ. */
    s->pumping = true;
    for (unsigned n = 0; n < 2; n++) {
        MACEPS2Port *p = &s->port[n];
        PS2State *child = mace_ps2_child(s, n);

        if (!mace_ps2_clock_permitted(p)) {
            continue;
        }
        if (p->tx_pending) {
            if (!(p->control & MACE_PS2_TXEN)) {
                continue;
            }
            if (n) {
                ps2_write_mouse(&s->mouse, p->tx);
            } else {
                ps2_write_keyboard(&s->keyboard, p->tx);
            }
            p->tx_pending = false;
            if (p->control & MACE_PS2_POST_TX) {
                p->inhibited = true;
            }
        }
        if (mace_ps2_clock_permitted(p) && !ps2_queue_empty(child)) {
            uint8_t data = ps2_read_data(child);

            mace_ps2_receive_frame(p, data, false, !(ctpop8(data) & 1), true);
        }
    }
    s->pumping = false;
    mace_ps2_update(s);
}

static void mace_ps2_available(void *opaque, int n, int level)
{
    MACEPS2State *s = opaque;

    /* Availability is derived from the child's queue, including after load. */
    mace_ps2_progress(s);
}

static uint64_t mace_ps2_read(void *opaque, hwaddr addr, unsigned size)
{
    MACEPS2State *s = opaque;
    MACEPS2Port *p = &s->port[(addr >> 5) & 1];
    uint64_t value;

    if (size != 8 || (addr & 7)) {
        goto unsupported;
    }
    switch (addr & 0x18) {
    case 8:
        if (p->rx_full) {
            value = mace_ps2_receive(p);
            mace_ps2_progress(s);
            return value;
        }
        return p->rx | ((uint64_t)mace_ps2_status(p) << 8);
    case 16:
        return p->control;
    case 24:
        return mace_ps2_status(p);
    }
unsupported:
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-mace-ps2: unsupported read "
                  "at 0x%" HWADDR_PRIx " size %u\n", addr, size);
    return 0;
}

static void mace_ps2_write(void *opaque, hwaddr addr, uint64_t value,
                            unsigned size)
{
    MACEPS2State *s = opaque;
    MACEPS2Port *p = &s->port[(addr >> 5) & 1];
    uint64_t writable;

    if (size != 8 || (addr & 7)) {
        goto unsupported;
    }
    switch (addr & 0x18) {
    case 0:
        if (p->tx_pending || (p->control & MACE_PS2_RESET)) {
            goto unsupported;
        }
        writable = 0xff;
        p->tx = value;
        p->tx_pending = true;
        break;
    case 16:
        writable = 0x3f;
        p->control = value & writable;
        if (p->control & MACE_PS2_RESET) {
            mace_ps2_clear(p);
        } else if (!(p->control & MACE_PS2_POST_TX)) {
            p->inhibited = false;
        }
        break;
    default:
        goto unsupported;
    }
    if (value & ~writable) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-mace-ps2: reserved write bits "
                      "at 0x%" HWADDR_PRIx " size %u value 0x%" PRIx64 "\n",
                      addr, size, value);
    }
    mace_ps2_progress(s);
    return;
unsupported:
    qemu_log_mask(LOG_GUEST_ERROR, "sgi-mace-ps2: unsupported write "
                  "at 0x%" HWADDR_PRIx " size %u value 0x%" PRIx64 "\n",
                  addr, size, value);
}

static const MemoryRegionOps mace_ps2_ops = {
    .read = mace_ps2_read,
    .write = mace_ps2_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8, .unaligned = true },
};

static void mace_ps2_reset_enter(Object *obj, ResetType type)
{
    MACEPS2State *s = SGI_MACE_PS2(obj);

    s->resetting = true;
    for (unsigned n = 0; n < 2; n++) {
        mace_ps2_clear(&s->port[n]);
        s->port[n].control = MACE_PS2_CLOCK;
    }
}

static void mace_ps2_reset_hold(Object *obj, ResetType type)
{
    mace_ps2_update(SGI_MACE_PS2(obj));
}

static void mace_ps2_reset_exit(Object *obj, ResetType type)
{
    MACEPS2State *s = SGI_MACE_PS2(obj);

    s->resetting = false;
    mace_ps2_progress(s);
}

static int mace_ps2_post_load(void *opaque, int version_id)
{
    MACEPS2State *s = opaque;

    for (unsigned n = 0; n < 2; n++) {
        MACEPS2Port *p = &s->port[n];

        if ((p->control & ~0x3f) || (p->errors & ~0xc0) ||
            (p->inhibited && !(p->control & MACE_PS2_POST_TX)) ||
            ((p->control & MACE_PS2_RESET) &&
             (p->tx_pending || p->rx_full || p->inhibited || p->errors))) {
            return -EINVAL;
        }
    }
    /* Child VMState loads first because the children realize first. */
    mace_ps2_progress(s);
    return 0;
}

static const VMStateDescription vmstate_mace_ps2_port = {
    .name = "sgi-mace-ps2/port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(control, MACEPS2Port),
        VMSTATE_UINT8(tx, MACEPS2Port),
        VMSTATE_UINT8(rx, MACEPS2Port),
        VMSTATE_UINT8(errors, MACEPS2Port),
        VMSTATE_BOOL(tx_pending, MACEPS2Port),
        VMSTATE_BOOL(rx_full, MACEPS2Port),
        VMSTATE_BOOL(inhibited, MACEPS2Port),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mace_ps2 = {
    .name = TYPE_SGI_MACE_PS2,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = mace_ps2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(port, MACEPS2State, 2, 0,
                             vmstate_mace_ps2_port, MACEPS2Port),
        VMSTATE_END_OF_LIST()
    },
};

static void mace_ps2_realize(DeviceState *dev, Error **errp)
{
    MACEPS2State *s = SGI_MACE_PS2(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->keyboard), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->mouse), errp)) {
        return;
    }
    for (unsigned n = 0; n < 2; n++) {
        qdev_connect_gpio_out(DEVICE(mace_ps2_child(s, n)), PS2_DEVICE_IRQ,
                              qdev_get_gpio_in_named(dev, "available", n));
    }
}

static void mace_ps2_init(Object *obj)
{
    MACEPS2State *s = SGI_MACE_PS2(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "keyboard", &s->keyboard, TYPE_PS2_KBD_DEVICE);
    object_initialize_child(obj, "mouse", &s->mouse, TYPE_PS2_MOUSE_DEVICE);
    memory_region_init_io(&s->mmio, obj, &mace_ps2_ops, s,
                          TYPE_SGI_MACE_PS2, 0x10000);
    sysbus_init_mmio(sbd, &s->mmio);
    qdev_init_gpio_in_named(DEVICE(obj), mace_ps2_available, "available", 2);
    qdev_init_gpio_out(DEVICE(obj), s->output, ARRAY_SIZE(s->output));
}

static void mace_ps2_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI MACE PS/2 byte transport";
    dc->realize = mace_ps2_realize;
    dc->vmsd = &vmstate_mace_ps2;
    dc->user_creatable = false;
    rc->phases.enter = mace_ps2_reset_enter;
    rc->phases.hold = mace_ps2_reset_hold;
    rc->phases.exit = mace_ps2_reset_exit;
}

static const TypeInfo mace_ps2_info = {
    .name = TYPE_SGI_MACE_PS2,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(MACEPS2State),
    .instance_init = mace_ps2_init,
    .class_init = mace_ps2_class_init,
};

static void mace_ps2_register_types(void)
{
    type_register_static(&mace_ps2_info);
}

type_init(mace_ps2_register_types)
