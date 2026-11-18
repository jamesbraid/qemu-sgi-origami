/* SPDX-License-Identifier: GPL-2.0-or-later */
/* DSA signature verification with a 1024-bit modulus and a 160-bit order. */
#ifndef TARGET_IA64_DSA_H
#define TARGET_IA64_DSA_H

#define IA64_DSA_P_BYTES 128
#define IA64_DSA_Q_BYTES 20
#define IA64_DSA_SIG_BYTES 24

typedef struct IA64DsaKey {
    uint8_t p[IA64_DSA_P_BYTES];
    uint8_t g[IA64_DSA_P_BYTES];
    uint8_t y[IA64_DSA_P_BYTES];
    uint8_t q[IA64_DSA_Q_BYTES];
} IA64DsaKey;

/*
 * All integers are little-endian. r and w are 24-byte fields and w is the
 * inverse of s modulo q. digest is the 20-byte SHA-1 of the message.
 */
bool ia64_dsa_verify(const IA64DsaKey *key, const uint8_t digest[20],
                     const uint8_t r[IA64_DSA_SIG_BYTES],
                     const uint8_t w[IA64_DSA_SIG_BYTES]);

#endif
