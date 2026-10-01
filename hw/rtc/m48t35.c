/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * M48T35-class Timekeeper: 32 KB of battery-backed byte-wide storage with a
 * real-time clock in its top eight bytes.
 *
 * The storage below the clock must answer: SGI's driver probes which clock
 * part is fitted by writing a byte there and reading it back.
 */

#include "qemu/osdep.h"
#include "qapi/error.h"
#include "qemu/bcd.h"
#include "qemu/bswap.h"
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

/* A date in the part's own terms: cell year 0-99, month 0-11. */
typedef struct M48T35Calendar {
    int sec;
    int min;
    int hour;
    int mday;
    int mon;
    int year;
} M48T35Calendar;

static const uint16_t m48t35_days_before_month[12] = {
    0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334,
};

static const uint8_t m48t35_days_in_month[12] = {
    31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31,
};

/* The part's rule: a year cell that is a multiple of four has 29 February. */
static bool m48t35_leap_year(int year)
{
    return year % 4 == 0;
}

static int m48t35_month_length(int year, int mon)
{
    return m48t35_days_in_month[mon] + (mon == 1 && m48t35_leap_year(year));
}

static int64_t m48t35_wrap(int64_t seconds)
{
    seconds %= M48T35_CENTURY_SECONDS;
    return seconds < 0 ? seconds + M48T35_CENTURY_SECONDS : seconds;
}

/*
 * The part's seconds for a date. A day past its month's end runs on into the
 * next month, as the arithmetic falls; a month outside 1-12 carries into the
 * year the way mktime() would, so a guest's nonsense stays in range.
 */
static int64_t m48t35_part_seconds(const M48T35Calendar *c)
{
    int year = c->year + c->mon / 12 - (c->mon % 12 < 0);
    int mon = (c->mon % 12 + 12) % 12;
    int64_t days;

    year = (year % M48T35_CELL_YEARS + M48T35_CELL_YEARS) % M48T35_CELL_YEARS;
    days = (int64_t)year * 365 + (year + 3) / 4 +
           m48t35_days_before_month[mon] +
           (mon > 1 && m48t35_leap_year(year)) + c->mday - 1;
    return m48t35_wrap(days * M48T35_DAY_SECONDS + c->hour * 3600 +
                       c->min * 60 + c->sec);
}

static void m48t35_part_calendar(int64_t seconds, M48T35Calendar *c)
{
    int64_t days;
    int rest, length;

    seconds = m48t35_wrap(seconds);
    days = seconds / M48T35_DAY_SECONDS;
    rest = seconds % M48T35_DAY_SECONDS;
    c->hour = rest / 3600;
    c->min = rest / 60 % 60;
    c->sec = rest % 60;
    for (c->year = 0;
         days >= (length = 365 + m48t35_leap_year(c->year)); c->year++) {
        days -= length;
    }
    for (c->mon = 0;
         days >= (length = m48t35_month_length(c->year, c->mon)); c->mon++) {
        days -= length;
    }
    c->mday = days + 1;
}

/* Seconds of the clock -rtc selects, the model's running reference. */
static int64_t m48t35_host_seconds(void)
{
    struct tm tm;

    qemu_get_timedate(&tm, 0);
    return mktimegm(&tm);
}

/* The host's date written as the part would hold it. */
static int64_t m48t35_host_part_seconds(void)
{
    struct tm tm;
    M48T35Calendar c;

    qemu_get_timedate(&tm, 0);
    c = (M48T35Calendar) {
        .sec = tm.tm_sec,
        .min = tm.tm_min,
        .hour = tm.tm_hour,
        .mday = tm.tm_mday,
        .mon = tm.tm_mon,
        /* +100: C's remainder keeps the sign, and 1969 is cell 99. */
        .year = ((tm.tm_year + 1900 - M48T35_YEAR_BASE) % 100 + 100) % 100,
    };
    return m48t35_part_seconds(&c);
}

/* Start the part at the host's date. */
static void m48t35_follow_host(M48T35State *s)
{
    s->time_offset = m48t35_host_part_seconds() - m48t35_host_seconds();
}

/* The part's counters, whether or not a read has frozen the cells. */
static int64_t m48t35_running(M48T35State *s)
{
    if (s->stopped) {
        return s->stopped_epoch;
    }
    return m48t35_wrap(m48t35_host_seconds() + s->time_offset);
}

/* Both paths share the offset, so releasing a freeze cannot jump the clock. */
static void m48t35_now(M48T35State *s, M48T35Calendar *c)
{
    m48t35_part_calendar(s->frozen ? s->frozen_epoch : m48t35_running(s), c);
}

static uint64_t m48t35_read_clock(M48T35State *s, hwaddr addr)
{
    M48T35Calendar c;

    m48t35_now(s, &c);

    switch (addr) {
    case M48T35_SECONDS:
        return to_bcd(c.sec) | (s->stopped ? M48T35_SECONDS_STOP : 0);
    case M48T35_MINUTES:
        return to_bcd(c.min);
    case M48T35_HOURS:
        return to_bcd(c.hour);
    case M48T35_WEEKDAY:
        /* Stored, not counted; no driver here reads it back. */
        return s->cell[M48T35_WEEKDAY];
    case M48T35_DAY:
        return to_bcd(c.mday);
    case M48T35_MONTH:
        /* One through twelve, where the calendar counts from zero. */
        return to_bcd(c.mon + 1);
    case M48T35_YEAR:
        return to_bcd(c.year);
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

/*
 * Save the running calendar into the backing image's clock cells, in the
 * part's format, and the moment of the save into the reference record. The
 * weekday cell between them is storage and keeps what the guest wrote; no
 * other cell is touched. See the header.
 */
static void m48t35_save_clock(M48T35State *s)
{
    uint8_t record[M48T35_CLOCK_REF_BYTES];
    int64_t host = m48t35_host_seconds();
    M48T35Calendar c;

    if (!s->blk) {
        return;
    }
    m48t35_part_calendar(m48t35_running(s), &c);
    s->cell[M48T35_SECONDS] = to_bcd(c.sec) |
                              (s->stopped ? M48T35_SECONDS_STOP : 0);
    s->cell[M48T35_MINUTES] = to_bcd(c.min);
    s->cell[M48T35_HOURS] = to_bcd(c.hour);
    s->cell[M48T35_DAY] = to_bcd(c.mday);
    s->cell[M48T35_MONTH] = to_bcd(c.mon + 1);
    s->cell[M48T35_YEAR] = to_bcd(c.year);
    if (blk_pwrite(s->blk, M48T35_SECONDS, M48T35_YEAR - M48T35_SECONDS + 1,
                   &s->cell[M48T35_SECONDS], 0) < 0) {
        error_report("m48t35: saving the clock to the backing store failed");
    }
    trace_m48t35_clock_saved(c.year, c.mon + 1, c.mday, c.hour, c.min, c.sec,
                             host);

    if (s->clock_blk) {
        memcpy(record, M48T35_CLOCK_REF_MAGIC, 8);
        stq_le_p(record + 8, host);
        if (blk_pwrite(s->clock_blk, 0, sizeof(record), record, 0) < 0) {
            error_report("m48t35: saving the clock reference failed");
        }
    }
}

static bool m48t35_is_bcd(uint8_t v, int lo, int hi)
{
    return (v & 0xf) <= 9 && (v >> 4) <= 9 &&
           from_bcd(v) >= lo && from_bcd(v) <= hi;
}

/* The time the reference record says the image was saved, if it has one. */
static bool m48t35_clock_reference(M48T35State *s, int64_t *saved_at)
{
    uint8_t record[M48T35_CLOCK_REF_BYTES];

    if (!s->clock_blk ||
        blk_getlength(s->clock_blk) < M48T35_CLOCK_REF_BYTES ||
        blk_pread(s->clock_blk, 0, sizeof(record), record, 0) < 0 ||
        memcmp(record, M48T35_CLOCK_REF_MAGIC, 8)) {
        return false;
    }
    *saved_at = ldq_le_p(record + 8);
    return true;
}

/*
 * Restore the clock from a backing image, per the contract's rules:
 * - clock cells all zero, or not a valid date in BCD: never saved, so the
 *   clock follows the host, as it did before images carried one;
 * - the stop bit set in the saved seconds: the oscillator was stopped, so
 *   the saved calendar comes back with no time added, still stopped;
 * - no reference record: the saved calendar with no time added, logged;
 * - otherwise the saved calendar plus the time since the save, counted in
 *   the part's calendar, so it rolls over February as the part would.
 */
static void m48t35_restore_clock(M48T35State *s)
{
    const uint8_t *cell = s->cell;
    uint8_t sec = cell[M48T35_SECONDS] & ~M48T35_SECONDS_STOP;
    bool stopped = cell[M48T35_SECONDS] & M48T35_SECONDS_STOP;
    int64_t host = m48t35_host_seconds();
    int64_t elapsed = 0, saved_at;
    M48T35Calendar c;

    if (!(cell[M48T35_SECONDS] | cell[M48T35_MINUTES] | cell[M48T35_HOURS] |
          cell[M48T35_DAY] | cell[M48T35_MONTH] | cell[M48T35_YEAR]) ||
        !m48t35_is_bcd(sec, 0, 59) ||
        !m48t35_is_bcd(cell[M48T35_MINUTES], 0, 59) ||
        !m48t35_is_bcd(cell[M48T35_HOURS], 0, 23) ||
        !m48t35_is_bcd(cell[M48T35_DAY], 1, 31) ||
        !m48t35_is_bcd(cell[M48T35_MONTH], 1, 12) ||
        !m48t35_is_bcd(cell[M48T35_YEAR], 0, 99)) {
        trace_m48t35_clock_restore("never saved: following the host", 0);
        m48t35_follow_host(s);
        return;
    }

    c = (M48T35Calendar) {
        .sec = from_bcd(sec),
        .min = from_bcd(cell[M48T35_MINUTES]),
        .hour = from_bcd(cell[M48T35_HOURS]),
        .mday = from_bcd(cell[M48T35_DAY]),
        .mon = from_bcd(cell[M48T35_MONTH]) - 1,
        .year = from_bcd(cell[M48T35_YEAR]),
    };
    if (stopped) {
        trace_m48t35_clock_restore("stopped: no time added", 0);
        s->stopped = true;
        s->stopped_epoch = m48t35_part_seconds(&c);
        return;
    }
    if (m48t35_clock_reference(s, &saved_at)) {
        elapsed = MAX(host - saved_at, 0);
        trace_m48t35_clock_restore("saved time plus elapsed", elapsed);
    } else {
        qemu_log_mask(LOG_UNIMP, "m48t35: no clock reference beside the "
                      "backing image; restoring the saved time with no "
                      "time added\n");
        trace_m48t35_clock_restore("no reference: no time added", 0);
    }
    s->time_offset = m48t35_part_seconds(&c) + elapsed - host;
}

static void m48t35_write_clock(M48T35State *s, hwaddr addr, uint8_t val)
{
    /* Collected while the write bit is set, applied when it clears. */
    switch (addr) {
    case M48T35_SECONDS:
        s->staged_sec = from_bcd(val & ~M48T35_SECONDS_STOP);
        s->staged_stop = val & M48T35_SECONDS_STOP;
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
        s->staged_year = from_bcd(val);
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
        s->frozen_epoch = m48t35_running(s);
        s->frozen = true;
    } else if (!(val & M48T35_CSR_READ) && (was & M48T35_CSR_READ)) {
        s->frozen = false;
    }

    if ((val & M48T35_CSR_WRITE) && !(was & M48T35_CSR_WRITE)) {
        /* Start from the current time so unwritten fields keep their value. */
        M48T35Calendar c;

        m48t35_now(s, &c);
        s->staged_sec = c.sec;
        s->staged_min = c.min;
        s->staged_hour = c.hour;
        s->staged_mday = c.mday;
        s->staged_mon = c.mon;
        s->staged_year = c.year;
        s->staged_stop = s->stopped;
        s->staging = true;
    } else if (!(val & M48T35_CSR_WRITE) && (was & M48T35_CSR_WRITE)) {
        M48T35Calendar c = {
            .sec = s->staged_sec,
            .min = s->staged_min,
            .hour = s->staged_hour,
            .mday = s->staged_mday,
            .mon = s->staged_mon,
            .year = s->staged_year,
        };

        /*
         * Differenced against the clock -rtc selects, as reads are, so both
         * ends stay on the same clock.
         */
        s->staging = false;
        s->stopped = s->staged_stop;
        s->stopped_epoch = m48t35_part_seconds(&c);
        s->time_offset = s->stopped_epoch - m48t35_host_seconds();
        m48t35_store(s, M48T35_CONTROL, val);
        m48t35_save_clock(s);
        return;
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

/* The VM stopping is the teardown the image's clock cells are saved at. */
static void m48t35_vm_state_change(void *opaque, bool running,
                                   RunState state)
{
    if (!running) {
        m48t35_save_clock(opaque);
    }
}

/* The reference drive, by the "<storage drive>-clock" convention if unset. */
static bool m48t35_attach_clock_drive(M48T35State *s, Error **errp)
{
    g_autofree char *id = NULL;
    BlockBackend *blk;
    int64_t len;

    if (!s->clock_blk && *blk_name(s->blk)) {
        id = g_strdup_printf("%s-clock", blk_name(s->blk));
        blk = blk_by_name(id);
        if (blk &&
            !qdev_prop_set_drive_err(DEVICE(s), "clock-drive", blk, errp)) {
            return false;
        }
    }
    if (!s->clock_blk) {
        return true;
    }
    /* Too small to hold the record would fail every save; say so now. */
    len = blk_getlength(s->clock_blk);
    if (len < 0) {
        error_setg_errno(errp, -len, "m48t35: could not get the length of "
                         "the clock reference drive");
        return false;
    }
    if (len < M48T35_CLOCK_REF_BYTES) {
        error_setg(errp, "m48t35: clock reference drive is %" PRId64
                   " bytes, less than the %u its record needs",
                   len, M48T35_CLOCK_REF_BYTES);
        return false;
    }
    return blk_set_perm(s->clock_blk,
                        BLK_PERM_CONSISTENT_READ | BLK_PERM_WRITE,
                        BLK_PERM_ALL, errp) >= 0;
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
        if (!m48t35_attach_clock_drive(s, errp)) {
            return;
        }
        m48t35_restore_clock(s);
        s->vmstate_change =
            qemu_add_vm_change_state_handler(m48t35_vm_state_change, s);
    } else {
        m48t35_install_factory_image(s);
        m48t35_follow_host(s);
    }

    memory_region_init_io(&s->iomem, OBJECT(dev), &m48t35_ops, s,
                          "m48t35", M48T35_SIZE);
    sysbus_init_mmio(SYS_BUS_DEVICE(dev), &s->iomem);
}

static void m48t35_unrealize(DeviceState *dev)
{
    M48T35State *s = M48T35(dev);

    if (s->vmstate_change) {
        m48t35_save_clock(s);
        qemu_del_vm_change_state_handler(s->vmstate_change);
        s->vmstate_change = NULL;
    }
    g_free(s->cell);
    s->cell = NULL;
}

/*
 * Version 2: time_offset and frozen_epoch count the part's own calendar,
 * and staged_year the year cell, where version 1 counted the real calendar.
 * Version 3: the stop bit.
 */
static const VMStateDescription vmstate_m48t35 = {
    .name = "m48t35",
    .version_id = 3,
    .minimum_version_id = 3,
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
        VMSTATE_BOOL(stopped, M48T35State),
        VMSTATE_INT64(stopped_epoch, M48T35State),
        VMSTATE_BOOL(staged_stop, M48T35State),
        VMSTATE_END_OF_LIST()
    }
};

static const Property m48t35_properties[] = {
    DEFINE_PROP_DRIVE("drive", M48T35State, blk),
    DEFINE_PROP_DRIVE("clock-drive", M48T35State, clock_blk),
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
