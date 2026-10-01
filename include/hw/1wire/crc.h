/* SPDX-License-Identifier: BSD-3-Clause */
/* Dallas/Maxim 1-Wire CRC helper declarations. */
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
