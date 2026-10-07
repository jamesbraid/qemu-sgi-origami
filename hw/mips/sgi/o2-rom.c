/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * MACE's 4 MiB system ROM decode over an explicitly selected diagnostic
 * image. Capacity, image placement and padding describe a test selection;
 * they do not identify an O2's fitted flash part or its programming layout.
 */
#include "qemu/osdep.h"
#include <glib/gstdio.h>
#include "hw/mips/sgi/o2-rom.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/resettable.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"
#include "qemu/units.h"

#define O2_ROM_WINDOW (4 * MiB)
#define O2_ROM_MIN_SIZE (512 * KiB)
#define O2_ROM_MAX_SIZE (2 * MiB)
#define O2_ROM_MAX_ALIASES (O2_ROM_WINDOW / O2_ROM_MIN_SIZE)

struct SGIO2ROMState {
    SysBusDevice parent_obj;

    MemoryRegion window;
    MemoryRegion storage;
    MemoryRegion aliases[O2_ROM_MAX_ALIASES - 1];
    uint32_t flash_size;
    uint32_t image_offset;
    uint16_t pad_byte;
    char *image;
    bool write_enabled;
};

void sgi_o2_rom_set_write_enable(SGIO2ROMState *rom, bool enabled)
{
    rom->write_enabled = enabled;
}

static bool o2_rom_get_write_enable(Object *obj, Error **errp)
{
    return SGI_O2_ROM(obj)->write_enabled;
}

static void o2_rom_set_write_enable(Object *obj, bool enabled, Error **errp)
{
    sgi_o2_rom_set_write_enable(SGI_O2_ROM(obj), enabled);
}

static MemTxResult o2_rom_read(void *opaque, hwaddr offset, uint64_t *value,
                               unsigned size, MemTxAttrs attrs)
{
    SGIO2ROMState *s = opaque;
    const uint8_t *bytes = memory_region_get_ram_ptr(&s->storage);

    *value = 0;
    for (unsigned i = 0; i < size; i++) {
        *value = (*value << 8) | bytes[offset + i];
    }
    return MEMTX_OK;
}

static MemTxResult o2_rom_write(void *opaque, hwaddr offset, uint64_t value,
                                unsigned size, MemTxAttrs attrs)
{
    SGIO2ROMState *s = opaque;

    if (size != 1) {
        qemu_log_mask(LOG_UNIMP,
                      "sgi-o2-rom: unsupported multi-byte flash write "
                      "(%u bytes) at offset 0x%" HWADDR_PRIx "\n",
                      size, offset);
        return MEMTX_ERROR;
    }
    if (!s->write_enabled) {
        return MEMTX_OK;
    }
    qemu_log_mask(LOG_UNIMP,
                  "sgi-o2-rom: unsupported enabled flash write of "
                  "0x%02" PRIx64 " at offset 0x%" HWADDR_PRIx "\n",
                  value & 0xff, offset);
    return MEMTX_ERROR;
}

static const MemoryRegionOps o2_rom_ops = {
    .read_with_attrs = o2_rom_read,
    .write_with_attrs = o2_rom_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

static void o2_rom_realize(DeviceState *dev, Error **errp)
{
    SGIO2ROMState *s = SGI_O2_ROM(dev);
    g_autofree char *contents = NULL;
    gsize length;
    GError *io_error = NULL;
    GStatBuf image_stat;
    uint8_t *bytes;

    if (s->flash_size != 512 * KiB && s->flash_size != 1 * MiB &&
        s->flash_size != 2 * MiB) {
        error_setg(errp, "sgi-o2-rom requires flash-size of 512 KiB, "
                   "1 MiB or 2 MiB");
        return;
    }
    if (!s->image || s->image_offset == UINT32_MAX ||
        s->pad_byte > UINT8_MAX) {
        error_setg(errp, "sgi-o2-rom requires image, image-offset and "
                   "pad-byte properties");
        return;
    }
    if (s->image_offset > s->flash_size) {
        error_setg(errp, "sgi-o2-rom image-offset exceeds flash-size");
        return;
    }
    if (g_stat(s->image, &image_stat) < 0) {
        error_setg_errno(errp, errno, "sgi-o2-rom cannot stat image '%s'",
                         s->image);
        return;
    }
    if (image_stat.st_size < 0 ||
        (uint64_t)image_stat.st_size > s->flash_size - s->image_offset) {
        error_setg(errp, "sgi-o2-rom image '%s' does not fit at offset "
                   "0x%x in a %u-byte device", s->image, s->image_offset,
                   s->flash_size);
        return;
    }
    if (!g_file_get_contents(s->image, &contents, &length, &io_error)) {
        error_setg(errp, "sgi-o2-rom cannot read image '%s': %s", s->image,
                   io_error->message);
        g_clear_error(&io_error);
        return;
    }
    if (length > s->flash_size - s->image_offset) {
        error_setg(errp, "sgi-o2-rom image '%s' does not fit at offset "
                   "0x%x in a %u-byte device", s->image, s->image_offset,
                   s->flash_size);
        return;
    }
    if (!memory_region_init_rom_device(&s->storage, OBJECT(s), &o2_rom_ops,
                                       s, "sgi-o2-rom.storage",
                                       s->flash_size, errp)) {
        return;
    }
    bytes = memory_region_get_ram_ptr(&s->storage);
    memset(bytes, s->pad_byte, s->flash_size);
    memcpy(bytes + s->image_offset, contents, length);
    memory_region_flush_rom_device(&s->storage, 0, s->flash_size);

    memory_region_init(&s->window, OBJECT(s), "sgi-o2-rom.window",
                       O2_ROM_WINDOW);
    memory_region_add_subregion(&s->window, 0, &s->storage);
    for (unsigned i = 1; i < O2_ROM_WINDOW / s->flash_size; i++) {
        g_autofree char *name = g_strdup_printf("sgi-o2-rom.alias%u", i);
        MemoryRegion *alias = &s->aliases[i - 1];

        memory_region_init_alias(alias, OBJECT(s), name, &s->storage,
                                 0, s->flash_size);
        memory_region_add_subregion(&s->window, i * s->flash_size, alias);
    }
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->window);
}

static void o2_rom_reset_enter(Object *obj, ResetType type)
{
    sgi_o2_rom_set_write_enable(SGI_O2_ROM(obj), false);
}

static void o2_rom_init(Object *obj)
{
    object_property_add_bool(obj, "write-enable", o2_rom_get_write_enable,
                             o2_rom_set_write_enable);
}

static const Property o2_rom_properties[] = {
    DEFINE_PROP_UINT32("flash-size", SGIO2ROMState, flash_size, 0),
    DEFINE_PROP_STRING("image", SGIO2ROMState, image),
    DEFINE_PROP_UINT32("image-offset", SGIO2ROMState, image_offset,
                       UINT32_MAX),
    DEFINE_PROP_UINT16("pad-byte", SGIO2ROMState, pad_byte, UINT16_MAX),
};

static const VMStateDescription vmstate_o2_rom = {
    .name = TYPE_SGI_O2_ROM,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT32_EQUAL(flash_size, SGIO2ROMState),
        VMSTATE_UINT32_EQUAL(image_offset, SGIO2ROMState),
        VMSTATE_UINT16_EQUAL(pad_byte, SGIO2ROMState),
        VMSTATE_BOOL(write_enabled, SGIO2ROMState),
        VMSTATE_END_OF_LIST()
    },
};

static void o2_rom_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "O2 MACE system ROM diagnostic view";
    dc->user_creatable = true;
    dc->realize = o2_rom_realize;
    dc->vmsd = &vmstate_o2_rom;
    device_class_set_props(dc, o2_rom_properties);
    rc->phases.enter = o2_rom_reset_enter;
}

static const TypeInfo o2_rom_info = {
    .name = TYPE_SGI_O2_ROM,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIO2ROMState),
    .instance_init = o2_rom_init,
    .class_init = o2_rom_class_init,
};

static void o2_rom_register_types(void)
{
    type_register_static(&o2_rom_info);
}

type_init(o2_rom_register_types)
