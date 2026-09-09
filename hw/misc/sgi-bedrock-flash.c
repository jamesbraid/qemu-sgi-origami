/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 boot flash: adapts Bedrock boot-window cycles to a CFI device.
 *
 * Reads are one-to-one. Commands and programming arrive as 64-bit stores
 * carrying one 16-bit word zero-extended to 64 bits, the word at index n
 * addressed by the store at offset 8n, so a store is one two-byte cycle,
 * never two one-byte cycles. A value with anything above the word set is
 * outside that interface, so it is refused rather than masked down to fit.
 * Published documentation also describes a byte-write view putting byte n at
 * 8n; the firmware drives the 16-bit path modelled here, and a narrower
 * store is refused rather than guessed at. What a selection can command
 * follows from that transform and the view size; whether the hardware
 * commands the whole part through one selection is unresolved.
 *
 * Unknown, so not modelled: the fitted part, package count, how packages map
 * to the two selections, the chip-select equations, what an absent part
 * answers, and the decode outside the two selections. Both selections reach
 * one device so that firmware sees its own writes back through either one;
 * that is a compatibility choice, not a claim about the board. The CFI
 * device's identifiers are its defaults and name no part.
 */
#include "qemu/osdep.h"
#include "hw/misc/sgi-bedrock-flash.h"
#include "exec/memop.h"
#include "hw/block/flash.h"
#include "hw/core/qdev-properties.h"
#include "migration/vmstate.h"
#include "qapi/error.h"
#include "qemu/log.h"
#include "qemu/module.h"

/* AMD-compatible unlock cycles, as flash word addresses. */
#define FLASH_UNLOCK_ADDR0 0x555
#define FLASH_UNLOCK_ADDR1 0x2aa

static const char *const flash_selection_name[SGI_BEDROCK_FLASH_SELECTIONS] = {
    "primary", "alternate"
};

static MemTxResult flash_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    SGIBedrockFlashView *view = opaque;
    SGIBedrockFlashState *s = view->adapter;
    uint64_t high, low;
    MemTxResult result;

    switch (size) {
    case 1:
    case 2:
    case 4:
        return memory_region_dispatch_read(s->flash_mmio, offset, value,
                                           size_memop(size) | MO_BE, attrs);
    case 8:
        /* The flash answers at most four bytes per cycle. */
        result = memory_region_dispatch_read(s->flash_mmio, offset, &high,
                                             MO_32 | MO_BE, attrs);
        result |= memory_region_dispatch_read(s->flash_mmio, offset + 4, &low,
                                              MO_32 | MO_BE, attrs);
        *value = (high << 32) | (uint32_t)low;
        return result;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-flash: %s selection: %u-byte "
                      "read at 0x%" HWADDR_PRIx " is not a supported access "
                      "width\n", flash_selection_name[view->index], size,
                      offset);
        *value = 0;
        return MEMTX_ERROR;
    }
}

static MemTxResult flash_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    SGIBedrockFlashView *view = opaque;
    SGIBedrockFlashState *s = view->adapter;

    if (size != 8) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-flash: %s selection: %u-byte "
                      "write of 0x%" PRIx64 " at 0x%" HWADDR_PRIx " is "
                      "outside the 64-bit store the firmware uses; what the "
                      "hardware does with it is unknown\n",
                      flash_selection_name[view->index], size, value, offset);
        return MEMTX_ERROR;
    }
    if (value > UINT16_MAX) {
        /*
         * No cycle is issued, so the flash stays wherever the sequence had
         * reached and the next read may return status rather than data.
         */
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-flash: %s selection: "
                      "write of 0x%" PRIx64 " at 0x%" HWADDR_PRIx " sets bits "
                      "above the 16-bit flash word; refused without a flash "
                      "cycle\n", flash_selection_name[view->index], value,
                      offset);
        return MEMTX_ERROR;
    }

    /* The store at offset 8n carries flash word n, at flash byte 2n. */
    return memory_region_dispatch_write(s->flash_mmio, offset >> 2, value,
                                        MO_16 | MO_BE, attrs);
}

/*
 * Eight-byte accesses reach the handler whole, so a store is one command
 * rather than a run of narrower ones the firmware never issues.
 */
static const MemoryRegionOps flash_ops = {
    .read_with_attrs = flash_read,
    .write_with_attrs = flash_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 8 },
    .impl = { .min_access_size = 1, .max_access_size = 8 },
};

/*
 * Every byte, command and mode belongs to the CFI device, which brings its
 * own migration state and its own reset to read-array mode. The adapter
 * translates and holds nothing across either.
 */
static const VMStateDescription vmstate_flash = {
    .name = TYPE_SGI_BEDROCK_FLASH,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_END_OF_LIST()
    },
};

static void flash_realize(DeviceState *dev, Error **errp)
{
    SGIBedrockFlashState *s = SGI_BEDROCK_FLASH(dev);
    g_autofree char *drive = object_property_get_str(OBJECT(s->flash), "drive",
                                                     NULL);

    if (!drive || !drive[0]) {
        error_setg(errp, "sgi-bedrock-flash needs a %" PRIu64
                   " MiB raw flash image as its drive",
                   (uint64_t)(SGI_BEDROCK_FLASH_SIZE / MiB));
        return;
    }

    /*
     * One 16-bit bank rather than a pair of byte-wide ones: the firmware
     * hands over a whole flash word per bus cycle, and a byte-wide
     * configuration would take each of those as two separate commands.
     */
    qdev_prop_set_string(s->flash, "name", TYPE_SGI_BEDROCK_FLASH);
    qdev_prop_set_uint32(s->flash, "num-blocks",
                         SGI_BEDROCK_FLASH_SIZE /
                         SGI_BEDROCK_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint32(s->flash, "sector-length",
                         SGI_BEDROCK_FLASH_SECTOR_SIZE);
    qdev_prop_set_uint8(s->flash, "width", 2);
    /* Above one, the CFI device publishes an undispatchable container. */
    qdev_prop_set_uint8(s->flash, "mappings", 1);
    qdev_prop_set_uint8(s->flash, "big-endian", 1);
    qdev_prop_set_uint16(s->flash, "unlock-addr0", FLASH_UNLOCK_ADDR0);
    qdev_prop_set_uint16(s->flash, "unlock-addr1", FLASH_UNLOCK_ADDR1);

    /* Rejects a backing image that is not exactly the published capacity. */
    if (!sysbus_realize(SYS_BUS_DEVICE(s->flash), errp)) {
        return;
    }
    s->flash_mmio = sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash), 0);
}

static void flash_init(Object *obj)
{
    SGIBedrockFlashState *s = SGI_BEDROCK_FLASH(obj);

    s->flash = qdev_new(TYPE_PFLASH_CFI02);
    object_property_add_child(obj, "flash", OBJECT(s->flash));
    object_unref(OBJECT(s->flash));
    object_property_add_alias(obj, "drive", OBJECT(s->flash), "drive");

    for (unsigned i = 0; i < SGI_BEDROCK_FLASH_SELECTIONS; i++) {
        g_autofree char *name = g_strdup_printf("%s.%s",
                                                TYPE_SGI_BEDROCK_FLASH,
                                                flash_selection_name[i]);

        s->view[i].adapter = s;
        s->view[i].index = i;
        memory_region_init_io(&s->view[i].mmio, obj, &flash_ops, &s->view[i],
                              name, SGI_BEDROCK_FLASH_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->view[i].mmio);
    }
}

static void flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "SGI Origin 300 boot flash";
    dc->realize = flash_realize;
    dc->vmsd = &vmstate_flash;
    dc->user_creatable = true;
    set_bit(DEVICE_CATEGORY_STORAGE, dc->categories);
}

static const TypeInfo flash_info = {
    .name = TYPE_SGI_BEDROCK_FLASH,
    .parent = TYPE_SYS_BUS_DEVICE,
    .instance_size = sizeof(SGIBedrockFlashState),
    .instance_init = flash_init,
    .class_init = flash_class_init,
};

static void flash_register_types(void)
{
    type_register_static(&flash_info);
}

type_init(flash_register_types)
