/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * M48T35-class Timekeeper: battery-backed storage with a clock in its top
 * bytes.
 */

#ifndef HW_RTC_M48T35_H
#define HW_RTC_M48T35_H

#include "hw/core/sysbus.h"
#include "system/runstate.h"
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
 * Bit 6 freezes the clock so six reads are one instant; bit 7 arms a
 * write of them.
 */
#define M48T35_CSR_READ     0x40
#define M48T35_CSR_WRITE    0x80

/*
 * Default software year base. The chip itself only keeps two year digits.
 */
#define M48T35_YEAR_BASE    1970

/*
 * The part's calendar: two BCD year digits and no century, with February 29
 * days long when the year cell is a multiple of four. On this machine that
 * puts the leap day in 2022, 2026 and 2030 and none in 2024 or 2028. The
 * model counts the part's time as seconds since 1 January of cell year 00
 * under that rule, wrapping every hundred cell years, so reads, writes and
 * the restore after a stop all agree with it.
 */
#define M48T35_CELL_YEARS   100
#define M48T35_CENTURY_DAYS (M48T35_CELL_YEARS * 365 + M48T35_CELL_YEARS / 4)
#define M48T35_DAY_SECONDS  86400
#define M48T35_CENTURY_SECONDS \
    ((int64_t)M48T35_CENTURY_DAYS * M48T35_DAY_SECONDS)

/*
 * Bit 7 of the seconds cell stops the part's oscillator: a stopped clock
 * does not count, the bit reads back, and it is battery-backed, so a save
 * keeps it and a restore adds no time (see "The clock across a stopped
 * emulator" below).
 * The model takes it, like the rest of the seconds register, when a guest
 * clock write completes.
 */
#define M48T35_SECONDS_STOP 0x80

/*
 * The clock across a stopped emulator. With a backing image the model
 * writes the calendar into the image's own clock cells, in the part's
 * format, whenever a guest clock write completes and when the VM stops, and
 * restores it at realize with the time that passed in between.
 *
 * That elapsed time needs a reference the 32 KB image cannot hold without
 * changing what the guest reads. Model policy: it lives in a second, small
 * raw drive, the "clock-drive" property. If that is not set and the storage
 * drive is named, a drive named "<storage drive>-clock" is used, so
 * "-drive if=none,id=sgi-nvram0-clock,file=...,format=raw" beside the
 * machine's "sgi-nvram0" is all a launcher adds. The record is a magic
 * value and the time of the save, as seconds of the clock QEMU's -rtc
 * option selects, so the drive must hold at least M48T35_CLOCK_REF_BYTES,
 * 16. Realize refuses one the block layer reports as smaller, rather than
 * fail every save. It reports a raw file's length in whole 512-byte
 * sectors, so in practice that is an empty file; a shorter nonempty one
 * reads as zeros past its end and grows on the first save. A missing or
 * unwritten record restores the saved calendar with no time added.
 */
#define M48T35_CLOCK_REF_MAGIC  "M48TREF1"
#define M48T35_CLOCK_REF_BYTES  16

struct M48T35State {
    SysBusDevice parent_obj;

    MemoryRegion iomem;

    uint32_t year_base;
    bool initial_ip27_fixture;

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

    /* The saved-time reference for the clock across a stop; see above. */
    BlockBackend *clock_blk;
    VMChangeStateEntry *vmstate_change;

    /*
     * The part's time, in seconds of its own calendar (above), less the
     * clock -rtc selects: the part reads (host seconds + time_offset) wrapped
     * to a century of cell years.
     */
    int64_t time_offset;

    /*
     * The frozen instant, in the part's seconds, one number so the six
     * fields read from it cannot disagree -- the point of the freeze.
     */
    bool frozen;
    int64_t frozen_epoch;

    /*
     * The oscillator stopped, and the instant it stopped at in the part's
     * seconds; time_offset is not used while it is.
     */
    bool stopped;
    int64_t stopped_epoch;

    /* Fields staged while the write bit is set, applied when it clears. */
    bool staging;
    int32_t staged_sec;
    int32_t staged_min;
    int32_t staged_hour;
    int32_t staged_mday;
    int32_t staged_mon;
    int32_t staged_year;
    bool staged_stop;
};

#endif /* HW_RTC_M48T35_H */
