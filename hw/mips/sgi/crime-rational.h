/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#ifndef HW_MIPS_SGI_CRIME_RATIONAL_H
#define HW_MIPS_SGI_CRIME_RATIONAL_H

/* Fixed capacity is a model work bound, never a wrapping accumulator. */
typedef struct CrimeInteger {
    uint32_t word[16];
} CrimeInteger;

typedef struct CrimeRational {
    CrimeInteger numerator, denominator;
    bool negative;
} CrimeRational;

static inline CrimeInteger crime_integer(uint64_t n)
{
    CrimeInteger r = { .word = { n, n >> 32 } };
    return r;
}

static inline unsigned crime_integer_words(const CrimeInteger *n)
{
    unsigned count = ARRAY_SIZE(n->word);
    while (count && !n->word[count - 1]) {
        count--;
    }
    return count;
}

static inline unsigned crime_integer_bits(const CrimeInteger *n)
{
    unsigned words = crime_integer_words(n);
    return words ? (words - 1) * 32 + 32 - clz32(n->word[words - 1]) : 0;
}

static inline int crime_integer_compare(const CrimeInteger *a,
                                         const CrimeInteger *b)
{
    for (int i = 15; i >= 0; i--) {
        if (a->word[i] != b->word[i]) {
            return a->word[i] > b->word[i] ? 1 : -1;
        }
    }
    return 0;
}

static inline void crime_integer_subtract(CrimeInteger *r,
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

static inline bool crime_integer_add(CrimeInteger *r, const CrimeInteger *a,
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

static inline bool crime_integer_shift(CrimeInteger *r, const CrimeInteger *a,
                                 unsigned shift)
{
    CrimeInteger v = { 0 };
    unsigned bits = crime_integer_bits(a);
    if (bits + shift > 512) {
        return false;
    }
    for (unsigned i = 0; i < bits; i++) {
        if (a->word[i / 32] & (1U << (i % 32))) {
            v.word[(i + shift) / 32] |= 1U << ((i + shift) % 32);
        }
    }
    *r = v;
    return true;
}

static inline bool crime_integer_multiply(CrimeInteger *r,
                                          const CrimeInteger *a,
                                    const CrimeInteger *b)
{
    CrimeInteger v = { 0 };
    unsigned na = crime_integer_words(a), nb = crime_integer_words(b);
    if (na && nb && crime_integer_bits(a) + crime_integer_bits(b) > 513) {
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

static inline void crime_integer_divide(CrimeInteger *q, CrimeInteger *rem,
                                  const CrimeInteger *a, const CrimeInteger *b)
{
    CrimeInteger quotient = { 0 }, remainder = *a, shifted;
    int shift = (int)crime_integer_bits(a) - crime_integer_bits(b);
    g_assert(crime_integer_bits(b));
    if (crime_integer_words(a) <= 2 && crime_integer_words(b) <= 2) {
        uint64_t av = (uint64_t)a->word[1] << 32 | a->word[0];
        uint64_t bv = (uint64_t)b->word[1] << 32 | b->word[0];
        *q = crime_integer(av / bv);
        *rem = crime_integer(av % bv);
        return;
    }
    for (; shift >= 0; shift--) {
        bool fit = crime_integer_shift(&shifted, b, shift);
        g_assert(fit);
        if (crime_integer_compare(&remainder, &shifted) >= 0) {
            crime_integer_subtract(&remainder, &remainder, &shifted);
            quotient.word[shift / 32] |= 1U << (shift % 32);
        }
    }
    *q = quotient;
    *rem = remainder;
}

static inline CrimeInteger crime_integer_gcd(CrimeInteger a, CrimeInteger b)
{
    CrimeInteger q, r;
    while (crime_integer_bits(&b)) {
        crime_integer_divide(&q, &r, &a, &b);
        a = b;
        b = r;
    }
    return a;
}

static inline CrimeInteger crime_integer_quotient(const CrimeInteger *a,
                                           const CrimeInteger *b)
{
    CrimeInteger q, r;
    crime_integer_divide(&q, &r, a, b);
    g_assert(!crime_integer_bits(&r));
    return q;
}

static inline bool crime_rational_valid(const CrimeRational *r)
{
    unsigned bits = crime_integer_bits(&r->numerator);
    if (!crime_integer_bits(&r->denominator)) {
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

static inline bool crime_rational_reduce(CrimeRational *r)
{
    CrimeInteger gcd;
    if (!crime_rational_valid(r)) {
        return false;
    }
    if (!crime_integer_bits(&r->numerator)) {
        r->negative = false;
        r->denominator = crime_integer(1);
        return true;
    }
    gcd = crime_integer_gcd(r->numerator, r->denominator);
    r->numerator = crime_integer_quotient(&r->numerator, &gcd);
    r->denominator = crime_integer_quotient(&r->denominator, &gcd);
    return crime_rational_valid(r);
}

static inline CrimeRational crime_rational(int64_t n, uint64_t d)
{
    CrimeRational r = {
        .numerator = crime_integer(n < 0 ? -(uint64_t)n : n),
        .denominator = crime_integer(d), .negative = n < 0,
    };
    uint64_t magnitude = n < 0 ? -(uint64_t)n : n;
    uint64_t divisor = d;
    g_assert(d);
    while (divisor) {
        uint64_t rem = magnitude % divisor;
        magnitude = divisor;
        divisor = rem;
    }
    r.numerator = crime_integer((n < 0 ? -(uint64_t)n : n) / magnitude);
    r.denominator = crime_integer(d / magnitude);
    return r;
}

static inline bool crime_rational_multiply(CrimeRational *r,
                                          const CrimeRational *a,
                                     const CrimeRational *b)
{
    CrimeInteger ga = crime_integer_gcd(a->numerator, b->denominator);
    CrimeInteger gb = crime_integer_gcd(b->numerator, a->denominator);
    CrimeInteger an = crime_integer_quotient(&a->numerator, &ga);
    CrimeInteger bn = crime_integer_quotient(&b->numerator, &gb);
    CrimeInteger ad = crime_integer_quotient(&a->denominator, &gb);
    CrimeInteger bd = crime_integer_quotient(&b->denominator, &ga);
    CrimeRational v = { .negative = a->negative != b->negative };
    if (!crime_integer_multiply(&v.numerator, &an, &bn) ||
        !crime_integer_multiply(&v.denominator, &ad, &bd) ||
        !crime_rational_valid(&v)) {
        return false;
    }
    if (!crime_integer_bits(&v.numerator)) {
        v.negative = false;
    }
    *r = v;
    return true;
}

static inline bool crime_rational_add(CrimeRational *r, const CrimeRational *a,
                                const CrimeRational *b)
{
    CrimeInteger g = crime_integer_gcd(a->denominator, b->denominator);
    CrimeInteger ad = crime_integer_quotient(&a->denominator, &g);
    CrimeInteger bd = crime_integer_quotient(&b->denominator, &g);
    CrimeInteger an, bn, h, denominator;
    CrimeRational v = { .negative = a->negative,
                        .denominator = crime_integer(1) };
    CrimeRational term = { .denominator = crime_integer(1) };
    if (!crime_integer_multiply(&an, &a->numerator, &bd) ||
        !crime_integer_multiply(&bn, &b->numerator, &ad)) {
        return false;
    }
    term.numerator = an;
    term.negative = a->negative;
    if (!crime_rational_valid(&term)) {
        return false;
    }
    term.numerator = bn;
    term.negative = b->negative;
    if (!crime_rational_valid(&term)) {
        return false;
    }
    if (a->negative == b->negative) {
        if (!crime_integer_add(&v.numerator, &an, &bn)) {
            return false;
        }
    } else if (crime_integer_compare(&an, &bn) >= 0) {
        crime_integer_subtract(&v.numerator, &an, &bn);
    } else {
        crime_integer_subtract(&v.numerator, &bn, &an);
        v.negative = b->negative;
    }
    if (!crime_rational_valid(&v)) {
        return false;
    }
    h = crime_integer_gcd(v.numerator, g);
    v.numerator = crime_integer_quotient(&v.numerator, &h);
    denominator = crime_integer_quotient(&b->denominator, &h);
    if (!crime_integer_multiply(&v.denominator, &ad, &denominator) ||
        !crime_rational_reduce(&v)) {
        return false;
    }
    *r = v;
    return true;
}

static inline bool crime_rational_subtract(CrimeRational *r,
                                          const CrimeRational *a,
                                     const CrimeRational *b)
{
    CrimeRational negative = *b;
    if (crime_integer_bits(&negative.numerator)) {
        negative.negative = !negative.negative;
    }
    return crime_rational_add(r, a, &negative);
}

/* Continued fractions avoid an oversized cross-product during comparison. */
static inline int crime_rational_compare(const CrimeRational *a,
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
        crime_integer_divide(&aq, &ar, &an, &ad);
        crime_integer_divide(&bq, &br, &bn, &bd);
        result = crime_integer_compare(&aq, &bq);
        if (result) {
            return result * direction;
        }
        if (!crime_integer_bits(&ar) || !crime_integer_bits(&br)) {
            return (crime_integer_bits(&ar) ? 1 :
                    crime_integer_bits(&br) ? -1 : 0) * direction;
        }
        an = ad;
        ad = ar;
        bn = bd;
        bd = br;
        direction = -direction;
    }
}

static inline bool crime_rational_floor(const CrimeRational *a, int64_t *out)
{
    CrimeInteger q, rem, one = crime_integer(1);
    uint64_t value;
    crime_integer_divide(&q, &rem, &a->numerator, &a->denominator);
    if (a->negative && crime_integer_bits(&rem) &&
        !crime_integer_add(&q, &q, &one)) {
        return false;
    }
    if (crime_integer_bits(&q) > 64) {
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
