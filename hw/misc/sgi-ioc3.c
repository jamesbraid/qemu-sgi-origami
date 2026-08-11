/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * SGI IOC3 base I/O controller: the serial console first.
 *
 * The chip is one PCI device with a single base address register covering
 * everything it has, and the console lives in a window near the top of that
 * range which is unlike the rest of the chip. The SuperIO window at offset
 * 0x20000 holds byte-wide registers a single byte apart, where the chip's own
 * blocks are thirty-two bits wide. Getting that stride wrong is the most
 * likely way to produce a console that emits nothing, so the two ports declare
 * their access width explicitly rather than inheriting a default.
 *
 * Only the two serial ports are modelled here. The rest of the chip's window
 * is deliberately left as a hole, so an access to something unimplemented
 * reaches no device and is reported, rather than decoding and returning a
 * plausible zero. On a machine with one node, one processor and no I/O, zero
 * is the right answer to a great many questions, which is exactly what makes
 * answering all of them dangerous.
 *
 * The register file itself is QEMU's, wrapped rather than reimplemented. What
 * this file supplies is the machine's view of it: the byte stride, the byte
 * width, and the endianness.
 */

#include "qemu/osdep.h"
#include "qemu/log.h"
#include "qemu/units.h"
#include "qapi/error.h"
#include "hw/misc/sgi-ioc3.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "hw/core/sysbus.h"
#include "migration/vmstate.h"
#include "chardev/char-fe.h"

/*
 * The ports' access contract, which is the whole reason this wrapper exists
 * rather than using the register file's own region directly.
 *
 * Three things differ from what that region declares. The machine is
 * big-endian where the register file assumes the little-endian PCI I/O space
 * it was written for; the registers are one byte apart rather than at the
 * four-byte stride the chip's other blocks use; and an access wider than a
 * byte is refused.
 *
 * That last one is a choice, and the datasheet records it as a choice rather
 * than a finding. Nothing in the sources or in either guest says what the
 * hardware does with a wider access, and no correct driver issues one, so no
 * observation settles it. Refusing is the option that cannot be wrong quietly:
 * the memory system reports the attempt, and the report can only ever fire on
 * a real defect. Silently narrowing, which is what the register file's own
 * wrapper does, is the one option with nothing to recommend it, because it is
 * an unknown presented as known.
 *
 * The endianness is declared and then left alone. A big-endian processor
 * reaching a little-endian part needs its byte lanes placed, and both guests
 * expect that: one does it in its bus accessors, the other in its driver. The
 * device must not do its own lane arithmetic on top, because that cancels out
 * against one guest and doubles against the other, which is the worst failure
 * available here.
 */
static uint64_t sgi_ioc3_serial_read(void *opaque, hwaddr addr, unsigned size)
{
    return serial_io_ops.read(opaque, addr, 1);
}

static void sgi_ioc3_serial_write(void *opaque, hwaddr addr, uint64_t val,
                                  unsigned size)
{
    serial_io_ops.write(opaque, addr, val & 0xff, 1);
}

static const MemoryRegionOps sgi_ioc3_serial_ops = {
    .read = sgi_ioc3_serial_read,
    .write = sgi_ioc3_serial_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static const hwaddr sgi_ioc3_serial_base[SGI_IOC3_SERIAL_PORTS] = {
    SGI_IOC3_SERIAL_A, SGI_IOC3_SERIAL_B,
};

static const char *const sgi_ioc3_serial_name[SGI_IOC3_SERIAL_PORTS] = {
    "sgi-ioc3.serial-a", "sgi-ioc3.serial-b",
};

static void sgi_ioc3_realize(DeviceState *dev, Error **errp)
{
    SGIIOC3State *s = SGI_IOC3(dev);
    unsigned int i;

    memory_region_init(&s->bar, OBJECT(dev), "sgi-ioc3.bar",
                       SGI_IOC3_BAR_SIZE);

    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        SerialState *serial = &s->serial[i];

        if (!qdev_realize(DEVICE(serial), NULL, errp)) {
            return;
        }

        /*
         * Eight registers of one byte each, so the region is eight bytes wide
         * and the register file's own bound of eight offsets cannot be passed.
         */
        memory_region_init_io(&s->serial_io[i], OBJECT(dev),
                              &sgi_ioc3_serial_ops, serial,
                              sgi_ioc3_serial_name[i], 8);
        memory_region_add_subregion(&s->bar, sgi_ioc3_serial_base[i],
                                    &s->serial_io[i]);
    }

    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->bar);
}

static void sgi_ioc3_init(Object *obj)
{
    SGIIOC3State *s = SGI_IOC3(obj);
    unsigned int i;

    /*
     * The ports are children of the chip rather than devices in their own
     * right, because that is what they are: one chip, one base address
     * register, two register files inside it. Making them separate devices
     * would give each its own window and lose the containment that decides
     * where they answer.
     */
    for (i = 0; i < SGI_IOC3_SERIAL_PORTS; i++) {
        object_initialize_child(obj, "serial[*]", &s->serial[i], TYPE_SERIAL);
    }
}

static const VMStateDescription vmstate_sgi_ioc3 = {
    .name = "sgi-ioc3",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_STRUCT_ARRAY(serial, SGIIOC3State, SGI_IOC3_SERIAL_PORTS, 0,
                             vmstate_serial, SerialState),
        VMSTATE_END_OF_LIST()
    }
};

static const Property sgi_ioc3_properties[] = {
    DEFINE_PROP_CHR("chardev-a", SGIIOC3State, serial[0].chr),
    DEFINE_PROP_CHR("chardev-b", SGIIOC3State, serial[1].chr),
};

static void sgi_ioc3_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->desc = "SGI IOC3 base I/O controller";
    dc->realize = sgi_ioc3_realize;
    dc->vmsd = &vmstate_sgi_ioc3;
    device_class_set_props(dc, sgi_ioc3_properties);
}

static const TypeInfo sgi_ioc3_types[] = {
    {
        .name = TYPE_SGI_IOC3,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(SGIIOC3State),
        .instance_init = sgi_ioc3_init,
        .class_init = sgi_ioc3_class_init,
    },
};

DEFINE_TYPES(sgi_ioc3_types)
