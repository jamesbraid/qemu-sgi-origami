/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * The default station address of an SGI machine whose launcher gave none.
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
 * These are synthetic values, not any machine's factory identity.
 */

#ifndef HW_MIPS_SGI_SGI_BOARD_IDENTITY_H
#define HW_MIPS_SGI_SGI_BOARD_IDENTITY_H

#include <glib.h>
#include <stdint.h>

typedef struct SGIBoardIdentity {
    uint8_t mac[6];
} SGIBoardIdentity;

/*
 * Fill @id with 08:00:69:(0xe0 | seed[0] & 0xf):seed[1]:seed[2], where seed
 * is the SHA-256 of @key.
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
}

#endif /* HW_MIPS_SGI_SGI_BOARD_IDENTITY_H */
