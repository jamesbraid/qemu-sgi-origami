/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The default station address and system serial of an SGI machine whose
 * launcher gave neither.
 *
 * All of it comes from one SHA-256 seed over a key, so two machines that
 * differ in their disk get different identities with nothing to register or
 * count. The key is the canonical path of the machine's system disk image,
 * or the machine type name when it has none.
 *
 * The address keeps SGI's 08:00:69 prefix, which the firmware checks. The
 * top nibble of the next byte is 0xe, so these addresses sit in a block
 * real boards do not use and read as the emulator's.
 *
 * A six-character text serial, the system board's in the L1's board areas
 * and the L1 brick serial, is "EMUN" and one hex byte of the seed. Node n
 * adds n to that byte. Neither is derived from the address.
 *
 * The system serial, which the L1 answers to request 0x0008 on the
 * Origin 300 and which the guest's system-id call and FLEXlm's host id
 * read, is "L" and seven digits: seed bytes 6 to 9, big-endian, modulo
 * 3999999, plus one. The L2's serial check documents the form as a letter
 * and seven digits up to 3999999, and the guest packs the digits into its
 * system id, so a marked "EMU" text would not survive it. The Fuel's L1
 * answers the same request with its station address as 12 hex digits and
 * follows mac= instead.
 *
 * The O2's real-time clock carries a 48-bit silicon serial number, seed
 * bytes 14 to 19 big-endian, never zero.
 *
 * These are synthetic values, not any machine's factory identity.
 */

#ifndef HW_MIPS_SGI_SGI_BOARD_IDENTITY_H
#define HW_MIPS_SGI_SGI_BOARD_IDENTITY_H

#include <glib.h>
#include <stdint.h>
#include <stdio.h>

/* Room for "EMUNxx" and the terminator. */
#define SGI_BOARD_TEXT_SERIAL_SIZE 7

/* Room for "Lnnnnnnn" and the terminator. */
#define SGI_BOARD_SYSTEM_SERIAL_SIZE 9

/* Room for "Knnnnnnnnn" and the terminator. */
#define SGI_BOARD_MODULE_SERIAL_SIZE 11

typedef struct SGIBoardIdentity {
    uint8_t mac[6];
    uint8_t text_byte;
    uint32_t system_serial;
    uint32_t module_serial;
    uint64_t silicon_serial;
} SGIBoardIdentity;

/*
 * Fill @id with 08:00:69:(0xe0 | seed[0] & 0xf):seed[1]:seed[2], seed[5]
 * the system serial number from seed[6..9], the module serial from
 * seed[10..13] and the silicon serial from seed[14..19], where seed is the
 * SHA-256 of @key.
 */
static inline void sgi_board_identity_from_key(const char *key,
                                               SGIBoardIdentity *id)
{
    uint8_t seed[32];
    gsize length = sizeof(seed);
    GChecksum *sum = g_checksum_new(G_CHECKSUM_SHA256);

    g_checksum_update(sum, (const guchar *)key, -1);
    g_checksum_get_digest(sum, seed, &length);
    g_checksum_free(sum);

    id->mac[0] = 0x08;
    id->mac[1] = 0x00;
    id->mac[2] = 0x69;
    id->mac[3] = 0xe0 | (seed[0] & 0x0f);
    id->mac[4] = seed[1];
    id->mac[5] = seed[2];
    id->text_byte = seed[5];
    id->system_serial = ((uint32_t)seed[6] << 24 | seed[7] << 16 |
                         seed[8] << 8 | seed[9]) % 3999999 + 1;
    id->module_serial = (uint32_t)seed[10] << 24 | seed[11] << 16 |
                        seed[12] << 8 | seed[13];
    /* Seed bytes 14 to 19, big-endian; never zero, which means "unset". */
    id->silicon_serial = 0;
    for (int i = 14; i < 20; i++) {
        id->silicon_serial = id->silicon_serial << 8 | seed[i];
    }
    if (!id->silicon_serial) {
        id->silicon_serial = 1;
    }
}

/* "EMUN" and the seed byte plus @node as uppercase hex, modulo 256. */
static inline void sgi_board_text_serial(
    const SGIBoardIdentity *id, unsigned int node,
    char text_serial[SGI_BOARD_TEXT_SERIAL_SIZE])
{
    snprintf(text_serial, SGI_BOARD_TEXT_SERIAL_SIZE, "EMUN%02X",
             (id->text_byte + node) & 0xff);
}

/* "L" and the seven-digit system serial number. */
static inline void sgi_board_system_serial(
    const SGIBoardIdentity *id, char text[SGI_BOARD_SYSTEM_SERIAL_SIZE])
{
    snprintf(text, SGI_BOARD_SYSTEM_SERIAL_SIZE, "L%07u", id->system_serial);
}

/*
 * The ten-character module serial the Origin 200, Origin 2000 and Onyx2
 * modules carry: "K", eight digits from seed bytes 10 to 13 plus @module
 * modulo 10^8, and the module's last digit, so every module of one machine
 * differs and the guest's system id differs between machines.
 */
static inline void sgi_board_module_serial(
    const SGIBoardIdentity *id, unsigned int module,
    char text[SGI_BOARD_MODULE_SERIAL_SIZE])
{
    snprintf(text, SGI_BOARD_MODULE_SERIAL_SIZE, "K%08u%u",
             (unsigned)((id->module_serial + module) % 100000000u),
             module % 10);
}

#endif /* HW_MIPS_SGI_SGI_BOARD_IDENTITY_H */
