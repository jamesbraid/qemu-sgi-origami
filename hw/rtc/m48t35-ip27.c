/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The fresh IP27 image of an M48T35-class Timekeeper. The layout and the
 * checksum are the Origami project's Timekeeper datasheet's, in its
 * sections on the IRIX leap-cycle record and the fixture for a new
 * machine.
 */

#include "qemu/osdep.h"
#include "hw/rtc/m48t35-ip27.h"

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

void m48t35_ip27_fresh_image(uint8_t *cell, const struct tm *tm)
{
    memset(cell, 0, M48T35_SIZE);
    cell[M48T35_LEAP_CYCLE] = m48t35_leap_cycle(tm);
    cell[M48T35_CHECKSUM] = m48t35_factory_checksum(cell);
}
