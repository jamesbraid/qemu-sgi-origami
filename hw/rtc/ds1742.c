/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * DS1742/DS1744 byte-wide timekeeping SRAM.
 * An optional drive holds the SRAM and calendar; a separate clock
 * reference drive holds an eight-byte magic and wall seconds in little
 * endian, letting the calendar advance across emulator restarts.
 * Host restart restores completed calendar state, not an unfinished W/R
 * transaction. The published specification does not define that power-fail
 * latch behavior.
 */
#include "qemu/osdep.h"
#include "qapi/error.h"
#include "block/block-io.h"
#include "hw/core/sysbus.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "qemu/bcd.h"
#include "qemu/bswap.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "qemu/log.h"
#include "qemu/timer.h"
#include "system/block-backend.h"
#include "system/rtc.h"
#include "system/runstate.h"

#define TYPE_DS1742 "ds1742"
#define TYPE_DS1744 "ds1744"
#define TYPE_DS1742_BASE "ds1742-base"
OBJECT_DECLARE_TYPE(DS1742State, DS1742Class, DS1742_BASE)
#define DS1742_SIZE 0x800
#define DS1742_CTRL 0x7f8
#define DS1744_SIZE 0x8000
#define DS1744_CTRL 0x7ff8
#define W 0x80
#define R 0x40
#define OSC 0x80
#define FT 0x40
#define BF 0x80
#define CLOCK_REF_BYTES 16
#define CLOCK_REF_MAGIC "DS1742R1"

struct DS1742State {
    SysBusDevice parent_obj;
    MemoryRegion iomem;
    Object *iomem_link;
    BlockBackend *blk, *clock_blk;
    VMChangeStateEntry *vmstate_change;
    Notifier shutdown;
    uint8_t ram[DS1744_CTRL];
    uint8_t control, weekday, migration_weekday, staged[8], frozen[8];
    int64_t epoch, reference_ns, migration_epoch;
    uint32_t fresh_year_base;
    bool stopped, frozen_valid;
    /* Equal writes retry until both backing records are saved. Host-only. */
    bool storage_failed;
    uint32_t size, ctrl;
    const char *name;
};

struct DS1742Class {
    SysBusDeviceClass parent_class;
    uint32_t size, ctrl;
    const char *name;
    const VMStateDescription *vmsd;
};

static int64_t ds1742_ns(void)
{
    return qemu_clock_get_ns(QEMU_CLOCK_VIRTUAL);
}

static int64_t ds1742_wall_seconds(void)
{
    return g_get_real_time() / G_USEC_PER_SEC;
}

static int64_t ds1742_seconds(DS1742State *s)
{
    return s->epoch + (s->stopped ? 0 :
        (ds1742_ns() - s->reference_ns) / NANOSECONDS_PER_SECOND);
}

static int64_t ds1742_day_index(int64_t seconds)
{
    return seconds >= 0 ? seconds / 86400 : (seconds + 1) / 86400 - 1;
}

static void ds1742_image(DS1742State *s, uint8_t image[8])
{
    int64_t now = ds1742_seconds(s);
    time_t stamp = now;
    struct tm tm;
    int weekday;

    gmtime_r(&stamp, &tm);
    weekday = ((s->weekday & 7) - 1 +
               ds1742_day_index(now) - ds1742_day_index(s->epoch)) % 7;
    if (weekday < 0) {
        weekday += 7;
    }
    image[0] = (s->control & 0xc0) |
               to_bcd((tm.tm_year + 1900) / 100);
    image[1] = to_bcd(tm.tm_sec) | (s->stopped ? OSC : 0);
    image[2] = to_bcd(tm.tm_min);
    image[3] = to_bcd(tm.tm_hour);
    image[4] = BF | (s->weekday & FT) | (weekday + 1);
    image[5] = to_bcd(tm.tm_mday);
    image[6] = to_bcd(tm.tm_mon + 1);
    image[7] = to_bcd((tm.tm_year + 1900) % 100);
}

static bool ds1742_drive_writable(BlockBackend *blk)
{
    BlockDriverState *bs = blk ? blk_bs(blk) : NULL;

    return bs && bdrv_is_writable(bs);
}

static void ds1742_save_clock(DS1742State *s)
{
    uint8_t image[DS1744_SIZE], record[CLOCK_REF_BYTES];

    if (!ds1742_drive_writable(s->blk)) {
        return;
    }
    if (s->control & (W | R)) {
        qemu_log_mask(LOG_UNIMP,
                      "ds1742: unfinished W/R transaction cannot be restored "
                      "from backing store\n");
    }
    /* Migrated RAM must reach the destination's backing store too. */
    memcpy(image, s->ram, s->ctrl);
    ds1742_image(s, image + s->ctrl);
    s->storage_failed = true;
    if (blk_pwrite(s->blk, 0, s->size, image, 0) < 0) {
        error_report("ds1742: could not save image to backing store");
        return;
    }
    if (s->clock_blk) {
        if (!ds1742_drive_writable(s->clock_blk)) {
            return;
        }
        memcpy(record, CLOCK_REF_MAGIC, 8);
        stq_le_p(record + 8, ds1742_wall_seconds());
        if (blk_pwrite(s->clock_blk, 0, sizeof(record), record, 0) < 0) {
            error_report("ds1742: could not save clock reference");
            return;
        }
    }
    s->storage_failed = false;
}

static bool ds1742_bcd(uint8_t val, int low, int high)
{
    return (val & 15) < 10 && (val >> 4) < 10 &&
        from_bcd(val) >= low && from_bcd(val) <= high;
}

static bool ds1742_valid_date(int year, int month, int day)
{
    static const uint8_t days_in_month[] = {
        31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31
    };
    bool leap = year % 4 == 0 && (year % 100 != 0 || year % 400 == 0);

    return day <= days_in_month[month - 1] + (month == 2 && leap);
}

static bool ds1742_commit(DS1742State *s, uint8_t century)
{
    struct tm tm = { 0 };

    if (!ds1742_bcd(century, 0, 39) ||
        !ds1742_bcd(s->staged[1] & 0x7f, 0, 59) ||
        !ds1742_bcd(s->staged[2], 0, 59) ||
        !ds1742_bcd(s->staged[3], 0, 23) ||
        !ds1742_bcd(s->staged[4] & 7, 1, 7) ||
        !ds1742_bcd(s->staged[5], 1, 31) ||
        !ds1742_bcd(s->staged[6], 1, 12) ||
        !ds1742_bcd(s->staged[7], 0, 99)) {
        qemu_log_mask(LOG_GUEST_ERROR, "ds1742: invalid calendar write\n");
        return false;
    }
    tm.tm_year = from_bcd(century) * 100 + from_bcd(s->staged[7]) - 1900;
    tm.tm_mon = from_bcd(s->staged[6]) - 1;
    tm.tm_mday = from_bcd(s->staged[5]);
    if (!ds1742_valid_date(tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday)) {
        qemu_log_mask(LOG_UNIMP,
                      "ds1742: unsupported calendar date %04d-%02d-%02d\n",
                      tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return false;
    }
    tm.tm_hour = from_bcd(s->staged[3]);
    tm.tm_min = from_bcd(s->staged[2]);
    tm.tm_sec = from_bcd(s->staged[1] & 0x7f);
    s->epoch = mktimegm(&tm);
    s->reference_ns = ds1742_ns();
    s->weekday = s->staged[4] & (FT | 7);
    s->stopped = s->staged[1] & OSC;
    return true;
}

static void ds1742_restore_clock(DS1742State *s, const uint8_t image[8])
{
    uint8_t record[CLOCK_REF_BYTES];
    int64_t saved_at, elapsed = 0;

    memcpy(s->staged, image, 8);
    if (!ds1742_commit(s, image[0] & 0x3f)) {
        qemu_log_mask(LOG_UNIMP,
                      "ds1742: invalid saved calendar; using RTC seed\n");
        return;
    }
    s->control = image[0] & 0x3f;
    if (!s->stopped && s->clock_blk &&
        blk_pread(s->clock_blk, 0, sizeof(record), record, 0) >= 0 &&
        !memcmp(record, CLOCK_REF_MAGIC, 8)) {
        saved_at = ldq_le_p(record + 8);
        elapsed = MAX(ds1742_wall_seconds() - saved_at, 0);
    } else if (!s->stopped) {
        qemu_log_mask(LOG_UNIMP,
                      "ds1742: no clock reference; restored time cannot advance\n");
    }
    if (elapsed) {
        int64_t days = ds1742_day_index(s->epoch + elapsed) -
                       ds1742_day_index(s->epoch);

        s->weekday = (s->weekday & FT) |
                     (((s->weekday & 7) - 1 + days % 7) % 7 + 1);
        s->epoch += elapsed;
    }
}

static uint64_t ds1742_read(void *opaque, hwaddr addr, unsigned size)
{
    DS1742State *s = opaque;
    uint8_t image[8], value;

    if (addr < s->ctrl) {
        return s->ram[addr];
    }
    if (addr == s->ctrl) {
        if (s->control & W) {
            return s->control;
        }
        if (s->frozen_valid) {
            return (s->control & 0xc0) | (s->frozen[0] & 0x3f);
        }
        ds1742_image(s, image);
        return image[0];
    }
    if (s->control & W) {
        value = s->staged[addr - s->ctrl];
    } else if (s->frozen_valid) {
        value = s->frozen[addr - s->ctrl];
    } else {
        ds1742_image(s, image);
        value = image[addr - s->ctrl];
    }
    if (addr == s->ctrl + 4) {
        value |= BF;
    }
    if (addr == s->ctrl + 1 && (s->weekday & FT) && !s->stopped) {
        value = (value & ~1) | ((ds1742_ns() / 976562) & 1);
    }
    return value;
}

static void ds1742_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    DS1742State *s = opaque;
    uint8_t value = val, old, image[8];

    if (addr < s->ctrl) {
        if (s->ram[addr] == value && !s->storage_failed) {
            return;
        }
        s->ram[addr] = value;
        if (s->blk && blk_pwrite(s->blk, addr, 1, &value, 0) < 0) {
            s->storage_failed = true;
            error_report("ds1742: could not save SRAM byte at 0x%03x",
                         (unsigned)addr);
        }
        return;
    }
    if (addr != s->ctrl) {
        if (s->control & W) {
            s->staged[addr - s->ctrl] = value;
        } else {
            qemu_log_mask(LOG_GUEST_ERROR,
                          "ds1742: calendar write with W clear at 0x%03x\n",
                          (unsigned)addr);
        }
        return;
    }
    if (!(s->control & (W | R))) {
        ds1742_image(s, image);
        s->control = image[0];
    }
    old = s->control;
    if ((value & R) && !(old & R)) {
        ds1742_image(s, s->frozen);
        s->frozen_valid = true;
    } else if (!(value & R)) {
        s->frozen_valid = false;
    }
    if ((value & W) && !(old & W)) {
        ds1742_image(s, s->staged);
    }
    if (!(value & W) && (old & W)) {
        bool valid = ds1742_commit(s, value & 0x3f);

        s->control = (value & 0xc0) |
                     (valid ? value & 0x3f : old & 0x3f);
        if (valid) {
            ds1742_save_clock(s);
        }
        return;
    }
    /* The published control rules preserve century when W or R is set. */
    s->control = (value & 0xc0) | (old & 0x3f);
}

static bool ds1742_accepts(void *opaque, hwaddr addr, unsigned size,
                           bool is_write, MemTxAttrs attrs)
{
    return size == 1;
}

static const MemoryRegionOps ds1742_ops = {
    .read = ds1742_read,
    .write = ds1742_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    /* Let address-space writes reach the rejection check before splitting. */
    .valid.max_access_size = 8,
    .valid.accepts = ds1742_accepts,
    .valid.unaligned = true,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
    .impl.unaligned = true,
};

static void ds1742_vm_state_change(void *opaque, bool running,
                                   RunState state)
{
    if (!running) {
        ds1742_save_clock(opaque);
    }
}

static void ds1742_shutdown_notifier(Notifier *notifier, void *data)
{
    DS1742State *s = container_of(notifier, DS1742State, shutdown);

    ds1742_save_clock(s);
}

static bool ds1742_shift_fresh_seed(DS1742State *s, struct tm *tm,
                                     Error **errp)
{
    int year = tm->tm_year + 1900;

    if (!s->fresh_year_base) {
        return true;
    }
    if (year < (int)s->fresh_year_base ||
        year > (int)s->fresh_year_base + 99) {
        error_setg(errp,
                   "ds1742: fresh-year-base=%u cannot seed RTC year %d",
                   s->fresh_year_base, year);
        return false;
    }
    /* The guest sees the year relative to fresh_year_base. Keep the weekday
     * from the unshifted seed, even when the mapped Gregorian year differs. */
    tm->tm_year = 2000 + (year - (int)s->fresh_year_base) % 100 - 1900;
    s->epoch = mktimegm(tm);
    s->control = to_bcd((tm->tm_year + 1900) / 100);
    return true;
}

static void ds1742_realize(DeviceState *dev, Error **errp)
{
    DS1742State *s = DS1742_BASE(dev);
    uint8_t image[8];
    struct tm tm;
    int64_t length;

    if (s->fresh_year_base != 0 && s->fresh_year_base != 1968) {
        error_setg(errp, "ds1742: fresh-year-base must be 0 or 1968");
        return;
    }
    qemu_get_timedate(&tm, 0);
    s->epoch = mktimegm(&tm);
    s->reference_ns = ds1742_ns();
    s->control = to_bcd((tm.tm_year + 1900) / 100);
    s->weekday = tm.tm_wday + 1;
    if (!s->blk && !ds1742_shift_fresh_seed(s, &tm, errp)) {
        return;
    }
    if (s->clock_blk && !s->blk) {
        error_setg(errp, "ds1742: clock-drive requires drive");
        return;
    }
    if (s->blk) {
        length = blk_getlength(s->blk);
        if (length != s->size) {
            error_setg(errp, "%s: backing drive must be exactly %u bytes",
                       s->name, s->size);
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (s->clock_blk) {
            length = blk_getlength(s->clock_blk);
            if (length < CLOCK_REF_BYTES) {
                error_setg(errp,
                           "ds1742: clock reference drive must be at least %u bytes",
                           CLOCK_REF_BYTES);
                return;
            }
            if (blk_set_perm(s->clock_blk,
                             BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                             BLK_PERM_ALL, errp) < 0) {
                return;
            }
        }
        if (blk_pread(s->blk, 0, s->ctrl, s->ram, 0) < 0 ||
            blk_pread(s->blk, s->ctrl, sizeof(image), image, 0) < 0) {
            error_setg(errp, "ds1742: could not read backing drive");
            return;
        }
        if (image[1] | image[2] | image[3] | image[5] | image[6] | image[7]) {
            ds1742_restore_clock(s, image);
        } else if (!ds1742_shift_fresh_seed(s, &tm, errp)) {
            return;
        }
        s->vmstate_change =
            qemu_add_vm_change_state_handler(ds1742_vm_state_change, s);
        s->shutdown.notify = ds1742_shutdown_notifier;
        qemu_register_shutdown_notifier(&s->shutdown);
    }
}

static void ds1742_unrealize(DeviceState *dev)
{
    DS1742State *s = DS1742_BASE(dev);

    if (s->vmstate_change) {
        ds1742_save_clock(s);
        qemu_del_vm_change_state_handler(s->vmstate_change);
        s->vmstate_change = NULL;
        notifier_remove(&s->shutdown);
    }
}

static int ds1742_pre_save(void *opaque)
{
    DS1742State *s = opaque;
    uint8_t image[8];

    ds1742_image(s, image);
    s->migration_weekday = image[4] & 7;
    s->migration_epoch = ds1742_seconds(s);
    return 0;
}

static int ds1742_post_load(void *opaque, int version_id)
{
    DS1742State *s = opaque;

    s->epoch = s->migration_epoch;
    s->reference_ns = ds1742_ns();
    s->weekday = (s->weekday & FT) | s->migration_weekday;
    return 0;
}

static const VMStateDescription vmstate_ds1742 = {
    .name = "ds1742",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = ds1742_pre_save,
    .post_load = ds1742_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_SUB_ARRAY(ram, DS1742State, 0, DS1742_CTRL),
        VMSTATE_UINT8(control, DS1742State),
        VMSTATE_UINT8(weekday, DS1742State),
        VMSTATE_UINT8(migration_weekday, DS1742State),
        VMSTATE_UINT8_ARRAY(staged, DS1742State, 8),
        VMSTATE_UINT8_ARRAY(frozen, DS1742State, 8),
        VMSTATE_INT64(migration_epoch, DS1742State),
        VMSTATE_BOOL(stopped, DS1742State),
        VMSTATE_BOOL(frozen_valid, DS1742State),
        VMSTATE_END_OF_LIST()
    }
};

static const VMStateDescription vmstate_ds1744 = {
    .name = "ds1744",
    .version_id = 1,
    .minimum_version_id = 1,
    .pre_save = ds1742_pre_save,
    .post_load = ds1742_post_load,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(ram, DS1742State, DS1744_CTRL),
        VMSTATE_UINT8(control, DS1742State),
        VMSTATE_UINT8(weekday, DS1742State),
        VMSTATE_UINT8(migration_weekday, DS1742State),
        VMSTATE_UINT8_ARRAY(staged, DS1742State, 8),
        VMSTATE_UINT8_ARRAY(frozen, DS1742State, 8),
        VMSTATE_INT64(migration_epoch, DS1742State),
        VMSTATE_BOOL(stopped, DS1742State),
        VMSTATE_BOOL(frozen_valid, DS1742State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property ds1742_properties[] = {
    DEFINE_PROP_DRIVE("drive", DS1742State, blk),
    DEFINE_PROP_DRIVE("clock-drive", DS1742State, clock_blk),
    DEFINE_PROP_UINT32("fresh-year-base", DS1742State, fresh_year_base, 0),
};

static void ds1742_instance_init(Object *obj)
{
    DS1742State *s = DS1742_BASE(obj);
    DS1742Class *dc = DS1742_BASE_CLASS(object_get_class(obj));

    s->size = dc->size;
    s->ctrl = dc->ctrl;
    s->name = dc->name;
    memory_region_init_io(&s->iomem, obj, &ds1742_ops, s, s->name, s->size);
    sysbus_init_mmio(SYS_BUS_DEVICE(obj), &s->iomem);
    s->iomem_link = OBJECT(&s->iomem);
    object_property_add_link(obj, "memory-region", TYPE_MEMORY_REGION,
                             &s->iomem_link, NULL, 0);
}

static const uint32_t ds1742_geometry = DS1742_SIZE;
static const uint32_t ds1744_geometry = DS1744_SIZE;

static void ds1742_base_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);

    dc->realize = ds1742_realize;
    dc->unrealize = ds1742_unrealize;
    device_class_set_props(dc, ds1742_properties);
}

static void ds1742_variant_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    DS1742Class *dsc = DS1742_BASE_CLASS(oc);

    dsc->size = *(const uint32_t *)data;
    dsc->ctrl = dsc->size == DS1742_SIZE ? DS1742_CTRL : DS1744_CTRL;
    dsc->name = dsc->size == DS1742_SIZE ? "ds1742" : "ds1744";
    dsc->vmsd = dsc->size == DS1742_SIZE ? &vmstate_ds1742 : &vmstate_ds1744;
    dc->desc = dsc->size == DS1742_SIZE ? "DS1742 timekeeping SRAM" :
                                          "DS1744 timekeeping SRAM";
    dc->user_creatable = true;
    dc->vmsd = dsc->vmsd;
}

static const TypeInfo ds1742_base_info = {
    .name = TYPE_DS1742_BASE,
    .parent = TYPE_SYS_BUS_DEVICE,
    .class_size = sizeof(DS1742Class),
    .instance_size = sizeof(DS1742State),
    .instance_init = ds1742_instance_init,
    .class_init = ds1742_base_class_init,
    .abstract = true,
};

static const TypeInfo ds1742_info = {
    .name = TYPE_DS1742,
    .parent = TYPE_DS1742_BASE,
    .class_init = ds1742_variant_class_init,
    .class_data = &ds1742_geometry,
};

static const TypeInfo ds1744_info = {
    .name = TYPE_DS1744,
    .parent = TYPE_DS1742_BASE,
    .class_init = ds1742_variant_class_init,
    .class_data = &ds1744_geometry,
};

static void ds1742_register_types(void)
{
    type_register_static(&ds1742_base_info);
    type_register_static(&ds1742_info);
    type_register_static(&ds1744_info);
}

type_init(ds1742_register_types)
