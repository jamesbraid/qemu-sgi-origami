/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * MACE 2.0 PS/2 frontend. The reviewed O2 PS/2 platform contract selects
 * atomic complete-byte transfers, not serial clock timing.
 */
#include "qemu/osdep.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/input/ps2.h"
#include "hw/mips/sgi/mace-ps2.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/module.h"
#include "qom/object.h"

OBJECT_DECLARE_SIMPLE_TYPE(MACEPS2State, SGI_MACE_PS2)

QEMU_BUILD_BUG_ON(sizeof(MACEPS2PortMigration) != 10);

struct MACEPS2State {
    SysBusDevice parent_obj;
    MemoryRegion mmio;
    PS2KbdState keyboard;
    PS2MouseState mouse;
    MACEPS2PortMigration port[2];
    qemu_irq output[4];
    void *rust_state;
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
        bool irq, poll;

        sgi_mace_ps2_outputs(s->rust_state, n, s->resetting, &irq, &poll);
        qemu_set_irq(s->output[2 * n], irq);
        qemu_set_irq(s->output[2 * n + 1], poll);
    }
}

static void mace_ps2_progress(MACEPS2State *s)
{
    if (s->pumping || s->resetting) {
        return;
    }
    s->pumping = true;
    for (unsigned n = 0; n < 2; n++) {
        PS2State *child = mace_ps2_child(s, n);

        for (;;) {
            uint8_t data;
            int action = sgi_mace_ps2_next_action(s->rust_state, n,
                                                   ps2_queue_empty(child), &data);

            switch (action) {
            case MACE_PS2_ACTION_POP_RX:
                data = ps2_read_data(child);
                sgi_mace_ps2_receive(s->rust_state, n, data, child);
                break;
            case MACE_PS2_ACTION_SEND_KEYBOARD:
                ps2_write_keyboard(&s->keyboard, data);
                sgi_mace_ps2_transmit_complete(s->rust_state, n, 0);
                break;
            case MACE_PS2_ACTION_SEND_MOUSE:
                ps2_write_mouse(&s->mouse, data);
                sgi_mace_ps2_transmit_complete(s->rust_state, n,
                                                child->queue.count);
                break;
            case MACE_PS2_ACTION_DROP_MOUSE_STREAM: {
                unsigned dropped = 0;

                while (!ps2_queue_empty(child)) {
                    ps2_read_data(child);
                    dropped++;
                }
                sgi_mace_ps2_stream_dropped(dropped);
                break;
            }
            default:
                goto port_done;
            }
        }
port_done:
        ;
    }
    s->pumping = false;
    mace_ps2_update(s);
}

static void mace_ps2_available(void *opaque, int n, int level)
{
    mace_ps2_progress(opaque);
}

static uint64_t mace_ps2_read(void *opaque, hwaddr addr, unsigned size)
{
    MACEPS2State *s = opaque;
    uint64_t value;

    if (sgi_mace_ps2_read(s->rust_state, addr, size, &value)) {
        mace_ps2_progress(s);
    }
    return value;
}

static void mace_ps2_write(void *opaque, hwaddr addr, uint64_t value,
                           unsigned size)
{
    MACEPS2State *s = opaque;

    if (sgi_mace_ps2_write(s->rust_state, addr, value, size)) {
        mace_ps2_progress(s);
    }
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
    sgi_mace_ps2_reset(s->rust_state);
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

    return sgi_mace_ps2_import(s->rust_state, s->port) ? 0 : -EINVAL;
}

static int mace_ps2_pre_save(void *opaque)
{
    MACEPS2State *s = opaque;

    sgi_mace_ps2_export(s->rust_state, s->port);
    return 0;
}

void sgi_mace_ps2_restore_post_load(DeviceState *dev)
{
    mace_ps2_progress(SGI_MACE_PS2(dev));
}

static bool mace_ps2_reply_needed(void *opaque)
{
    MACEPS2PortMigration *p = opaque;

    return p->reply != 0;
}

static const VMStateDescription vmstate_mace_ps2_port_reply = {
    .name = "sgi-mace-ps2/port/reply",
    .version_id = 1,
    .minimum_version_id = 1,
    .needed = mace_ps2_reply_needed,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT16(reply, MACEPS2PortMigration),
        VMSTATE_END_OF_LIST()
    },
};

static const VMStateDescription vmstate_mace_ps2_port = {
    .name = "sgi-mace-ps2/port",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8(control, MACEPS2PortMigration),
        VMSTATE_UINT8(tx, MACEPS2PortMigration),
        VMSTATE_UINT8(rx, MACEPS2PortMigration),
        VMSTATE_UINT8(errors, MACEPS2PortMigration),
        VMSTATE_BOOL(tx_pending, MACEPS2PortMigration),
        VMSTATE_BOOL(rx_full, MACEPS2PortMigration),
        VMSTATE_BOOL(inhibited, MACEPS2PortMigration),
        VMSTATE_END_OF_LIST()
    },
    .subsections = (const VMStateDescription * const []) {
        &vmstate_mace_ps2_port_reply,
        NULL
    },
};

static const VMStateDescription vmstate_mace_ps2 = {
    .name = TYPE_SGI_MACE_PS2,
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = mace_ps2_pre_save,
    .post_load = mace_ps2_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(port, MACEPS2State, 2, 0,
                             vmstate_mace_ps2_port, MACEPS2PortMigration),
        VMSTATE_END_OF_LIST()
    },
};

static void mace_ps2_realize(DeviceState *dev, Error **errp)
{
    MACEPS2State *s = SGI_MACE_PS2(dev);

    s->rust_state = sgi_mace_ps2_new();
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->keyboard), errp) ||
        !sysbus_realize(SYS_BUS_DEVICE(&s->mouse), errp)) {
        sgi_mace_ps2_free(s->rust_state);
        s->rust_state = NULL;
        return;
    }
    for (unsigned n = 0; n < 2; n++) {
        qdev_connect_gpio_out(DEVICE(mace_ps2_child(s, n)), PS2_DEVICE_IRQ,
                              qdev_get_gpio_in_named(dev, "available", n));
    }
}

static void mace_ps2_unrealize(DeviceState *dev)
{
    MACEPS2State *s = SGI_MACE_PS2(dev);

    sgi_mace_ps2_free(s->rust_state);
    s->rust_state = NULL;
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
    dc->unrealize = mace_ps2_unrealize;
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
