/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * SGI CRIME 1.5 revision-1 processor interface and MACE interrupts.
 * RAM population, ECC, timer operation, watchdog, graphics, GBE and VICE
 * are not modeled. Unsupported register operations fail visibly.
 */
#include "qemu/osdep.h"
#include "hw/mips/sgi/crime.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

#define CRIME_REGISTER_SIZE 0x2000000
#define CRIME_MACE_MASK 0xffffU
#define CRIME_SOFTWARE_MASK 0x30000000U
#define CRIME_ENABLE_MASK (CRIME_MACE_MASK | CRIME_SOFTWARE_MASK)
#define CRIME_IP2_MASK 0x0038U
#define CRIME_IP3_MASK 0xff07U
#define CRIME_IP4_MASK (1U << 28)
#define CRIME_IP5_MASK ((1U << 6) | (1U << 29))
#define CRIME_IP6_MASK (1U << 7)

static void crime_update_irq(SGICrimeState *s)
{
    uint32_t pending = (s->hardware | s->software) & s->enable;

    qemu_set_irq(s->cpu_ip[0], !!(pending & CRIME_IP2_MASK));
    qemu_set_irq(s->cpu_ip[1], !!(pending & CRIME_IP3_MASK));
    qemu_set_irq(s->cpu_ip[2], !!(pending & CRIME_IP4_MASK));
    qemu_set_irq(s->cpu_ip[3], !!(pending & CRIME_IP5_MASK));
    qemu_set_irq(s->cpu_ip[4], !!(pending & CRIME_IP6_MASK));
}

static void crime_mace_input(void *opaque, int n, int level)
{
    SGICrimeState *s = opaque;

    if (level) {
        s->hardware |= 1U << n;
    } else {
        s->hardware &= ~(1U << n);
    }
    crime_update_irq(s);
}

static MemTxResult crime_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    SGICrimeState *s = opaque;

    if (size != 8 || offset & 7) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-crime: invalid %u-byte read at 0x%" HWADDR_PRIx
                      "\n", size, offset);
        return MEMTX_ERROR;
    }
    if (offset >= 0x208 && offset <= 0x240) {
        *value = (offset - 0x208) / 8;
        return MEMTX_OK;
    }
    switch (offset) {
    case 0x0000:
        *value = 0xb1;
        break;
    case 0x0008:
        *value = 0x120;
        break;
    case 0x0010:
        *value = s->hardware | s->software;
        break;
    case 0x0018:
        *value = s->enable;
        break;
    case 0x0020:
        *value = s->software;
        break;
    case 0x0028:
        *value = s->hardware;
        break;
    case 0x0030: /* Watchdog remains disabled. */
    case 0x0038: /* Counter frequency and writes are not modeled. */
    case 0x0040: /* No CPU error producer is modeled. */
    case 0x0200: /* ECC control remains disabled. */
    case 0x4048: /* No CPU error producer is modeled. */
        *value = 0;
        break;
    default:
        qemu_log_mask(LOG_UNIMP,
                      "sgi-crime: unsupported read at 0x%" HWADDR_PRIx
                      "\n", offset);
        return MEMTX_ERROR;
    }
    return MEMTX_OK;
}

static MemTxResult crime_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    SGICrimeState *s = opaque;

    if (size != 8 || offset & 7) {
        qemu_log_mask(LOG_GUEST_ERROR,
                      "sgi-crime: invalid %u-byte write at 0x%" HWADDR_PRIx
                      "\n", size, offset);
        return MEMTX_ERROR;
    }
    switch (offset) {
    case 0x0018:
        if (!(value & ~CRIME_ENABLE_MASK)) {
            s->enable = value;
            crime_update_irq(s);
            return MEMTX_OK;
        }
        break;
    case 0x0020:
        if (!(value & ~CRIME_SOFTWARE_MASK)) {
            s->software = value;
            crime_update_irq(s);
            return MEMTX_OK;
        }
        break;
    default:
        break;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-crime: unsupported write at 0x%" HWADDR_PRIx
                  " value 0x%" PRIx64 "\n", offset, value);
    return MEMTX_ERROR;
}

static const MemoryRegionOps crime_ops = {
    .read_with_attrs = crime_read,
    .write_with_attrs = crime_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static void crime_reset_enter(Object *obj, ResetType type)
{
    SGICrimeState *s = SGI_CRIME(obj);

    s->enable = 0;
    s->software = 0;
}

static void crime_reset_hold(Object *obj, ResetType type)
{
    crime_update_irq(SGI_CRIME(obj));
}

static int crime_post_load(void *opaque, int version_id)
{
    SGICrimeState *s = opaque;

    if ((s->hardware & ~CRIME_MACE_MASK) ||
        (s->enable & ~CRIME_ENABLE_MASK) ||
        (s->software & ~CRIME_SOFTWARE_MASK)) {
        return -EINVAL;
    }
    crime_update_irq(s);
    return 0;
}

static const VMStateDescription vmstate_crime = {
    .name = TYPE_SGI_CRIME,
    .version_id = 1,
    .minimum_version_id = 1,
    .post_load = crime_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32(hardware, SGICrimeState),
        VMSTATE_UINT32(enable, SGICrimeState),
        VMSTATE_UINT32(software, SGICrimeState),
        VMSTATE_END_OF_LIST()
    },
};

static void crime_realize(DeviceState *dev, Error **errp)
{
    SGICrimeState *s = SGI_CRIME(dev);

    if (s->interface_revision != 1) {
        error_setg(errp, "sgi-crime supports only interface revision 1");
    }
}

static void crime_init(Object *obj)
{
    SGICrimeState *s = SGI_CRIME(obj);

    memory_region_init_io(&s->registers, obj, &crime_ops, s,
                          "sgi-crime.registers", CRIME_REGISTER_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->registers);
    qdev_init_gpio_in_named(DEVICE(obj), crime_mace_input, "mace", 16);
    qdev_init_gpio_out_named(DEVICE(obj), s->cpu_ip, "cpu-ip", 5);
}

static const Property crime_properties[] = {
    DEFINE_PROP_UINT8("interface-revision", SGICrimeState,
                      interface_revision, 1),
};

static void crime_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI CRIME 1.5 revision-1 processor interface";
    dc->realize = crime_realize;
    dc->vmsd = &vmstate_crime;
    dc->user_creatable = true;
    device_class_set_props(dc, crime_properties);
    rc->phases.enter = crime_reset_enter;
    rc->phases.hold = crime_reset_hold;
}

static const TypeInfo crime_info = {
    .name = TYPE_SGI_CRIME,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGICrimeState),
    .instance_init = crime_init,
    .class_init = crime_class_init,
};

static void crime_register_types(void)
{
    type_register_static(&crime_info);
}

type_init(crime_register_types)
