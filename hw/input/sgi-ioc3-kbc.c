/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the PS/2 keyboard and mouse controller.
 *
 * A child of the multifunction chip (hw/misc/sgi-ioc3.c). The block is a
 * bit-level PS/2 line engine, not an 8042 clone: it wraps two generic ps2
 * devices and presents SGI's five-register interface -- a control/status
 * register, a receive register per port and a transmit register per port.
 * The project tree's IOC3 datasheet is the specification. See
 * docs/specs/sgi-ioc3.rst.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "exec/hwaddr.h"
#include "hw/core/sysbus.h"
#include "hw/core/irq.h"
#include "hw/input/sgi-ioc3-kbc.h"
#include "migration/vmstate.h"
#include "qapi/error.h"

/*
 * The controller's output is the data-available line, driven high while
 * either port holds bytes. The IOC3 latches this into interrupt-status bit
 * 22: the bit is set on a queued byte, held across a drain of the receive
 * registers, and cleared only when the guest writes a one to it. So this
 * line falls once both queues drain, but the latched bit 22 does not.
 */
static void sgi_ioc3_kbc_update_irq(SGIIOC3KBCState *s)
{
    int level = !ps2_queue_empty(PS2_DEVICE(&s->kbd)) ||
                !ps2_queue_empty(PS2_DEVICE(&s->mouse));

    qemu_set_irq(s->irq, level);
}

/* Each ps2 device raises its own line on queue and lowers it on drain. */
static void sgi_ioc3_kbc_set_irq(void *opaque, int n, int level)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(opaque);

    sgi_ioc3_kbc_update_irq(s);
}

/*
 * Pack up to three bytes drained from a port's queue, oldest in the highest
 * slot. Reading pops the queue, so the bytes and their flags clear next time.
 */
static uint32_t sgi_ioc3_kbc_receive(SGIIOC3KBCState *s, PS2State *ps2,
                                     bool aux)
{
    uint32_t val = 0;
    unsigned int i;
    unsigned int got = 0;

    for (i = 0; i < SGI_IOC3_KBC_RX_SLOTS; i++) {
        unsigned int slot = (SGI_IOC3_KBC_RX_SLOTS - 1) - i;

        if (ps2_queue_empty(ps2)) {
            break;
        }
        val |= SGI_IOC3_KBC_RX_DATA(slot, ps2_read_data(ps2));
        val |= SGI_IOC3_KBC_RX_VALID(slot);
        /*
         * The per-byte error flag (bit 24+slot) stays clear: the ps2 layer
         * delivers clean bytes, so no received byte is ever in error.
         */
        got++;
    }

    if (got && aux) {
        val |= SGI_IOC3_KBC_RX_SOURCE_AUX;
    }

    /*
     * The datasheet's overflow flag (bit 28) reports that a fourth byte
     * arrived and was lost. This model's ps2 queue is deep and drops
     * nothing: any surplus stays queued and the shared source stays raised,
     * so the guest reads it on the next interrupt. Nothing is ever lost, so
     * the flag is never set.
     */

    /*
     * Draining lowers this controller's data-available line once the queue is
     * empty; the latched interrupt-status bit 22 is unaffected.
     */
    sgi_ioc3_kbc_update_irq(s);
    return val;
}

static uint64_t sgi_ioc3_kbc_read(void *opaque, hwaddr addr, unsigned size)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(opaque);

    switch (addr) {
    case SGI_IOC3_KBC_CONTROL:
        /*
         * Write-pending and the other hardware-status bits read clear: a
         * synchronous transmit completes within the register write, so a
         * driver's post-transmit poll finishes on its first look.
         */
        return s->control;

    case SGI_IOC3_KBC_KBD_RECEIVE:
        return sgi_ioc3_kbc_receive(s, PS2_DEVICE(&s->kbd), false);

    case SGI_IOC3_KBC_AUX_RECEIVE:
        return sgi_ioc3_kbc_receive(s, PS2_DEVICE(&s->mouse), true);

    default:
        /* The transmit registers are write-only. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-kbc: read of write-only register 0x%"
                      HWADDR_PRIx "\n", addr);
        return 0;
    }
}

static void sgi_ioc3_kbc_write(void *opaque, hwaddr addr, uint64_t val,
                               unsigned size)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(opaque);

    switch (addr) {
    case SGI_IOC3_KBC_CONTROL:
        s->control = val & SGI_IOC3_KBC_CONTROL_MASK;
        return;

    case SGI_IOC3_KBC_KBD_TRANSMIT:
        ps2_write_keyboard(&s->kbd, val & 0xff);
        return;

    case SGI_IOC3_KBC_AUX_TRANSMIT:
        ps2_write_mouse(&s->mouse, val & 0xff);
        return;

    default:
        /* The receive registers are read-only. */
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-ioc3-kbc: write of 0x%" PRIx64 " to read-only "
                      "register 0x%" HWADDR_PRIx "\n", val, addr);
        return;
    }
}

static const MemoryRegionOps sgi_ioc3_kbc_ops = {
    .read = sgi_ioc3_kbc_read,
    .write = sgi_ioc3_kbc_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 4,
    .valid.max_access_size = 4,
    .impl.min_access_size = 4,
    .impl.max_access_size = 4,
};

static void sgi_ioc3_kbc_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(dev);

    if (!sysbus_realize(SYS_BUS_DEVICE(&s->kbd), errp)) {
        return;
    }
    if (!sysbus_realize(SYS_BUS_DEVICE(&s->mouse), errp)) {
        return;
    }

    /* Each port's data-available line feeds the one shared interrupt. */
    qdev_connect_gpio_out(DEVICE(&s->kbd), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-input", 0));
    qdev_connect_gpio_out(DEVICE(&s->mouse), PS2_DEVICE_IRQ,
                          qdev_get_gpio_in_named(dev, "ps2-input", 1));
}

static void sgi_ioc3_kbc_init(Object *obj)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(obj);
    SysBusDevice *sbd = SYS_BUS_DEVICE(obj);

    object_initialize_child(obj, "kbd", &s->kbd, TYPE_PS2_KBD_DEVICE);
    object_initialize_child(obj, "mouse", &s->mouse, TYPE_PS2_MOUSE_DEVICE);

    memory_region_init_io(&s->mmio, obj, &sgi_ioc3_kbc_ops, s, "sgi-ioc3.kbc",
                          SGI_IOC3_KBC_MMIO_SIZE);
    sysbus_init_mmio(sbd, &s->mmio);
    sysbus_init_irq(sbd, &s->irq);

    qdev_init_gpio_in_named(DEVICE(obj), sgi_ioc3_kbc_set_irq, "ps2-input", 2);
}

static void sgi_ioc3_kbc_reset_hold(Object *obj, ResetType type)
{
    SGIIOC3KBCState *s = SGI_IOC3_KBC(obj);

    s->control = 0;
}

static const VMStateDescription vmstate_sgi_ioc3_kbc = {
    .name = "sgi-ioc3-kbc",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(control, SGIIOC3KBCState),
        VMSTATE_END_OF_LIST()
    }
};

static void sgi_ioc3_kbc_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "SGI IOC3 PS/2 keyboard and mouse controller";
    dc->realize = sgi_ioc3_kbc_realize;
    dc->vmsd = &vmstate_sgi_ioc3_kbc;
    /* Instantiated only by the IOC3, which wires its bus and interrupt. */
    dc->user_creatable = false;
    rc->phases.hold = sgi_ioc3_kbc_reset_hold;
    set_bit(DEVICE_CATEGORY_INPUT, dc->categories);
}

static const TypeInfo sgi_ioc3_kbc_types[] = {
    {
        .name = TYPE_SGI_IOC3_KBC,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIIOC3KBCState),
        .instance_init = sgi_ioc3_kbc_init,
        .class_init = sgi_ioc3_kbc_class_init,
    },
};

DEFINE_TYPES(sgi_ioc3_kbc_types)
