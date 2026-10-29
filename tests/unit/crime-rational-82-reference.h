/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef TESTS_CRIME_RATIONAL_82_REFERENCE_H
#define TESTS_CRIME_RATIONAL_82_REFERENCE_H
#include "qemu/int128.h"
/* Frozen82f math: only function names and whitespace differ. */
static inline CrimeInteger frozen82_integer(uint64_t n)
{
    CrimeInteger r = { .word = { n, n >> 32 } };
    return r;
}

static inline unsigned frozen82_integer_words(const CrimeInteger *n)
{
    unsigned count = ARRAY_SIZE(n->word);
    while (count && !n->word[count - 1]) {
        count--;
    }
    return count;
}

static inline unsigned frozen82_integer_bits(const CrimeInteger *n)
{
    unsigned words = frozen82_integer_words(n);
    return words ? (words - 1) * 32 + 32 - clz32(n->word[words - 1]) : 0;
}

static inline int frozen82_integer_compare(const CrimeInteger *a,
                                         const CrimeInteger *b)
{
    for (int i = 15; i >= 0; i--) {
        if (a->word[i] != b->word[i]) {
            return a->word[i] > b->word[i] ? 1 : -1;
        }
    }
    return 0;
}

static inline void frozen82_integer_subtract(CrimeInteger *r,
                                          const CrimeInteger *a,
                                    const CrimeInteger *b)
{
    uint64_t borrow = 0;
    CrimeInteger v;
    for (unsigned i = 0; i < 16; i++) {
        uint64_t sub = b->word[i] + borrow;
        v.word[i] = a->word[i] - sub;
        borrow = a->word[i] < sub;
    }
    *r = v;
}

static inline bool frozen82_integer_add(CrimeInteger *r, const CrimeInteger *a,
                               const CrimeInteger *b)
{
    uint64_t carry = 0;
    CrimeInteger v;
    for (unsigned i = 0; i < 16; i++) {
        carry += (uint64_t)a->word[i] + b->word[i];
        v.word[i] = carry;
        carry >>= 32;
    }
    if (carry) {
        return false;
    }
    *r = v;
    return true;
}

static inline bool frozen82_integer_shift(CrimeInteger *r,
                                           const CrimeInteger *a,
                                           unsigned shift)
{
    CrimeInteger v = { 0 };
    unsigned bits = frozen82_integer_bits(a), part = shift % 32;
    unsigned whole = shift / 32;

    if (shift > 512 || bits > 512 - shift) {
        return false;
    }
    for (unsigned i = 0; i < DIV_ROUND_UP(bits, 32); i++) {
        unsigned index = i + whole;
        v.word[index] |= a->word[i] << part;
        if (part && index + 1 < ARRAY_SIZE(v.word)) {
            v.word[index + 1] = a->word[i] >> (32 - part);
        }
    }
    *r = v;
    return true;
}

static inline bool frozen82_integer_multiply(CrimeInteger *r,
                                          const CrimeInteger *a,
                                    const CrimeInteger *b)
{
    CrimeInteger v = { 0 };
    unsigned na = frozen82_integer_words(a), nb = frozen82_integer_words(b);
    if (na && nb && frozen82_integer_bits(a) + frozen82_integer_bits(b) > 513) {
        return false;
    }
    for (unsigned i = 0; i < na; i++) {
        uint64_t carry = 0;
        for (unsigned j = 0; j < nb; j++) {
            uint64_t product = (uint64_t)a->word[i] * b->word[j];
            if (i + j >= 16) {
                if (product || carry) {
                    return false;
                }
                continue;
            }
            carry += product + v.word[i + j];
            v.word[i + j] = carry;
            carry >>= 32;
        }
        if (carry) {
            if (i + nb >= 16) {
                return false;
            }
            v.word[i + nb] = carry;
        }
    }
    *r = v;
    return true;
}

static inline CrimeInteger frozen82_integer_from128(Int128 value)
{
    uint64_t low = int128_getlo(value), high = int128_gethi(value);
    CrimeInteger result = { .word = {low, low >> 32, high, high >> 32} };
    return result;
}

static inline void frozen82_integer_divide(CrimeInteger *q, CrimeInteger *rem,
                                  const CrimeInteger *a, const CrimeInteger *b)
{
    CrimeInteger quotient = { 0 }, remainder = *a, shifted;
    unsigned na = frozen82_integer_words(a), nb = frozen82_integer_words(b);
    unsigned divisor_bits;
    int shift;

    g_assert(nb);
    if (na <= 2 && nb <= 2) {
        uint64_t av = (uint64_t)a->word[1] << 32 | a->word[0];
        uint64_t bv = (uint64_t)b->word[1] << 32 | b->word[0];
        *q = frozen82_integer(av / bv);
        *rem = frozen82_integer(av % bv);
        return;
    }
    if (na <= 4 && nb <= 4) {
        Int128 av = int128_make128(
            (uint64_t)a->word[1] << 32 | a->word[0],
            (uint64_t)a->word[3] << 32 | a->word[2]);
        Int128 bv = int128_make128(
            (uint64_t)b->word[1] << 32 | b->word[0],
            (uint64_t)b->word[3] << 32 | b->word[2]);
        Int128 qv = int128_divu(av, bv), rv = int128_remu(av, bv);
        *q = frozen82_integer_from128(qv);
        *rem = frozen82_integer_from128(rv);
        return;
    }
    if (nb <= 2) {
        uint64_t divisor = (uint64_t)b->word[1] << 32 | b->word[0];
        uint64_t carry = 0;

        /* Each carried remainder is smaller than the native divisor. */
        for (int i = (na - 1) / 2; i >= 0; i--) {
            uint64_t low = (uint64_t)a->word[2 * i + 1] << 32 |
                           a->word[2 * i];
            uint64_t high = carry;
            carry = divu128(&low, &high, divisor);
            g_assert(!high);
            quotient.word[2 * i] = low;
            quotient.word[2 * i + 1] = low >> 32;
        }
        *q = quotient;
        *rem = frozen82_integer(carry);
        return;
    }
    divisor_bits = (nb - 1) * 32 + 32 - clz32(b->word[nb - 1]);
    shift = (int)frozen82_integer_bits(a) - (int)divisor_bits;
    if (shift >= 0) {
        bool fit = frozen82_integer_shift(&shifted, b, shift);
        g_assert(fit);
    }
    for (; shift >= 0; shift--) {
        unsigned words = DIV_ROUND_UP(divisor_bits + shift, 32);
        uint32_t carry = 0;

        if (frozen82_integer_compare(&remainder, &shifted) >= 0) {
            frozen82_integer_subtract(&remainder, &remainder, &shifted);
            quotient.word[shift / 32] |= 1U << (shift % 32);
        }
        /* Shift the same exact divisor instead of rebuilding every bit. */
        for (int i = words - 1; i >= 0; i--) {
            uint32_t next = shifted.word[i] << 31;
            shifted.word[i] = (shifted.word[i] >> 1) | carry;
            carry = next;
        }
    }
    *q = quotient;
    *rem = remainder;
}

static inline uint64_t frozen82_gcd64(uint64_t a, uint64_t b)
{
    while (b) {
        uint64_t r = a % b;
        a = b;
        b = r;
    }
    return a;
}

static inline Int128 frozen82_gcd128(Int128 a, Int128 b)
{
    while (int128_nz(b)) {
        if (!(int128_gethi(a) | int128_gethi(b))) {
            return int128_make64(frozen82_gcd64(int128_getlo(a),
                                                int128_getlo(b)));
        }
        Int128 r = int128_remu(a, b);
        a = b;
        b = r;
    }
    return a;
}

static inline CrimeInteger frozen82_integer_gcd(CrimeInteger a, CrimeInteger b)
{
    CrimeInteger q, r;
    unsigned nb;

    for (;;) {
        nb = frozen82_integer_words(&b);
        if (!nb) {
            return a;
        }
        if (nb <= 4) {
            unsigned na = frozen82_integer_words(&a);
            if (na <= 2 && nb <= 2) {
                uint64_t av = (uint64_t)a.word[1] << 32 | a.word[0];
                uint64_t bv = (uint64_t)b.word[1] << 32 | b.word[0];
                return frozen82_integer(frozen82_gcd64(av, bv));
            }
            if (na <= 4) {
                Int128 av = int128_make128(
                    (uint64_t)a.word[1] << 32 | a.word[0],
                    (uint64_t)a.word[3] << 32 | a.word[2]);
                Int128 bv = int128_make128(
                    (uint64_t)b.word[1] << 32 | b.word[0],
                    (uint64_t)b.word[3] << 32 | b.word[2]);
                return frozen82_integer_from128(frozen82_gcd128(av, bv));
            }
        }
        frozen82_integer_divide(&q, &r, &a, &b);
        a = b;
        b = r;
    }
    return a;
}

static inline CrimeInteger frozen82_integer_quotient(const CrimeInteger *a,
                                           const CrimeInteger *b)
{
    CrimeInteger q, r;
    frozen82_integer_divide(&q, &r, a, b);
    g_assert(!frozen82_integer_bits(&r));
    return q;
}

static inline bool frozen82_rational_valid(const CrimeRational *r)
{
    unsigned bits = frozen82_integer_bits(&r->numerator);
    if (!frozen82_integer_bits(&r->denominator)) {
        return false;
    }
    if (bits <= 511) {
        return true;
    }
    if (!r->negative || r->numerator.word[15] != 0x80000000U) {
        return false;
    }
    for (unsigned i = 0; i < 15; i++) {
        if (r->numerator.word[i]) {
            return false;
        }
    }
    return true;
}

static inline bool frozen82_rational_reduce(CrimeRational *r)
{
    CrimeInteger gcd;
    if (!frozen82_rational_valid(r)) {
        return false;
    }
    if (!frozen82_integer_bits(&r->numerator)) {
        r->negative = false;
        r->denominator = frozen82_integer(1);
        return true;
    }
    gcd = frozen82_integer_gcd(r->numerator, r->denominator);
    r->numerator = frozen82_integer_quotient(&r->numerator, &gcd);
    r->denominator = frozen82_integer_quotient(&r->denominator, &gcd);
    return frozen82_rational_valid(r);
}

static inline CrimeRational frozen82_rational(int64_t n, uint64_t d)
{
    CrimeRational r = {
        .numerator = frozen82_integer(n < 0 ? -(uint64_t)n : n),
        .denominator = frozen82_integer(d), .negative = n < 0,
    };
    uint64_t magnitude = n < 0 ? -(uint64_t)n : n;
    uint64_t divisor = d;
    g_assert(d);
    while (divisor) {
        uint64_t rem = magnitude % divisor;
        magnitude = divisor;
        divisor = rem;
    }
    r.numerator = frozen82_integer((n < 0 ? -(uint64_t)n : n) / magnitude);
    r.denominator = frozen82_integer(d / magnitude);
    return r;
}

static inline uint64_t frozen82_integer_low64(const CrimeInteger *a)
{
    return (uint64_t)a->word[1] << 32 | a->word[0];
}

static inline bool frozen82_rational_is64(const CrimeRational *a)
{
    return (a->denominator.word[0] | a->denominator.word[1]) &&
           frozen82_integer_words(&a->numerator) <= 2 &&
           frozen82_integer_words(&a->denominator) <= 2;
}

static inline Int128 frozen82_product64(uint64_t a, uint64_t b)
{
    uint64_t low, high;
    mulu64(&low, &high, a, b);
    return int128_make128(low, high);
}

static inline void frozen82_rational_multiply64(CrimeRational *r,
                                             const CrimeRational *a,
                                             const CrimeRational *b)
{
    uint64_t an = frozen82_integer_low64(&a->numerator);
    uint64_t bn = frozen82_integer_low64(&b->numerator);
    uint64_t ad = frozen82_integer_low64(&a->denominator);
    uint64_t bd = frozen82_integer_low64(&b->denominator);
    uint64_t ga = frozen82_gcd64(an, bd), gb = frozen82_gcd64(bn, ad);
    Int128 n = frozen82_product64(an / ga, bn / gb);
    Int128 d = frozen82_product64(ad / gb, bd / ga);
    CrimeRational v = {
        .numerator = frozen82_integer_from128(n),
        .denominator = frozen82_integer_from128(d),
        .negative = int128_nz(n) && (a->negative != b->negative),
    };
    *r = v;
}

static inline bool frozen82_rational_add64(CrimeRational *r,
                                        const CrimeRational *a,
                                        const CrimeRational *b)
{
    uint64_t an = frozen82_integer_low64(&a->numerator);
    uint64_t bn = frozen82_integer_low64(&b->numerator);
    uint64_t ad = frozen82_integer_low64(&a->denominator);
    uint64_t bd = frozen82_integer_low64(&b->denominator);
    uint64_t g = frozen82_gcd64(ad, bd);
    Int128 left = frozen82_product64(an, bd / g);
    Int128 right = frozen82_product64(bn, ad / g);
    Int128 n, d = frozen82_product64(ad / g, bd), common;
    bool negative = a->negative;
    CrimeRational v;

    if (a->negative == b->negative) {
        uint64_t al = int128_getlo(left), bl = int128_getlo(right);
        uint64_t ah = int128_gethi(left), bh = int128_gethi(right);
        uint64_t low = al + bl, high = ah + bh, carry = low < al;
        if (high < ah || high + carry < high) {
            return false;
        }
        n = int128_make128(low, high + carry);
    } else if (int128_ult(left, right)) {
        n = int128_sub(right, left);
        negative = b->negative;
    } else {
        n = int128_sub(left, right);
    }
    common = frozen82_gcd128(n, d);
    v = (CrimeRational) {
        .numerator = frozen82_integer_from128(int128_divu(n, common)),
        .denominator = frozen82_integer_from128(int128_divu(d, common)),
        .negative = int128_nz(n) && negative,
    };
    *r = v;
    return true;
}

static inline bool frozen82_rational_multiply(CrimeRational *r,
                                          const CrimeRational *a,
                                     const CrimeRational *b)
{
    /* Every old cross-product is at most 128 bits under this guard. */
    if (frozen82_rational_is64(a) && frozen82_rational_is64(b)) {
        frozen82_rational_multiply64(r, a, b);
        return true;
    }
    CrimeInteger ga = frozen82_integer_gcd(a->numerator, b->denominator);
    CrimeInteger gb = frozen82_integer_gcd(b->numerator, a->denominator);
    CrimeInteger an = frozen82_integer_quotient(&a->numerator, &ga);
    CrimeInteger bn = frozen82_integer_quotient(&b->numerator, &gb);
    CrimeInteger ad = frozen82_integer_quotient(&a->denominator, &gb);
    CrimeInteger bd = frozen82_integer_quotient(&b->denominator, &ga);
    CrimeRational v = { .negative = a->negative != b->negative };
    if (!frozen82_integer_multiply(&v.numerator, &an, &bn) ||
        !frozen82_integer_multiply(&v.denominator, &ad, &bd) ||
        !frozen82_rational_valid(&v)) {
        return false;
    }
    if (!frozen82_integer_bits(&v.numerator)) {
        v.negative = false;
    }
    *r = v;
    return true;
}

static inline bool frozen82_rational_equal_denominator(
    CrimeRational *r, const CrimeRational *a, const CrimeRational *b)
{
    CrimeRational v = { .denominator = a->denominator,
                        .negative = a->negative };

    /* Old cross-products are exactly the valid input numerators times one. */
    if (a->negative == b->negative) {
        if (!frozen82_integer_add(&v.numerator, &a->numerator, &b->numerator)) {
            return false;
        }
    } else if (frozen82_integer_compare(&a->numerator, &b->numerator) >= 0) {
        frozen82_integer_subtract(&v.numerator, &a->numerator, &b->numerator);
    } else {
        frozen82_integer_subtract(&v.numerator, &b->numerator, &a->numerator);
        v.negative = b->negative;
    }
    /* Reject an overwide signed sum before removing common factors. */
    if (!frozen82_rational_valid(&v) || !frozen82_rational_reduce(&v)) {
        return false;
    }
    *r = v;
    return true;
}

static inline bool frozen82_rational_add(CrimeRational *r,
                                          const CrimeRational *a,
                                          const CrimeRational *b)
{
    /* Old terms fit 128 bits and their sum fits 129, safely below 512. */
    if (frozen82_rational_is64(a) && frozen82_rational_is64(b) &&
        frozen82_rational_add64(r, a, b)) {
        return true;
    }
    if (!frozen82_integer_compare(&a->denominator, &b->denominator) &&
        frozen82_rational_valid(a) && frozen82_rational_valid(b)) {
        return frozen82_rational_equal_denominator(r, a, b);
    }
    CrimeInteger g = frozen82_integer_gcd(a->denominator, b->denominator);
    CrimeInteger ad = frozen82_integer_quotient(&a->denominator, &g);
    CrimeInteger bd = frozen82_integer_quotient(&b->denominator, &g);
    CrimeInteger an, bn, h, denominator;
    CrimeRational v = { .negative = a->negative,
                        .denominator = frozen82_integer(1) };
    CrimeRational term = { .denominator = frozen82_integer(1) };
    if (!frozen82_integer_multiply(&an, &a->numerator, &bd) ||
        !frozen82_integer_multiply(&bn, &b->numerator, &ad)) {
        return false;
    }
    term.numerator = an;
    term.negative = a->negative;
    if (!frozen82_rational_valid(&term)) {
        return false;
    }
    term.numerator = bn;
    term.negative = b->negative;
    if (!frozen82_rational_valid(&term)) {
        return false;
    }
    if (a->negative == b->negative) {
        if (!frozen82_integer_add(&v.numerator, &an, &bn)) {
            return false;
        }
    } else if (frozen82_integer_compare(&an, &bn) >= 0) {
        frozen82_integer_subtract(&v.numerator, &an, &bn);
    } else {
        frozen82_integer_subtract(&v.numerator, &bn, &an);
        v.negative = b->negative;
    }
    if (!frozen82_rational_valid(&v)) {
        return false;
    }
    h = frozen82_integer_gcd(v.numerator, g);
    v.numerator = frozen82_integer_quotient(&v.numerator, &h);
    denominator = frozen82_integer_quotient(&b->denominator, &h);
    if (!frozen82_integer_multiply(&v.denominator, &ad, &denominator) ||
        !frozen82_rational_reduce(&v)) {
        return false;
    }
    *r = v;
    return true;
}

static inline bool frozen82_rational_subtract(CrimeRational *r,
                                          const CrimeRational *a,
                                     const CrimeRational *b)
{
    CrimeRational negative = *b;
    if (frozen82_integer_bits(&negative.numerator)) {
        negative.negative = !negative.negative;
    }
    return frozen82_rational_add(r, a, &negative);
}

/* Continued fractions avoid an oversized cross-product during comparison. */
static inline int frozen82_rational_compare(const CrimeRational *a,
                                   const CrimeRational *b)
{
    CrimeInteger an = a->numerator, ad = a->denominator;
    CrimeInteger bn = b->numerator, bd = b->denominator;
    int direction = a->negative ? -1 : 1;
    if (a->negative != b->negative) {
        return a->negative ? -1 : 1;
    }
    for (;;) {
        CrimeInteger aq, ar, bq, br;
        int result;
        frozen82_integer_divide(&aq, &ar, &an, &ad);
        frozen82_integer_divide(&bq, &br, &bn, &bd);
        result = frozen82_integer_compare(&aq, &bq);
        if (result) {
            return result * direction;
        }
        if (!frozen82_integer_bits(&ar) || !frozen82_integer_bits(&br)) {
            return (frozen82_integer_bits(&ar) ? 1 :
                    frozen82_integer_bits(&br) ? -1 : 0) * direction;
        }
        an = ad;
        ad = ar;
        bn = bd;
        bd = br;
        direction = -direction;
    }
}

static inline bool frozen82_rational_floor(const CrimeRational *a, int64_t *out)
{
    CrimeInteger q, rem, one = frozen82_integer(1);
    uint64_t value;
    frozen82_integer_divide(&q, &rem, &a->numerator, &a->denominator);
    if (a->negative && frozen82_integer_bits(&rem) &&
        !frozen82_integer_add(&q, &q, &one)) {
        return false;
    }
    if (frozen82_integer_bits(&q) > 64) {
        return false;
    }
    value = ((uint64_t)q.word[1] << 32) | q.word[0];
    if (value > (a->negative ? UINT64_C(1) << 63 : INT64_MAX)) {
        return false;
    }
    *out = a->negative ? -(uint64_t)value : value;
    return true;
}

#endif
