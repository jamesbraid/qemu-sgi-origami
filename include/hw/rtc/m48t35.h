/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M48T35-class Timekeeper: battery-backed storage with a clock in its top
 * bytes.
 */

#ifndef HW_RTC_M48T35_H
#define HW_RTC_M48T35_H

#include "hw/core/sysbus.h"
#include "qom/object.h"
#include "system/block-backend.h"

#define TYPE_M48T35 "m48t35"
OBJECT_DECLARE_SIMPLE_TYPE(M48T35State, M48T35)

/*
 * Thirty-two kilobytes of byte-wide cells. All but the last sixteen are
 * ordinary storage; the last eight are the clock, and the eight between them
 * are a bank no source we hold identifies and no driver we run touches.
 */
#define M48T35_SIZE         0x8000

/*
 * The clock, at the top of the window. The control register comes first and
 * the six fields a driver reads follow it, which is the order SGI's own driver
 * for this machine uses and the order OpenBSD reads.
 */
#define M48T35_CONTROL      0x7ff8
#define M48T35_SECONDS      0x7ff9
#define M48T35_MINUTES      0x7ffa
#define M48T35_HOURS        0x7ffb
#define M48T35_WEEKDAY      0x7ffc
#define M48T35_DAY          0x7ffd
#define M48T35_MONTH        0x7ffe
#define M48T35_YEAR         0x7fff

#define M48T35_CLOCK_FIRST  M48T35_CONTROL

/*
 * Bit 6 freezes the clock registers so that six reads are one instant, and
 * bit 7 arms a write of them. Software sets one, does its six accesses, and
 * writes the register back with the bit clear.
 */
#define M48T35_CSR_READ     0x40
#define M48T35_CSR_WRITE    0x80

/*
 * The two-digit year is counted from 1970 on this machine. Every driver adds
 * it back: SGI's own subtracts a constant its comment gives as 1970, OpenBSD
 * adds POSIX_BASE_YEAR for an IP27, and mainline Linux subtracts 1970 when it
 * writes. A model that presented the calendar year's last two digits instead
 * would have a guest report the mid-1990s, and nothing in a boot would look
 * wrong -- which is why the epoch has a test rather than a comment alone.
 */
#define M48T35_YEAR_BASE    1970

struct M48T35State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /*
     * The cells. The clock's eight bytes are not kept here -- they are
     * answered from the host's time and this array's entries for them are
     * unused -- because storage that shadowed a running clock would have two
     * places to be wrong.
     */
    uint8_t cell[M48T35_SIZE];

    /*
     * The battery, optional. Unset, a fresh process starts from zeroed
     * cells exactly as it always has; given a drive of exactly M48T35_SIZE
     * bytes, the cells are loaded from it on realize and every write goes
     * back to it, so content outlives the process the way the real part's
     * content outlives the host machine being off.
     */
    BlockBackend *blk;

    /*
     * What the clock reads relative to the host's, in seconds. A guest that
     * sets the time moves this rather than stopping anything, so the clock
     * keeps running at the rate the host's does.
     */
    int64_t time_offset;

    /*
     * The frozen instant, valid while the freeze bit is set. One number rather
     * than six register values, so the fields a driver reads out of it cannot
     * disagree with each other -- which is the whole point of the freeze.
     */
    bool frozen;
    int64_t frozen_epoch;

    /*
     * The time a guest is assembling while the write bit is set, applied when
     * it clears. A driver writes the fields one at a time, so the set is
     * incomplete until the last one lands, and applying them as they arrive
     * would move the clock through five times nobody asked for.
     */
    bool staging;
    int32_t staged_sec;
    int32_t staged_min;
    int32_t staged_hour;
    int32_t staged_mday;
    int32_t staged_mon;
    int32_t staged_year;
};

#endif
