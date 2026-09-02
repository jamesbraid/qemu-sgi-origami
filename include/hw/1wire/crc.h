/* SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef HW_1WIRE_CRC_H
#define HW_1WIRE_CRC_H

/*
 * Dallas/Maxim 1-Wire checksums. CRC-8 is polynomial 0x31, reflected 0x8c,
 * seeded zero and sent plain; CRC-16 is polynomial 0x8005, reflected 0xa001,
 * seeded zero and sent inverted. Inversion is the caller's, because the two
 * differ and only the caller knows which stream it is building.
 */
uint8_t onewire_crc8(uint8_t crc, const void *buf, size_t len);
uint16_t onewire_crc16(uint16_t crc, const void *buf, size_t len);

#endif
