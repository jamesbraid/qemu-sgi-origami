/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M48T35-class Timekeeper: 32 KB of battery-backed byte-wide storage with a
 * real-time clock in its top eight bytes.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * The storage below the clock must answer: SGI's driver probes which clock
 * part is fitted by writing a byte there and reading it back.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bcd.h"
#include "qemu/log.h"
#include "hw/rtc/m48t35.h"
#include "hw/core/qdev-properties.h"
#include "hw/core/qdev-properties-system.h"
#include "migration/vmstate.h"
#include "system/block-backend.h"
#include "system/rtc.h"
#include "qemu/cutils.h"
#include "qemu/error-report.h"
#include "trace.h"

#define M48T35_CHECKSUM_START       0x000e
#define M48T35_RESTART_REASON       0x046f
#define M48T35_CHECKSUM             0x04bc
#define M48T35_LEAP_CYCLE           0x3883
#define M48T35_CHECKSUM_END         0x7fd1
#define M48T35_CHECKSUM_SEED        165

static uint8_t m48t35_rol8(uint8_t value)
{
    return (value << 1) | (value >> 7);
}

static uint8_t m48t35_factory_checksum(const uint8_t *cell)
{
    uint8_t checksum = M48T35_CHECKSUM_SEED;
    unsigned int offset;

    for (offset = M48T35_CHECKSUM_START;
         offset < M48T35_CHECKSUM_END; offset++) {
        if (offset != M48T35_CHECKSUM &&
            offset != M48T35_RESTART_REASON) {
            checksum ^= cell[offset];
        }
        checksum = m48t35_rol8(checksum);
    }
    return checksum;
}

static uint8_t m48t35_leap_cycle(const struct tm *tm)
{
    unsigned int year = (tm->tm_year + 1900 - M48T35_YEAR_BASE) % 100;
    unsigned int remainder = year % 4;

    if ((remainder == 2 && tm->tm_mon >= 2) || remainder == 3 ||
        (remainder == 0 && tm->tm_mon < 2)) {
        return 2;
    }
    return 1;
}

static void m48t35_install_factory_image(M48T35State *s)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    s->cell[M48T35_LEAP_CYCLE] = m48t35_leap_cycle(&tm);
    s->cell[M48T35_CHECKSUM] = m48t35_factory_checksum(s->cell);
}

/* Both paths share the offset, so releasing a freeze cannot jump the clock. */
static void m48t35_now(M48T35State *s, struct tm *tm)
{
    if (s->frozen) {
        time_t frozen = s->frozen_epoch;

        gmtime_r(&frozen, tm);
    } else {
        qemu_get_timedate(tm, s->time_offset);
    }
}

static uint64_t m48t35_read_clock(M48T35State *s, hwaddr addr)
{
    struct tm tm;

    m48t35_now(s, &tm);

    switch (addr) {
    case M48T35_SECONDS:
        return to_bcd(tm.tm_sec);
    case M48T35_MINUTES:
        return to_bcd(tm.tm_min);
    case M48T35_HOURS:
        return to_bcd(tm.tm_hour);
    case M48T35_WEEKDAY:
        /* Stored, not counted; no driver here reads it back. */
        return s->cell[M48T35_WEEKDAY];
    case M48T35_DAY:
        return to_bcd(tm.tm_mday);
    case M48T35_MONTH:
        /* One through twelve, where the C library counts from zero. */
        return to_bcd(tm.tm_mon + 1);
    case M48T35_YEAR:
        /* +100: C's remainder keeps the sign; to_bcd(negative) is not BCD. */
        return to_bcd(((tm.tm_year + 1900 - M48T35_YEAR_BASE) % 100 + 100)
                      % 100);
    default:
        g_assert_not_reached();
    }
}

/* A backing-store failure is not the guest's error; the write landed. */
static void m48t35_store(M48T35State *s, hwaddr addr, uint8_t val)
{
    s->cell[addr] = val;
    if (s->blk && blk_pwrite(s->blk, addr, 1, &s->cell[addr], 0) < 0) {
        error_report("m48t35: write to backing store failed");
    }
}

static void m48t35_write_clock(M48T35State *s, hwaddr addr, uint8_t val)
{
    /* Collected while the write bit is set, applied when it clears. */
    switch (addr) {
    case M48T35_SECONDS:
        s->staged_sec = from_bcd(val);
        break;
    case M48T35_MINUTES:
        s->staged_min = from_bcd(val);
        break;
    case M48T35_HOURS:
        s->staged_hour = from_bcd(val);
        break;
    case M48T35_WEEKDAY:
        /* The one writable clock byte that is storage on the part. */
        m48t35_store(s, M48T35_WEEKDAY, val);
        break;
    case M48T35_DAY:
        s->staged_mday = from_bcd(val);
        break;
    case M48T35_MONTH:
        s->staged_mon = from_bcd(val) - 1;
        break;
    case M48T35_YEAR:
        s->staged_year = from_bcd(val) + M48T35_YEAR_BASE - 1900;
        break;
    default:
        g_assert_not_reached();
    }
}

/* Drivers write the whole register back; only the transitions mean anything. */
static void m48t35_write_control(M48T35State *s, uint8_t val)
{
    uint8_t was = s->cell[M48T35_CONTROL];

    if ((val & M48T35_CSR_READ) && !(was & M48T35_CSR_READ)) {
        /* Every read until the bit clears reports this instant. */
        struct tm tm;

        qemu_get_timedate(&tm, s->time_offset);
        s->frozen_epoch = mktimegm(&tm);
        s->frozen = true;
    } else if (!(val & M48T35_CSR_READ) && (was & M48T35_CSR_READ)) {
        s->frozen = false;
    }

    if ((val & M48T35_CSR_WRITE) && !(was & M48T35_CSR_WRITE)) {
        /* Start from the current time so unwritten fields keep their value. */
        struct tm tm;

        m48t35_now(s, &tm);
        s->staged_sec = tm.tm_sec;
        s->staged_min = tm.tm_min;
        s->staged_hour = tm.tm_hour;
        s->staged_mday = tm.tm_mday;
        s->staged_mon = tm.tm_mon;
        s->staged_year = tm.tm_year;
        s->staging = true;
    } else if (!(val & M48T35_CSR_WRITE) && (was & M48T35_CSR_WRITE)) {
        struct tm tm = {
            .tm_sec = s->staged_sec,
            .tm_min = s->staged_min,
            .tm_hour = s->staged_hour,
            .tm_mday = s->staged_mday,
            .tm_mon = s->staged_mon,
            .tm_year = s->staged_year,
        };

        struct tm now;

        /*
         * Not qemu_timedate_diff(): that subtracts the host clock
         * unconditionally, while reads use whichever clock -rtc clock=
         * selected. Differencing through qemu_get_timedate() keeps both
         * ends on the same clock.
         */
        s->staging = false;
        qemu_get_timedate(&now, 0);
        s->time_offset = mktimegm(&tm) - mktimegm(&now);
    }

    m48t35_store(s, M48T35_CONTROL, val);
}

static uint64_t m48t35_read(void *opaque, hwaddr addr, unsigned size)
{
    M48T35State *s = opaque;
    uint64_t value;

    if (addr > M48T35_CONTROL) {
        value = m48t35_read_clock(s, addr);
        trace_m48t35_clock_read(addr, value);
        return value;
    }

    /* Control and below are storage, the unidentified bank included. */
    value = s->cell[addr];
    if (addr >= M48T35_CLOCK_FIRST) {
        trace_m48t35_clock_read(addr, value);
    } else {
        trace_m48t35_nvram_read(addr, value);
    }
    return value;
}

static void m48t35_write(void *opaque, hwaddr addr, uint64_t val,
                         unsigned size)
{
    M48T35State *s = opaque;

    if (addr >= M48T35_CLOCK_FIRST) {
        trace_m48t35_clock_write(addr, val);
    } else {
        trace_m48t35_nvram_write(addr, val);
    }

    if (addr == M48T35_CONTROL) {
        m48t35_write_control(s, val);
        return;
    }

    if (addr > M48T35_CONTROL) {
        if (s->staging) {
            m48t35_write_clock(s, addr, val);
        } else {
            /* The counters are running; hardware has nowhere to put this. */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "m48t35: write of 0x%02x to clock register 0x%04x "
                          "with the write bit clear\n",
                          (unsigned)val, (unsigned)addr);
        }
        return;
    }

    m48t35_store(s, addr, val);
}

/* Byte-wide by declaration; nothing else stops the core widening an access. */
static const MemoryRegionOps m48t35_ops = {
    .read = m48t35_read,
    .write = m48t35_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void m48t35_reset_hold(Object *obj, ResetType type)
{
    M48T35State *s = M48T35(obj);

    /*
     * Battery-backed: cells and time survive reset; an unfinished freeze or
     * write does not, or the next guest inherits a stopped clock.
     */
    s->frozen = false;
    s->staging = false;
    s->cell[M48T35_CONTROL] &= ~(M48T35_CSR_READ | M48T35_CSR_WRITE);
}

static void m48t35_realize(DeviceState *dev, Error **errp)
{
    M48T35State *s = M48T35(dev);

    s->cell = g_malloc0(M48T35_SIZE);

    /* Exact size required, as hw/nvram/mac_nvram.c: a mismatch is a mistake. */
    if (s->blk) {
        int64_t len = blk_getlength(s->blk);

        if (len < 0) {
            error_setg_errno(errp, -len,
                             "m48t35: could not get the length of the "
                             "nvram backing file");
            return;
        }
        if (len != M48T35_SIZE) {
            error_setg(errp,
                      "m48t35: nvram backing file is %" PRId64 " bytes, "
                      "not the required %u", len, M48T35_SIZE);
            return;
        }
        if (blk_set_perm(s->blk, BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                         BLK_PERM_ALL, errp) < 0) {
            return;
        }
        if (blk_pread(s->blk, 0, M48T35_SIZE, s->cell, 0) < 0) {
            error_setg(errp, "m48t35: could not read the nvram backing "
                      "file");
            return;
        }
    } else {
        m48t35_install_factory_image(s);
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &m48t35_ops, s,
                          "m48t35", M48T35_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void m48t35_unrealize(DeviceState *dev)
{
    M48T35State *s = M48T35(dev);

    g_free(s->cell);
    s->cell = NULL;
}

/*
 * The cell buffer is heap-allocated but a uint8 array and a buffer serialise
 * identically, so version 1 streams load unchanged.
 */
static const VMStateDescription vmstate_m48t35 = {
    .name = "m48t35",
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_BUFFER_POINTER_UNSAFE(cell, M48T35State, 0, M48T35_SIZE),
        VMSTATE_INT64(time_offset, M48T35State),
        VMSTATE_BOOL(frozen, M48T35State),
        VMSTATE_INT64(frozen_epoch, M48T35State),
        VMSTATE_BOOL(staging, M48T35State),
        VMSTATE_INT32(staged_sec, M48T35State),
        VMSTATE_INT32(staged_min, M48T35State),
        VMSTATE_INT32(staged_hour, M48T35State),
        VMSTATE_INT32(staged_mday, M48T35State),
        VMSTATE_INT32(staged_mon, M48T35State),
        VMSTATE_INT32(staged_year, M48T35State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property m48t35_properties[] = {
    DEFINE_PROP_DRIVE("drive", M48T35State, blk),
};

static void m48t35_class_init(ObjectClass *oc, const void *data)
{
    DeviceClass *dc = DEVICE_CLASS(oc);
    ResettableClass *rc = RESETTABLE_CLASS(oc);

    dc->desc = "M48T35-class Timekeeper NVRAM and real-time clock";
    dc->realize = m48t35_realize;
    dc->unrealize = m48t35_unrealize;
    dc->vmsd = &vmstate_m48t35;
    device_class_set_props(dc, m48t35_properties);
    rc->phases.hold = m48t35_reset_hold;
}

static const TypeInfo m48t35_types[] = {
    {
        .name = TYPE_M48T35,
        .parent = TYPE_SYS_BUS_DEVICE,
        .instance_size = sizeof(M48T35State),
        .class_init = m48t35_class_init,
    },
};

DEFINE_TYPES(m48t35_types)
