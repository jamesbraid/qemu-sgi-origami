/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * M48T35-class Timekeeper: battery-backed storage with a clock in its top
 * bytes.
 *
 * Copyright (c) 2026 Origami contributors
 */

#ifndef HW_RTC_M48T35_H
#define HW_RTC_M48T35_H

#include "hw/core/sysbus.h"
#include "qom/object.h"

#define TYPE_M48T35 "m48t35"
OBJECT_DECLARE_SIMPLE_TYPE(M48T35State, M48T35)

/*
 * 32 KB of byte-wide cells: storage, then an unidentified bank of eight,
 * then the clock in the top eight.
 */
#define M48T35_SIZE         0x8000

/* The clock: control first, then the six fields, the order drivers read. */
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
 * The two-digit year counts from 1970; every driver adds it back. Calendar
 * digits instead would boot cleanly and report the mid-1990s.
 */
#define M48T35_YEAR_BASE    1970

struct M48T35State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    /*
     * Allocated at realize. The clock's entries are unused: it answers
     * from time, and storage shadowing a running clock has two places to
     * be wrong.
     */
    uint8_t *cell;

    /*
     * The battery, optional: a drive of exactly M48T35_SIZE bytes that
     * cells load from at realize and every write goes back to.
     */
    BlockBackend *blk;

    /* Guest-set time as an offset from the host clock, in seconds. */
    int64_t time_offset;

    /*
     * The frozen instant, one number so the six fields read from it cannot
     * disagree -- the point of the freeze.
     */
    bool frozen;
    int64_t frozen_epoch;

    /* Fields staged while the write bit is set, applied when it clears. */
    bool staging;
    int32_t staged_sec;
    int32_t staged_min;
    int32_t staged_hour;
    int32_t staged_mday;
    int32_t staged_mon;
    int32_t staged_year;
};

#endif /* HW_RTC_M48T35_H */
