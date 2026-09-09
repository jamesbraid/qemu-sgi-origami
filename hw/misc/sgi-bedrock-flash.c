/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 boot flash: adapts Bedrock boot-window cycles to a CFI device.
 *
 * Reads and writes decode differently, and at one address they disagree. An
 * ordinary read is relative to the software read address it lands in and is
 * one-to-one within it. A command or programming store is relative to the
 * enclosing 128 MiB boot space: it carries one 16-bit word zero-extended to
 * 64 bits, the word at backing byte n addressed by the store at window offset
 * 4n, so a store is one two-byte cycle, never two one-byte cycles. The second
 * software read address is a stride into the window, not a second write base,
 * so a read there sees backing byte zero while a store there reaches the
 * middle of the backing. A value with anything above the word set, a narrower
 * store, or an offset past the last backing word is outside the interface and
 * is refused rather than guessed at.
 *
 * Published documentation also describes a byte-write view putting byte n at
 * 8n, spanning the whole window; the firmware drives the 16-bit path modelled
 * here, which spans its first 64 MiB.
 *
 * Unknown, so not modelled: the fitted part, package count, how packages map
 * to the boot spaces, the chip-select equations, what an absent part answers,
 * and the read decode outside the mapped views. Every view reaching one
 * device is a compatibility choice, not a claim about the board, so that
 * firmware sees its own writes back through each of them. The CFI device's
 * identifiers are its defaults and name no part.
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

static const char *const flash_window_name[SGI_BEDROCK_FLASH_WINDOWS] = {
    "local", "remote"
};

/*
 * The read addresses this device answers for inside a boot space: the base
 * itself, and the second software address one stride above it. Both are
 * one-to-one over the backing.
 */
static bool flash_backing_read_offset(hwaddr offset, hwaddr *backing)
{
    if (offset < SGI_BEDROCK_FLASH_SIZE) {
        *backing = offset;
        return true;
    }
    if (offset >= SGI_BEDROCK_FLASH_SECOND_READ &&
        offset < SGI_BEDROCK_FLASH_SECOND_READ + SGI_BEDROCK_FLASH_SIZE) {
        *backing = offset - SGI_BEDROCK_FLASH_SECOND_READ;
        return true;
    }
    return false;
}

static MemTxResult flash_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    SGIBedrockFlashWindow *window = opaque;
    SGIBedrockFlashState *s = window->adapter;
    uint64_t high, low;
    MemTxResult result;
    hwaddr backing;

    if (!flash_backing_read_offset(offset, &backing)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-flash: %s boot space: read at "
                      "0x%" HWADDR_PRIx " is outside every mapped read "
                      "address; what answers there is unknown\n",
                      flash_window_name[window->index], offset);
        *value = 0;
        return MEMTX_ERROR;
    }

    switch (size) {
    case 1:
    case 2:
    case 4:
        return memory_region_dispatch_read(s->flash_mmio, backing, value,
                                           size_memop(size) | MO_BE, attrs);
    case 8:
        /* The flash answers at most four bytes per cycle. */
        result = memory_region_dispatch_read(s->flash_mmio, backing, &high,
                                             MO_32 | MO_BE, attrs);
        result |= memory_region_dispatch_read(s->flash_mmio, backing + 4, &low,
                                              MO_32 | MO_BE, attrs);
        *value = (high << 32) | (uint32_t)low;
        return result;
    default:
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-flash: %s boot space: %u-byte "
                      "read at 0x%" HWADDR_PRIx " is not a supported access "
                      "width\n", flash_window_name[window->index], size,
                      offset);
        *value = 0;
        return MEMTX_ERROR;
    }
}

static MemTxResult flash_write(void *opaque, hwaddr offset, uint64_t value,
                               unsigned size, MemTxAttrs attrs)
{
    SGIBedrockFlashWindow *window = opaque;
    SGIBedrockFlashState *s = window->adapter;

    if (size != 8) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-flash: %s boot space: %u-byte "
                      "write of 0x%" PRIx64 " at 0x%" HWADDR_PRIx " is "
                      "outside the 64-bit store the firmware uses; what the "
                      "hardware does with it is unknown\n",
                      flash_window_name[window->index], size, value, offset);
        return MEMTX_ERROR;
    }
    if (value > UINT16_MAX) {
        /*
         * No cycle is issued, so the flash stays wherever the sequence had
         * reached and the next read may return status rather than data.
         */
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-flash: %s boot space: "
                      "write of 0x%" PRIx64 " at 0x%" HWADDR_PRIx " sets bits "
                      "above the 16-bit flash word; refused without a flash "
                      "cycle\n", flash_window_name[window->index], value,
                      offset);
        return MEMTX_ERROR;
    }

    if (offset >= SGI_BEDROCK_FLASH_WRITE_LIMIT) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-flash: %s boot space: "
                      "store at 0x%" HWADDR_PRIx " is past the last backing "
                      "word; refused without a flash cycle\n",
                      flash_window_name[window->index], offset);
        return MEMTX_ERROR;
    }

    /* The store at window offset 4n carries the word at backing byte n. */
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

    for (unsigned i = 0; i < SGI_BEDROCK_FLASH_WINDOWS; i++) {
        g_autofree char *name = g_strdup_printf("%s.%s",
                                                TYPE_SGI_BEDROCK_FLASH,
                                                flash_window_name[i]);

        s->window[i].adapter = s;
        s->window[i].index = i;
        memory_region_init_io(&s->window[i].mmio, obj, &flash_ops,
                              &s->window[i], name,
                              SGI_BEDROCK_FLASH_WINDOW_SIZE);
        sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->window[i].mmio);
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
