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
 * No SGI machine we model has a free-standing hex system serial: the
 * L1's system serial (request 0x0008) is the station address as 12 hex
 * digits and follows mac=.
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

typedef struct SGIBoardIdentity {
    uint8_t mac[6];
    uint8_t text_byte;
} SGIBoardIdentity;

/*
 * Fill @id with 08:00:69:(0xe0 | seed[0] & 0xf):seed[1]:seed[2] and
 * seed[5], where seed is the SHA-256 of @key.
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
}

/* "EMUN" and the seed byte plus @node as uppercase hex, modulo 256. */
static inline void sgi_board_text_serial(
    const SGIBoardIdentity *id, unsigned int node,
    char text_serial[SGI_BOARD_TEXT_SERIAL_SIZE])
{
    snprintf(text_serial, SGI_BOARD_TEXT_SERIAL_SIZE, "EMUN%02X",
             (id->text_byte + node) & 0xff);
}

#endif /* HW_MIPS_SGI_SGI_BOARD_IDENTITY_H */
