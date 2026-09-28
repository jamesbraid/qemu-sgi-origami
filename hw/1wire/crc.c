/* SPDX-License-Identifier: GPL-2.0-or-later */
/* Dallas/Maxim 1-Wire CRC-8 and CRC-16 helpers. */
#include "qemu/osdep.h"
#include "hw/1wire/crc.h"

uint8_t onewire_crc8(uint8_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0x8c : crc >> 1;
        }
    }
    return crc;
}

uint16_t onewire_crc16(uint16_t crc, const void *buf, size_t len)
{
    const uint8_t *p = buf;

    for (size_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (unsigned bit = 0; bit < 8; bit++) {
            crc = (crc & 1) ? (crc >> 1) ^ 0xa001 : crc >> 1;
        }
    }
    return crc;
}
