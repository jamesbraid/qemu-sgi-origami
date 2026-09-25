/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * Origin 300 boot flash: adapts Bedrock boot-window cycles to two CFI devices.
 *
 * Each boot space selects two independently stateful 8 MiB PROM positions on
 * 32 MiB boundaries. Reads are one-to-one within the selected position. A
 * command or programming store is relative to that position: it carries one
 * 16-bit word zero-extended to 64 bits, and a store at position offset 4n
 * addresses the word at byte n. A store is one two-byte cycle, never two
 * one-byte cycles. A value with anything above the word set, a narrower store,
 * or an offset past the selected part is outside the interface and is refused.
 * The local and remote boot spaces reach the same pair, including each part's
 * command state.
 *
 * Published documentation also describes a byte-write view putting byte n at
 * 8n, spanning the whole window; the firmware drives the 16-bit path modelled
 * here. The two position-relative store regions together span its first
 * 64 MiB.
 *
 * Unknown, so not modelled: the fitted part, package and chip-select details,
 * what an absent part answers, and the read decode outside the mapped views.
 * The CFI devices' identifiers are their defaults and name no fitted part.
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
 * The read addresses this device answers inside a boot space: each PROM
 * position's base, one-to-one over that position's own 8 MiB. The second
 * position does not repeat the first; it holds the upper half of the backing.
 */
static bool flash_read_position(hwaddr offset, unsigned *position,
                                hwaddr *part)
{
    if (offset < SGI_BEDROCK_FLASH_PART_SIZE) {
        *position = 0;
        *part = offset;
        return true;
    }
    if (offset >= SGI_BEDROCK_FLASH_SECOND_READ &&
        offset < SGI_BEDROCK_FLASH_SECOND_READ + SGI_BEDROCK_FLASH_PART_SIZE) {
        *position = 1;
        *part = offset - SGI_BEDROCK_FLASH_SECOND_READ;
        return true;
    }
    return false;
}

/* A command or programming store selects by the stride it lands above. */
static void flash_store_position(hwaddr offset, unsigned *position,
                                 hwaddr *within)
{
    if (offset >= SGI_BEDROCK_FLASH_SECOND_READ) {
        *position = 1;
        *within = offset - SGI_BEDROCK_FLASH_SECOND_READ;
    } else {
        *position = 0;
        *within = offset;
    }
}

static MemTxResult flash_read(void *opaque, hwaddr offset, uint64_t *value,
                              unsigned size, MemTxAttrs attrs)
{
    SGIBedrockFlashWindow *window = opaque;
    SGIBedrockFlashState *s = window->adapter;
    uint64_t high, low;
    MemTxResult result;
    MemoryRegion *part_mmio;
    unsigned position;
    hwaddr part;

    if (!flash_read_position(offset, &position, &part)) {
        qemu_log_mask(LOG_UNIMP, "sgi-bedrock-flash: %s boot space: read at "
                      "0x%" HWADDR_PRIx " is outside every mapped read "
                      "address; what answers there is unknown\n",
                      flash_window_name[window->index], offset);
        *value = 0;
        return MEMTX_ERROR;
    }

    part_mmio = s->flash_mmio[position];
    switch (size) {
    case 1:
    case 2:
    case 4:
        return memory_region_dispatch_read(part_mmio, part, value,
                                           size_memop(size) | MO_BE, attrs);
    case 8:
        /* The flash answers at most four bytes per cycle. */
        result = memory_region_dispatch_read(part_mmio, part, &high,
                                             MO_32 | MO_BE, attrs);
        result |= memory_region_dispatch_read(part_mmio, part + 4, &low,
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
    unsigned position;
    hwaddr within;
    MemTxResult result;

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

    flash_store_position(offset, &position, &within);
    if (within >= SGI_BEDROCK_FLASH_WRITE_LIMIT) {
        qemu_log_mask(LOG_GUEST_ERROR, "sgi-bedrock-flash: %s boot space: "
                      "store at 0x%" HWADDR_PRIx " is past the last backing "
                      "word; refused without a flash cycle\n",
                      flash_window_name[window->index], offset);
        return MEMTX_ERROR;
    }

    /* The store at 4n above a position carries that part's word at byte n. */
    result = memory_region_dispatch_write(s->flash_mmio[position], within >> 2,
                                          value, MO_16 | MO_BE, attrs);
    /*
     * Every accepted cycle, not only ones that end up changing a stored byte:
     * telling an unlock or a command cycle from the one that actually writes
     * or erases would mean shadowing the CFI device's own AMD command state
     * machine here. The notified side tells them apart itself, from the
     * part's own dirty record, so an unlock or a command cycle that changed
     * nothing costs it an empty check rather than a copy.
     */
    if (result == MEMTX_OK && s->programmed) {
        s->programmed(s->programmed_opaque, position);
    }
    return result;
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

    for (unsigned i = 0; i < SGI_BEDROCK_FLASH_POSITIONS; i++) {
        g_autofree char *drive =
            object_property_get_str(OBJECT(s->flash[i]), "drive", NULL);
        g_autofree char *name = s->node ?
            g_strdup_printf("%s.node%u.prom%u", TYPE_SGI_BEDROCK_FLASH,
                            s->node, i) :
            g_strdup_printf("%s.prom%u", TYPE_SGI_BEDROCK_FLASH, i);

        if (!drive || !drive[0]) {
            error_setg(errp, "sgi-bedrock-flash needs a %" PRIu64
                       " MiB raw image as 'drive%u', the %s half of the "
                       "backing", (uint64_t)(SGI_BEDROCK_FLASH_PART_SIZE / MiB),
                       i, i == 0 ? "lower" : "upper");
            return;
        }

        /*
         * One 16-bit bank per position rather than a pair of byte-wide ones:
         * the firmware hands over a whole flash word per bus cycle, and a
         * byte-wide configuration would take each of those as two commands.
         */
        qdev_prop_set_string(s->flash[i], "name", name);
        qdev_prop_set_uint32(s->flash[i], "num-blocks",
                             SGI_BEDROCK_FLASH_PART_SIZE /
                             SGI_BEDROCK_FLASH_SECTOR_SIZE);
        qdev_prop_set_uint32(s->flash[i], "sector-length",
                             SGI_BEDROCK_FLASH_SECTOR_SIZE);
        qdev_prop_set_uint8(s->flash[i], "width", 2);
        /* Above one, the CFI device publishes an undispatchable container. */
        qdev_prop_set_uint8(s->flash[i], "mappings", 1);
        qdev_prop_set_uint8(s->flash[i], "big-endian", 1);
        qdev_prop_set_uint16(s->flash[i], "unlock-addr0", FLASH_UNLOCK_ADDR0);
        qdev_prop_set_uint16(s->flash[i], "unlock-addr1", FLASH_UNLOCK_ADDR1);

        /* Rejects a half that is not exactly one position's capacity. */
        if (!sysbus_realize(SYS_BUS_DEVICE(s->flash[i]), errp)) {
            return;
        }
        s->flash_mmio[i] =
            sysbus_mmio_get_region(SYS_BUS_DEVICE(s->flash[i]), 0);
    }
}

static void flash_init(Object *obj)
{
    SGIBedrockFlashState *s = SGI_BEDROCK_FLASH(obj);

    for (unsigned i = 0; i < SGI_BEDROCK_FLASH_POSITIONS; i++) {
        g_autofree char *child = g_strdup_printf("flash%u", i);
        g_autofree char *alias = g_strdup_printf("drive%u", i);

        s->flash[i] = qdev_new(TYPE_PFLASH_CFI02);
        object_property_add_child(obj, child, OBJECT(s->flash[i]));
        object_unref(OBJECT(s->flash[i]));
        object_property_add_alias(obj, alias, OBJECT(s->flash[i]), "drive");
    }

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

static const Property flash_properties[] = {
    DEFINE_PROP_UINT32("node", SGIBedrockFlashState, node, 0),
};

static void flash_class_init(ObjectClass *klass, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);

    dc->desc = "SGI Origin 300 boot flash";
    dc->realize = flash_realize;
    dc->vmsd = &vmstate_flash;
    device_class_set_props(dc, flash_properties);
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
