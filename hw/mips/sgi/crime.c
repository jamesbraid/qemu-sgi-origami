/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
/*
 * SGI CRIME 1.5 revision-1 processor interface and MACE interrupts.
 * An opt-in standalone profile decodes eight logical 128 MiB RAM banks.
 * ECC, timer operation, watchdog, graphics, GBE and VICE are not modeled.
 * Unsupported register operations fail visibly.
 */
#include "qemu/osdep.h"
#include "hw/mips/sgi/crime.h"
#include "hw/core/irq.h"
#include "hw/core/qdev-properties.h"
#include "migration/blocker.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"
#include "system/address-spaces.h"

#define CRIME_REGISTER_SIZE 0x2000000
#define CRIME_FITTED_RAM (64 * MiB)
#define CRIME_OPTIONAL_RAM (128 * MiB)
#define CRIME_BANK_SIZE (128 * MiB)
#define CRIME_SMALL_BANK_SIZE (32 * MiB)
#define CRIME_BANKED_RAM (1 * GiB)
#define CRIME_LOW_RAM_SIZE (256 * MiB)
#define CRIME_HIGH_RAM 0x20000000ULL
#define CRIME_HIGH_RAM_SIZE (512 * MiB)
#define CRIME_BANK_FIELDS 0x11fU
#define CRIME_BANK_64_MBIT 0x100U
#define CRIME_ERROR_READ (1U << 25)
#define CRIME_ERROR_WRITE (1U << 26)
#define CRIME_FULL_RAM 0x40000000ULL
#define CRIME_NO_ECC_RAM 0x80000000ULL
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

static void crime_update_bank_mapping(SGICrimeState *s)
{
    if (!s->alias_profile || !s->ram_aliases_initialized) {
        return;
    }

    memory_region_transaction_begin();
    for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
        memory_region_set_enabled(&s->bank_ram[bank], false);
        memory_region_set_enabled(&s->wrong_geometry[bank], false);
    }
    for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
        uint16_t control = s->bank_control[bank];

        if (control & CRIME_BANK_64_MBIT) {
            hwaddr address = ((control >> 2) & 7) * CRIME_BANK_SIZE;

            memory_region_set_address(&s->bank_ram[bank], address);
            memory_region_set_enabled(&s->bank_ram[bank], true);
        } else {
            hwaddr address = (control & 0x1f) * CRIME_SMALL_BANK_SIZE;

            memory_region_set_address(&s->wrong_geometry[bank], address);
            memory_region_set_enabled(&s->wrong_geometry[bank], true);
        }
    }
    memory_region_transaction_commit();
}

static MemTxResult crime_unsupported_geometry(void *opaque, hwaddr offset)
{
    const uint16_t *control = opaque;
    hwaddr address = (*control & 0x1f) * CRIME_SMALL_BANK_SIZE + offset;

    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-crime: unsupported 16 Mbit bank geometry at "
                  "RAM offset 0x%" HWADDR_PRIx "\n", address);
    return MEMTX_ERROR;
}

static MemTxResult crime_unsupported_geometry_read(void *opaque, hwaddr offset,
                                                   uint64_t *value,
                                                   unsigned size,
                                                   MemTxAttrs attrs)
{
    *value = 0;
    return crime_unsupported_geometry(opaque, offset);
}

static MemTxResult crime_unsupported_geometry_write(void *opaque,
                                                    hwaddr offset,
                                                    uint64_t value,
                                                    unsigned size,
                                                    MemTxAttrs attrs)
{
    return crime_unsupported_geometry(opaque, offset);
}

static const MemoryRegionOps crime_wrong_geometry_ops = {
    .read_with_attrs = crime_unsupported_geometry_read,
    .write_with_attrs = crime_unsupported_geometry_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

static MemTxResult crime_invalid_ram_access(SGICrimeState *s, hwaddr address,
                                             bool write)
{
    s->memory_error_status |= write ? CRIME_ERROR_WRITE : CRIME_ERROR_READ;
    s->memory_error_address = address & 0x3fffffff;
    qemu_log_mask(LOG_GUEST_ERROR,
                  "sgi-crime: unmatched RAM %s at offset 0x%" HWADDR_PRIx
                  "\n", write ? "write" : "read", address);
    return MEMTX_ERROR;
}

static MemTxResult crime_invalid_ram_read(void *opaque, hwaddr address,
                                          uint64_t *value, unsigned size,
                                          MemTxAttrs attrs)
{
    *value = 0;
    return crime_invalid_ram_access(opaque, address, false);
}

static MemTxResult crime_invalid_ram_write(void *opaque, hwaddr address,
                                           uint64_t value, unsigned size,
                                           MemTxAttrs attrs)
{
    return crime_invalid_ram_access(opaque, address, true);
}

static const MemoryRegionOps crime_invalid_ram_ops = {
    .read_with_attrs = crime_invalid_ram_read,
    .write_with_attrs = crime_invalid_ram_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8,
               .unaligned = true },
    .impl = { .min_access_size = 1, .max_access_size = 8,
              .unaligned = true },
};

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
        unsigned bank = (offset - 0x208) / 8;

        *value = s->eight_128m_banks ? s->bank_control[bank] : bank;
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
    case 0x0250:
        if (s->eight_128m_banks) {
            *value = s->memory_error_status;
            break;
        }
        goto unsupported;
    case 0x0258:
        if (s->eight_128m_banks) {
            *value = s->memory_error_address;
            break;
        }
        goto unsupported;
    default:
    unsupported:
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
    if (offset >= 0x208 && offset <= 0x240 && s->eight_128m_banks) {
        unsigned bank = (offset - 0x208) / 8;

        if (!(value & ~CRIME_BANK_FIELDS)) {
            s->bank_control[bank] = value;
            crime_update_bank_mapping(s);
            return MEMTX_OK;
        }
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
    s->memory_error_status = 0;
    s->memory_error_address = 0;
    for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
        s->bank_control[bank] = bank;
    }
}

static void crime_reset_hold(Object *obj, ResetType type)
{
    SGICrimeState *s = SGI_CRIME(obj);

    crime_update_bank_mapping(s);
    crime_update_irq(s);
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
    uint64_t ram_size;

    if (s->interface_revision != 1) {
        error_setg(errp, "sgi-crime supports only interface revision 1");
        return;
    }
    if (!s->ram) {
        if (s->eight_128m_banks) {
            error_setg(errp, "sgi-crime eight 128 MiB banks require RAM");
        }
        return;
    }
    ram_size = memory_region_size(s->ram);
    if (!memory_region_is_ram(s->ram) ||
        (s->eight_128m_banks && ram_size != CRIME_BANKED_RAM) ||
        (!s->eight_128m_banks && ram_size != CRIME_FITTED_RAM &&
         ram_size != CRIME_OPTIONAL_RAM)) {
        if (s->eight_128m_banks) {
            error_setg(errp, "sgi-crime requires a 1 GiB RAM region for "
                       "eight 128 MiB banks");
        } else {
            error_setg(errp, "sgi-crime requires a 64 or 128 MiB RAM region "
                       "for the synthetic fit");
        }
        return;
    }
    if (memory_region_is_rom(s->ram)) {
        error_setg(errp, "sgi-crime requires writable RAM");
        return;
    }
    if (s->ram_aliases_initialized &&
        (s->ram != s->alias_target ||
         s->eight_128m_banks != s->alias_profile)) {
        error_setg(errp, "sgi-crime RAM backing or bank profile cannot "
                   "change after realize");
        return;
    }
    if (s->eight_128m_banks) {
        error_setg(&s->migration_blocker,
                   "CRIME eight 128 MiB banks do not support migration");
        if (migrate_add_blocker(&s->migration_blocker, errp) < 0) {
            return;
        }
    }
    if (!s->ram_aliases_initialized) {
        MemoryRegion *target = s->ram;
        uint64_t size = ram_size;

        if (s->eight_128m_banks) {
            memory_region_init(&s->decoded_ram, OBJECT(dev),
                               "sgi-crime.decoded-ram", CRIME_BANKED_RAM);
            memory_region_init_io(&s->invalid_ram, OBJECT(dev),
                                  &crime_invalid_ram_ops, s,
                                  "sgi-crime.invalid-ram", CRIME_BANKED_RAM);
            memory_region_add_subregion_overlap(&s->decoded_ram, 0,
                                                &s->invalid_ram, -1);
            for (unsigned bank = 0; bank < SGI_CRIME_BANK_COUNT; bank++) {
                g_autofree char *name =
                    g_strdup_printf("sgi-crime.bank%u", bank);
                g_autofree char *guard_name =
                    g_strdup_printf("sgi-crime.bank%u-geometry", bank);

                memory_region_init_alias(&s->bank_ram[bank], OBJECT(dev),
                                         name, s->ram,
                                         bank * CRIME_BANK_SIZE,
                                         CRIME_BANK_SIZE);
                memory_region_set_enabled(&s->bank_ram[bank], false);
                memory_region_add_subregion_overlap(&s->decoded_ram, 0,
                                                    &s->bank_ram[bank],
                                                    SGI_CRIME_BANK_COUNT -
                                                    bank);
                memory_region_init_io(&s->wrong_geometry[bank], OBJECT(dev),
                                      &crime_wrong_geometry_ops,
                                      &s->bank_control[bank], guard_name,
                                      CRIME_SMALL_BANK_SIZE);
                memory_region_set_enabled(&s->wrong_geometry[bank], false);
                memory_region_add_subregion_overlap(
                    &s->decoded_ram, 0, &s->wrong_geometry[bank],
                    SGI_CRIME_BANK_COUNT - bank);
                s->bank_control[bank] = bank;
            }
            target = &s->decoded_ram;
            size = CRIME_BANKED_RAM;
            memory_region_init_alias(&s->high_ram, OBJECT(dev),
                                     "sgi-crime.high-ram", target,
                                     CRIME_HIGH_RAM_SIZE,
                                     CRIME_HIGH_RAM_SIZE);
        }
        memory_region_init_alias(&s->low_ram, OBJECT(dev),
                                 "sgi-crime.low-ram", target, 0,
                                 s->eight_128m_banks ?
                                 CRIME_LOW_RAM_SIZE : size);
        memory_region_init_alias(&s->full_ram, OBJECT(dev),
                                 "sgi-crime.full-ram", target, 0, size);
        memory_region_init_alias(&s->no_ecc_ram, OBJECT(dev),
                                 "sgi-crime.no-ecc-ram", target, 0, size);
        s->alias_target = s->ram;
        s->alias_profile = s->eight_128m_banks;
        s->ram_aliases_initialized = true;
    }
    memory_region_add_subregion(get_system_memory(), 0, &s->low_ram);
    if (s->eight_128m_banks) {
        memory_region_add_subregion(get_system_memory(), CRIME_HIGH_RAM,
                                    &s->high_ram);
    }
    memory_region_add_subregion(get_system_memory(), CRIME_FULL_RAM,
                                &s->full_ram);
    memory_region_add_subregion(get_system_memory(), CRIME_NO_ECC_RAM,
                                &s->no_ecc_ram);
    s->ram_aliases_mapped = true;
}

static void crime_unrealize(DeviceState *dev)
{
    SGICrimeState *s = SGI_CRIME(dev);

    if (s->ram_aliases_mapped) {
        memory_region_del_subregion(get_system_memory(), &s->no_ecc_ram);
        memory_region_del_subregion(get_system_memory(), &s->full_ram);
        if (s->alias_profile) {
            memory_region_del_subregion(get_system_memory(), &s->high_ram);
        }
        memory_region_del_subregion(get_system_memory(), &s->low_ram);
        s->ram_aliases_mapped = false;
    }
    migrate_del_blocker(&s->migration_blocker);
}

MemoryRegion *sgi_crime_get_decoded_ram(DeviceState *dev)
{
    SGICrimeState *s = SGI_CRIME(dev);

    if (!qdev_is_realized(dev) || !s->eight_128m_banks ||
        !s->ram_aliases_mapped) {
        return NULL;
    }
    return &s->decoded_ram;
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
    DEFINE_PROP_LINK("ram", SGICrimeState, ram, TYPE_MEMORY_REGION,
                     MemoryRegion *),
    DEFINE_PROP_BOOL("eight-128m-banks", SGICrimeState, eight_128m_banks,
                     false),
};

static void crime_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    ResettableClass *rc = RESETTABLE_CLASS(klass);

    dc->desc = "SGI CRIME 1.5 revision-1 processor interface";
    dc->realize = crime_realize;
    dc->unrealize = crime_unrealize;
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
