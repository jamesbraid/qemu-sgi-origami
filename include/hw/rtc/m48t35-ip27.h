/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The image a new IP27-family machine's M48T35-class Timekeeper starts
 * with. Plain bytes in and out, so a host tool creating a machine's NVRAM
 * file writes what an unbacked part installs.
 */

#ifndef HW_RTC_M48T35_IP27_H
#define HW_RTC_M48T35_IP27_H

/* The part's 32 KB of byte-wide cells, clock included. */
#define M48T35_SIZE         0x8000

/*
 * Default software year base. The chip itself only keeps two year digits.
 */
#define M48T35_YEAR_BASE    1970

/*
 * Fill the M48T35_SIZE bytes at @cell with the fresh IP27 image for the
 * date @tm: zeroes, IRIX's leap-cycle record for that month and year, and
 * the PROM environment checksum over the result.
 */
void m48t35_ip27_fresh_image(uint8_t *cell, const struct tm *tm);

#endif /* HW_RTC_M48T35_IP27_H */
