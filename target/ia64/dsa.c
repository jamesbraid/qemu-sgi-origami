/* SPDX-License-Identifier: GPL-2.0-or-later */
/*
 * DSA verification. QEMU's crypto API has no DSA verify and this build links
 * no bignum library, so a small unsigned modular exponentiation is kept here.
 * Inputs are public.
 */
#include "qemu/osdep.h"
#include "dsa.h"

#define LIMBS 36 /* 1152 bits: room for a 1024-bit value shifted left once */

typedef struct BN {
    uint32_t l[LIMBS];
} BN;

static void bn_from_le(BN *a, const uint8_t *b, size_t n)
{
    memset(a, 0, sizeof(*a));
    for (size_t i = 0; i < n; i++) {
        a->l[i / 4] |= (uint32_t)b[i] << (8 * (i % 4));
    }
}

static void bn_from_be(BN *a, const uint8_t *b, size_t n)
{
    memset(a, 0, sizeof(*a));
    for (size_t i = 0; i < n; i++) {
        size_t k = n - 1 - i;

        a->l[k / 4] |= (uint32_t)b[i] << (8 * (k % 4));
    }
}

static bool bn_is_zero(const BN *a)
{
    for (int i = 0; i < LIMBS; i++) {
        if (a->l[i]) {
            return false;
        }
    }
    return true;
}

static int bn_cmp(const BN *a, const BN *b)
{
    for (int i = LIMBS - 1; i >= 0; i--) {
        if (a->l[i] != b->l[i]) {
            return a->l[i] > b->l[i] ? 1 : -1;
        }
    }
    return 0;
}

static void bn_sub(BN *a, const BN *b)
{
    int64_t borrow = 0;

    for (int i = 0; i < LIMBS; i++) {
        int64_t t = (int64_t)a->l[i] - b->l[i] - borrow;

        borrow = t < 0;
        a->l[i] = (uint32_t)t;
    }
}

static void bn_shl1_or(BN *a, unsigned bit)
{
    uint32_t carry = bit;

    for (int i = 0; i < LIMBS; i++) {
        uint32_t next = a->l[i] >> 31;

        a->l[i] = (a->l[i] << 1) | carry;
        carry = next;
    }
}

/* r = a * b mod m, by full product and bitwise reduction. */
static void bn_mulmod(BN *r, const BN *a, const BN *b, const BN *m)
{
    uint32_t prod[2 * LIMBS] = { 0 };
    BN rem = { { 0 } };

    for (int i = 0; i < LIMBS; i++) {
        uint64_t carry = 0;

        for (int j = 0; j < LIMBS; j++) {
            uint64_t t = (uint64_t)a->l[i] * b->l[j] + prod[i + j] + carry;

            prod[i + j] = (uint32_t)t;
            carry = t >> 32;
        }
        prod[i + LIMBS] = (uint32_t)carry;
    }
    for (int bit = 2 * LIMBS * 32 - 1; bit >= 0; bit--) {
        bn_shl1_or(&rem, (prod[bit / 32] >> (bit % 32)) & 1);
        if (bn_cmp(&rem, m) >= 0) {
            bn_sub(&rem, m);
        }
    }
    *r = rem;
}

/* r = base ^ exp mod m. */
static void bn_modexp(BN *r, const BN *base, const BN *exp, const BN *m)
{
    BN acc = { { 0 } };

    int top = LIMBS * 32 - 1;

    acc.l[0] = 1;
    while (top >= 0 && !((exp->l[top / 32] >> (top % 32)) & 1)) {
        top--;
    }
    for (int bit = top; bit >= 0; bit--) {
        bn_mulmod(&acc, &acc, &acc, m);
        if ((exp->l[bit / 32] >> (bit % 32)) & 1) {
            bn_mulmod(&acc, &acc, base, m);
        }
    }
    *r = acc;
}

bool ia64_dsa_verify(const IA64DsaKey *key, const uint8_t digest[20],
                     const uint8_t r_le[IA64_DSA_SIG_BYTES],
                     const uint8_t w_le[IA64_DSA_SIG_BYTES])
{
    BN p, g, y, q, r, w, h, u1, u2, a, b, v, one = { { 1 } };

    bn_from_le(&p, key->p, IA64_DSA_P_BYTES);
    bn_from_le(&g, key->g, IA64_DSA_P_BYTES);
    bn_from_le(&y, key->y, IA64_DSA_P_BYTES);
    bn_from_le(&q, key->q, IA64_DSA_Q_BYTES);
    bn_from_le(&r, r_le, IA64_DSA_SIG_BYTES);
    bn_from_le(&w, w_le, IA64_DSA_SIG_BYTES);
    bn_from_be(&h, digest, 20);

    if (bn_is_zero(&q) || bn_cmp(&q, &p) >= 0 || bn_is_zero(&r) ||
        bn_cmp(&r, &q) >= 0 || bn_is_zero(&w) || bn_cmp(&w, &q) >= 0) {
        return false;
    }
    bn_mulmod(&u1, &h, &w, &q);
    bn_mulmod(&u2, &r, &w, &q);
    bn_modexp(&a, &g, &u1, &p);
    bn_modexp(&b, &y, &u2, &p);
    bn_mulmod(&v, &a, &b, &p);
    /* v mod q, by multiplying with 1. */
    bn_mulmod(&v, &v, &one, &q);
    return bn_cmp(&v, &r) == 0;
}
