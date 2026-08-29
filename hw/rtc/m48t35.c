/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M48T35-class Timekeeper: 32 KB of battery-backed byte-wide storage with a
 * real-time clock in its top eight bytes.
 *
 * Copyright (c) 2026 Origami contributors
 *
 * The Origin 200 hangs one of these off its I/O controller's byte bus, where a
 * guest reads it once during boot to set the system clock. Everything below
 * the clock is ordinary storage, and that is not a detail: SGI's own driver
 * decides which of two possible clock parts is fitted by writing a byte into
 * that storage and reading it back, so a model answering only the clock
 * registers would be driven through a different part's register set entirely.
 *
 * This is a device of ours rather than an extension of QEMU's m48t59 model.
 * That model knows three parts, none of them this size, and writes their clock
 * offsets as literal cases rather than deriving them, so fitting this part
 * would mean threading a fourth model through both of its switch statements --
 * an edit to a shared upstream file, in a fork that is rebased onto master
 * indefinitely. It also carries an alarm block, a watchdog and an interrupt
 * output that this part does not have. The timekeeper datasheet in the
 * Origami project tree (docs/datasheets/timekeeper.md there, not in this
 * QEMU tree) carries the comparison and the sources behind every value
 * here.
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

/*
 * The time a read of the clock should report: the frozen instant if software
 * has asked for one, and otherwise the live time. Both go through the same
 * offset, so freezing cannot make the clock jump when it is released.
 */
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
        /*
         * What was last written, and zero until something writes it. Real
         * hardware counts days in this register and this model does not, which
         * no driver here can tell: the one that reads it discards the value,
         * and its own comment records that the part leaves the register alone
         * until software sets it non-zero -- so zero on a machine nobody has
         * set the day on is the honest answer rather than a gap.
         */
        return s->cell[M48T35_WEEKDAY];
    case M48T35_DAY:
        return to_bcd(tm.tm_mday);
    case M48T35_MONTH:
        /* One through twelve, where the C library counts from zero. */
        return to_bcd(tm.tm_mon + 1);
    case M48T35_YEAR:
        /*
         * Two digits counted from 1970, which is what every driver adds back.
         * The wrap is what happens on this part after 2069 and is the same
         * thing the hardware does with two digits.
         *
         * The addition before the modulo is not decoration: a base earlier
         * than 1970 gives a negative difference, and C's remainder keeps the
         * sign, so the plain expression would hand to_bcd() a negative number
         * and produce a byte that is not BCD at all. A clock started before
         * its own epoch has no right answer -- this one wraps, as the two
         * digits do at the other end.
         */
        return to_bcd(((tm.tm_year + 1900 - M48T35_YEAR_BASE) % 100 + 100)
                      % 100);
    default:
        g_assert_not_reached();
    }
}

/*
 * Every write into a cell, including the two bytes in the clock block that
 * are genuine storage rather than derived from the running clock -- the
 * control register and the weekday. This is the one place that has to know
 * a drive is attached, so nothing else in the file does. A backing-store
 * failure is reported and otherwise ignored: the guest's write already
 * landed in s->cell, and a host-side write failure is not something the
 * guest did wrong.
 */
static void m48t35_store(M48T35State *s, hwaddr addr, uint8_t val)
{
    s->cell[addr] = val;
    if (s->blk && blk_pwrite(s->blk, addr, 1, &s->cell[addr], 0) < 0) {
        error_report("m48t35: write to backing store failed");
    }
}

static void m48t35_write_clock(M48T35State *s, hwaddr addr, uint8_t val)
{
    /*
     * Fields arrive one at a time while the write bit is set, so they are
     * collected and applied together when it clears. A model that applied each
     * field as it arrived would move the clock six times through five
     * inconsistent times on the way to the one the guest meant.
     */
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
        /*
         * Kept rather than derived from the date. The register is writable and
         * a model that dropped the write would be advertising storage it does
         * not have -- invisible on every path we run, since OpenBSD writes it
         * and never reads it, IRIX skips it, and Linux reads it and throws the
         * value away, but a silent drop is not something to leave in a device
         * because nobody currently looks.
         */
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

/*
 * The control register's two bits, acted on as they change rather than as they
 * are written, because a driver writes the whole register back each time and
 * only the transitions mean anything.
 */
static void m48t35_write_control(M48T35State *s, uint8_t val)
{
    uint8_t was = s->cell[M48T35_CONTROL];

    if ((val & M48T35_CSR_READ) && !(was & M48T35_CSR_READ)) {
        /*
         * Freeze. The instant is captured now and every field read until the
         * bit clears comes from it, so a driver's six reads describe one time
         * even if a second passes between the first and the last.
         */
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
         * The offset has to be measured against the same clock the read path
         * applies it to, and qemu_timedate_diff() does not do that: it
         * subtracts the *host* clock unconditionally, while qemu_get_timedate()
         * reads whichever clock -rtc clock= selected. With the two agreeing --
         * the default, where both are the host clock -- the difference is
         * invisible. With them disagreeing, a guest that sets the time reads
         * back something else, off by however far apart the two clocks were at
         * the moment it set it.
         *
         * Taking the current time through qemu_get_timedate() with no offset
         * and differencing against that keeps both ends on one clock, whichever
         * one it is. Both operands go through the same conversion, so the base
         * cancels; the one case it does not is a daylight-saving boundary
         * falling between these two calls, which are microseconds apart.
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

    /*
     * The control register and everything below it read back as storage. That
     * covers the bank of eight below the clock whose purpose no source we hold
     * identifies -- treating it as cells is what lets a guest that writes there
     * read back what it wrote, and nothing we run does either.
     */
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
            /*
             * A clock field written without the write bit set. The hardware
             * has nowhere to put it -- the counters are running -- so the
             * write is dropped rather than stored somewhere a later read would
             * find it.
             */
            qemu_log_mask(LOG_GUEST_ERROR,
                          "m48t35: write of 0x%02x to clock register 0x%04x "
                          "with the write bit clear\n",
                          (unsigned)val, (unsigned)addr);
        }
        return;
    }

    m48t35_store(s, addr, val);
}

/*
 * Byte-wide, and declared so rather than inherited. The part is a flat block
 * of byte cells, which is exactly the shape that invites being mapped without
 * a thought about access width, and no guest on this machine reads it wide --
 * so a boot cannot catch the mistake and only this declaration prevents it.
 */
static const MemoryRegionOps m48t35_ops = {
    .read = m48t35_read,
    .write = m48t35_write,
    .endianness = DEVICE_BIG_ENDIAN,
    .valid.min_access_size = 1,
    .valid.max_access_size = 1,
    .impl.min_access_size = 1,
    .impl.max_access_size = 1,
};

static void m48t35_reset_hold(Object *obj, ResetType type)
{
    M48T35State *s = M48T35(obj);

    /*
     * The cells are not cleared and the time is not moved. This part is
     * battery-backed: a reset line does not empty memory, and what it holds
     * afterwards is what it held before. What does not survive is a
     * transaction a guest was in the middle of -- a freeze or a write it never
     * completed -- because leaving those set would hand the next guest a clock
     * that had stopped for reasons it could not see.
     */
    s->frozen = false;
    s->staging = false;
    s->cell[M48T35_CONTROL] &= ~(M48T35_CSR_READ | M48T35_CSR_WRITE);
}

static void m48t35_realize(DeviceState *dev, Error **errp)
{
    M48T35State *s = M48T35(dev);

    s->cell = g_malloc0(M48T35_SIZE);

    /*
     * The drive is optional, and the check is exact rather than a minimum:
     * a file of any other length is not this part's content, not a partial
     * write to grow later. Same contract hw/nvram/mac_nvram.c's blk-backed
     * NVRAM uses for the same reason -- a size mismatch is a configuration
     * mistake to report, not a size to adapt to.
     */
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
 * The cell buffer moved from an embedded array to a pointer, and the
 * migration stream did not move with it: a uint8 array and a buffer
 * serialise to the same raw bytes, so version 1 streams load unchanged.
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
