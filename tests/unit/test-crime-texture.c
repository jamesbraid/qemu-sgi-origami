/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/bswap.h"
#include "hw/mips/sgi/crime-rational.h"
#include "hw/mips/sgi/crime-texture.h"
#include "crime-rational-reference.h"
#include "crime-rational-vectors.h"

#ifdef CRIME_TEXTURE_ALLOCATION_WRAP
static int allocation_failure = -1;
static unsigned allocation_count;
void *__real_g_try_malloc(gsize size);
void *__real_g_try_malloc0(gsize size);
void *__real_g_try_malloc0_n(gsize count, gsize size);
void *__wrap_g_try_malloc(gsize size);
void *__wrap_g_try_malloc0(gsize size);
void *__wrap_g_try_malloc0_n(gsize count, gsize size);

static bool fail_allocation(void)
{
    return allocation_failure >= 0 &&
           allocation_count++ == (unsigned)allocation_failure;
}

void *__wrap_g_try_malloc(gsize size)
{
    return fail_allocation() ? NULL : __real_g_try_malloc(size);
}

void *__wrap_g_try_malloc0(gsize size)
{
    return fail_allocation() ? NULL : __real_g_try_malloc0(size);
}

void *__wrap_g_try_malloc0_n(gsize count, gsize size)
{
    return fail_allocation() ? NULL : __real_g_try_malloc0_n(count, size);
}
#endif

static void test_rational_small(void)
{
    for (int a = -12; a <= 12; a++) {
        for (unsigned b = 1; b <= 11; b++) {
            CrimeRational left = crime_rational(a, b);
            for (int c = -9; c <= 9; c++) {
                for (unsigned d = 1; d <= 7; d++) {
                    CrimeRational right = crime_rational(c, d), sum, product;
                    CrimeRational expected_sum = crime_rational((int64_t)a * d +
                                                                (int64_t)c * b,
                                                                b * d);
                    CrimeRational expected_product = crime_rational(a * c,
                                                                    b * d);
                    int64_t floor = 0;
                    int64_t expected_floor = a < 0 ?
                        -((-(int64_t)a + b - 1) / b) : a / b;
                    g_assert_true(crime_rational_add(&sum, &left, &right));
                    g_assert_true(crime_rational_multiply(&product, &left,
                                                          &right));
                    g_assert_cmpint(crime_rational_compare(
                        &sum, &expected_sum), ==, 0);
                    g_assert_cmpint(crime_rational_compare(
                        &product, &expected_product), ==, 0);
                    g_assert_true(crime_rational_floor(&left, &floor));
                    g_assert_cmpint(floor, ==, expected_floor);
                }
            }
        }
    }
}

static void test_rational_limits(void)
{
    CrimeInteger one = crime_integer(1);
    CrimeRational a = crime_rational(1, 1), b = a, result;
    CrimeRational identity = crime_rational(1, 1), twice = crime_rational(2, 1);

    g_assert_true(crime_integer_shift(&a.numerator, &one, 255));
    g_assert_true(crime_integer_shift(&b.numerator, &one, 256));
    g_assert_false(crime_rational_multiply(&result, &a, &b));
    a.negative = true;
    g_assert_true(crime_rational_multiply(&result, &a, &b));
    g_assert_cmpuint(crime_integer_bits(&result.numerator), ==, 512);
    g_assert_false(crime_rational_multiply(&result, &result, &twice));

    a = crime_rational(1, 1);
    b = a;
    g_assert_true(crime_integer_shift(&a.numerator, &one, 509));
    g_assert_true(crime_integer_shift(&a.denominator, &one, 510));
    g_assert_true(crime_integer_add(&a.denominator, &a.denominator, &one));
    b.numerator = a.denominator;
    b.denominator = a.numerator;
    g_assert_true(crime_rational_multiply(&result, &a, &b));
    g_assert_cmpint(crime_rational_compare(&result, &identity), ==, 0);

    g_assert_true(crime_integer_shift(&b.numerator, &one, 510));
    g_assert_true(crime_integer_shift(&b.denominator, &one, 511));
    g_assert_true(crime_integer_add(&b.denominator, &b.denominator, &one));
    g_assert_cmpint(crime_rational_compare(&b, &a), >, 0);
    a = crime_rational(INT64_MIN, 1);
    {
        int64_t floor = 0;
        g_assert_true(crime_rational_floor(&a, &floor));
        g_assert_cmpint(floor, ==, INT64_MIN);
    }
}

static bool texture_ram(void *opaque, uint64_t address, unsigned size,
                         uint64_t *identity, uint8_t *data)
{
    const uint8_t *ram = opaque;
    if (address < 0xa0000 || address + size > 0xb0000) {
        return false;
    }
    *identity = address - 0xa0000;
    memcpy(data, ram + *identity, size);
    return true;
}

static void test_point_constant(void)
{
    const unsigned derivatives[] = {0x138, 0x140, 0x148, 0x150, 0x158, 0x15c};
    uint64_t p[128] = { 0 }, tlb[28] = { 0 };
    g_autofree uint8_t *ram = g_malloc0(65536);

    p[0x110 / 4] = (2U << 20) | (2U << 18) | (10U << 14) |
                   (2U << 4) | (2U << 2) | 3U;
    p[0x118 / 4] = UINT64_C(0xaa00000f000f);
    p[0x120 / 4] = p[0x128 / 4] = (UINT64_C(1) << 43) + 0x800;
    p[0x130 / 4] = 0x20000800;
    tlb[93 / 4] = UINT64_C(0x800a) << 32;
    stl_be_p(ram + 0xfc00, 0x2371b9d5);
    for (unsigned omitted = 0; omitted < ARRAY_SIZE(derivatives); omitted++) {
        /* A preceding command leaves one nonzero derivative behind. */
        p[derivatives[omitted] / 4] = 1;
        g_autoptr(CrimeTexture) stale = crime_texture_new(
            p, tlb, 0, 0, true, texture_ram, ram);
        g_assert_null(stale);
        p[derivatives[omitted] / 4] = 0;
    }
#ifdef CRIME_TEXTURE_ALLOCATION_WRAP
    for (unsigned failure = 0; failure < 3; failure++) {
        CrimeTexture *t;
        uint32_t color = 0x12345678;
        bool sampled = false;
        allocation_failure = failure;
        allocation_count = 0;
        t = crime_texture_new(p, tlb, 0, 0, true, texture_ram, ram);
        if (t) {
            sampled = crime_texture_sample(t, 0, 0, &color);
        }
        allocation_failure = -1;
        g_assert_false(sampled);
        g_assert_cmphex(color, ==, 0x12345678);
        crime_texture_free(t);
    }
#endif
#ifdef CRIME_TEXTURE_TEST
    for (unsigned limit = 0; limit < 3; limit++) {
        g_autoptr(CrimeTexture) t = crime_texture_new(
            p, tlb, 0, 0, true, texture_ram, ram);
        uint32_t color = 0x12345678;
        g_assert_nonnull(t);
        crime_texture_test_work(t,
            limit == 0 ? CRIME_TEXTURE_MAX_FRAGMENTS : 0,
            limit == 1 ? CRIME_TEXTURE_MAX_TAPS : 0,
            limit == 2 ? CRIME_TEXTURE_MAX_SNAPSHOT : 0);
        g_assert_false(crime_texture_sample(t, 0, 0, &color));
        g_assert_cmphex(color, ==, 0x12345678);
        g_assert_cmphex(ldl_be_p(ram + 0xfc00), ==, 0x2371b9d5);
    }
#endif
    {
        g_autoptr(CrimeTexture) t = crime_texture_new(
            p, tlb, 17, -31, true, texture_ram, ram);
        g_assert_nonnull(t);
        for (int x = -4; x <= 4; x++) {
            uint32_t color = 0x12345678;
            g_assert_true(crime_texture_sample(t, x, 3 * x, &color));
            g_assert_cmphex(color, ==, 0x2371b9d5);
        }
    }
}

static void assert_integer(const CrimeInteger *a, const CrimeInteger *b)
{
    g_assert_cmpmem(a, sizeof(*a), b, sizeof(*b));
}

static void assert_rational(const CrimeRational *a, const CrimeRational *b)
{
    assert_integer(&a->numerator, &b->numerator);
    assert_integer(&a->denominator, &b->denominator);
    g_assert_cmpint(a->negative, ==, b->negative);
}

static void test_division_vectors(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(crime_division_vectors); i++) {
        const CrimeDivisionVector *v = &crime_division_vectors[i];
        CrimeInteger q, r, gcd;
        g_test_message("independent division/GCD vector %u", i);
        crime_integer_divide(&q, &r, &v->a, &v->b);
        assert_integer(&q, &v->quotient);
        assert_integer(&r, &v->remainder);
        gcd = crime_integer_gcd(v->a, v->b);
        assert_integer(&gcd, &v->gcd);
        reference_integer_divide(&q, &r, &v->a, &v->b);
        assert_integer(&q, &v->quotient);
        assert_integer(&r, &v->remainder);
    }
}

static void test_rational_vectors(void)
{
    for (unsigned i = 0; i < ARRAY_SIZE(crime_rational_vectors); i++) {
        const CrimeRationalVector *v = &crime_rational_vectors[i];
        CrimeRational a = crime_rational(7, 11), b = a, sentinel = a;
        bool actual, reference;
        g_test_message("independent rational vector %u", i);
        actual = crime_rational_add(&a, &v->a, &v->b);
        reference = reference_rational_add(&b, &v->a, &v->b);
        g_assert_cmpint(actual, ==, v->add_allowed);
        g_assert_cmpint(actual, ==, reference);
        assert_rational(&a, actual ? &v->sum : &sentinel);
        assert_rational(&a, &b);
        a = b = sentinel;
        actual = crime_rational_multiply(&a, &v->a, &v->b);
        reference = reference_rational_multiply(&b, &v->a, &v->b);
        g_assert_cmpint(actual, ==, v->multiply_allowed);
        g_assert_cmpint(actual, ==, reference);
        assert_rational(&a, actual ? &v->product : &sentinel);
        assert_rational(&a, &b);
    }
}

static uint64_t math_random(uint64_t *state)
{
    *state ^= *state << 13;
    *state ^= *state >> 7;
    *state ^= *state << 17;
    return *state;
}

static CrimeInteger math_integer(uint64_t *state, unsigned bits)
{
    CrimeInteger result = { 0 };
    for (unsigned i = 0; i < DIV_ROUND_UP(bits, 32); i++) {
        result.word[i] = math_random(state);
    }
    if (bits % 32) {
        result.word[bits / 32] &= (1U << (bits % 32)) - 1;
    }
    if (bits) {
        result.word[(bits - 1) / 32] |= 1U << ((bits - 1) % 32);
    }
    return result;
}

static void test_math_reference(void)
{
    const unsigned shifts[] = {0, 1, 31, 32, 63, 64, 127, 128, 511, 512, 513};
    uint64_t seed = UINT64_C(0x5121284352494d45);
    CrimeInteger zero = crime_integer(0), gcd;
    gcd = crime_integer_gcd(zero, zero);
    assert_integer(&gcd, &zero);
    for (unsigned i = 0; i < 256; i++) {
        unsigned bits = i * 73 % 513;
        CrimeInteger a = math_integer(&seed, bits);
        CrimeInteger b = math_integer(&seed, i * 137 % 512 + 1);
        CrimeInteger q, r, expected_q, expected_r;
        CrimeInteger alias_a = a, alias_b = b;
        g_test_message("reference vector %u: %u/%u bits", i, bits,
                       reference_integer_bits(&b));
        reference_integer_divide(&expected_q, &expected_r, &a, &b);
        crime_integer_divide(&q, &r, &a, &b);
        assert_integer(&q, &expected_q);
        assert_integer(&r, &expected_r);
        crime_integer_divide(&alias_a, &alias_b, &alias_a, &alias_b);
        assert_integer(&alias_a, &q);
        assert_integer(&alias_b, &r);
        gcd = crime_integer_gcd(a, b);
        expected_q = reference_integer_gcd(a, b);
        assert_integer(&gcd, &expected_q);
        for (unsigned j = 0; j < ARRAY_SIZE(shifts); j++) {
            bool actual, reference;
            q = r = crime_integer(0x12345678);
            actual = crime_integer_shift(&q, &a, shifts[j]);
            reference = reference_integer_shift(&r, &a, shifts[j]);
            g_assert_cmpint(actual, ==, reference);
            assert_integer(&q, &r);
        }
        if (i < 64) {
            CrimeRational left = { .numerator = a, .denominator = b };
            CrimeRational right = crime_rational((int)i - 31, i + 1);
            CrimeRational actual = crime_rational(7, 11), expected = actual;
            bool ok, ref_ok;
            left.numerator.word[15] &= 0x7fffffff;
            left.negative = i & 1;
            g_assert_true(reference_rational_reduce(&left));
            ok = crime_rational_add(&actual, &left, &right);
            ref_ok = reference_rational_add(&expected, &left, &right);
            g_assert_cmpint(ok, ==, ref_ok);
            assert_rational(&actual, &expected);
            actual = expected = crime_rational(7, 11);
            ok = crime_rational_multiply(&actual, &left, &right);
            ref_ok = reference_rational_multiply(&expected, &left, &right);
            g_assert_cmpint(ok, ==, ref_ok);
            assert_rational(&actual, &expected);
        }
    }
}

static void test_divide_zero(void)
{
    if (g_test_subprocess()) {
        CrimeInteger a = crime_integer(1), zero = crime_integer(0), q, r;
        crime_integer_divide(&q, &r, &a, &zero);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
}

static void test_reference_divide_zero(void)
{
    if (g_test_subprocess()) {
        CrimeInteger a = crime_integer(1), zero = crime_integer(0), q, r;
        reference_integer_divide(&q, &r, &a, &zero);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_failed();
}

typedef uint64_t (*MathKernel)(unsigned operation, unsigned iteration);

#define MATH_KERNEL(name, prefix) \
static uint64_t __attribute__((__noinline__)) \
name(unsigned operation, unsigned iteration) \
{ \
    unsigned index = operation >= 4 ? \
        ARRAY_SIZE(crime_division_vectors) - 8 + iteration % 8 : \
        ARRAY_SIZE(crime_division_vectors) - 24 + iteration % 16; \
    const CrimeDivisionVector *v = &crime_division_vectors[index]; \
    CrimeInteger a = v->a, q, r; \
    a.word[0] ^= iteration; \
    if (operation == 0 || operation == 4) { \
        prefix ## _integer_divide(&q, &r, &a, &v->b); \
        return q.word[0] ^ r.word[0]; \
    } \
    if (operation == 1 || operation == 5) { \
        q = prefix ## _integer_gcd(a, v->b); \
        return q.word[0]; \
    } \
    const CrimeRationalVector *pair = \
        &crime_rational_vectors[7 + iteration % 24]; \
    CrimeRational result = crime_rational(7, 11); \
    bool ok = operation == 2 ? \
        prefix ## _rational_add(&result, &pair->a, &pair->b) : \
        prefix ## _rational_multiply(&result, &pair->a, &pair->b); \
    return result.numerator.word[0] ^ result.denominator.word[0] ^ \
           ((uint64_t)ok << 63) ^ ((uint64_t)result.negative << 62); \
}

MATH_KERNEL(math_current, crime)
MATH_KERNEL(math_reference, reference)

static int64_t math_measure(MathKernel kernel, unsigned operation,
                             uint64_t *checksum)
{
    int64_t start = g_get_monotonic_time();
    uint64_t result = 1;
    for (unsigned i = 0; i < 8192; i++) {
        result = (result ^ kernel(operation, i)) * UINT64_C(0x100000001b3);
    }
    *checksum = result;
    return g_get_monotonic_time() - start;
}

static void test_rational_performance(void)
{
    const char *names[] = {
        "divide-72-160", "gcd-72-160", "rational-add", "rational-multiply",
        "divide-256-512", "gcd-256-512",
    };
    if (!g_test_perf()) {
        g_test_skip("select -m perf for controlled old/new helper timings");
        return;
    }
    for (unsigned operation = 0; operation < ARRAY_SIZE(names); operation++) {
        for (unsigned round = 0; round < 3; round++) {
            uint64_t old_sum = 0, new_sum = 0;
            int64_t old_us, new_us;
            if (round & 1) {
                new_us = math_measure(math_current, operation, &new_sum);
                old_us = math_measure(math_reference, operation, &old_sum);
            } else {
                old_us = math_measure(math_reference, operation, &old_sum);
                new_us = math_measure(math_current, operation, &new_sum);
            }
            g_assert_cmphex(old_sum, ==, new_sum);
            g_test_message("%s round %u: old %" PRId64 " us, new %" PRId64
                           " us, ratio %.3f, checksum %016" PRIx64,
                           names[operation], round, old_us, new_us,
                           new_us ? (double)old_us / new_us : 0, new_sum);
        }
    }
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/crime/rational/small", test_rational_small);
    g_test_add_func("/crime/rational/limits", test_rational_limits);
    g_test_add_func("/crime/texture/constant-point", test_point_constant);
    g_test_add_func("/crime/rational/division-vectors", test_division_vectors);
    g_test_add_func("/crime/rational/python-vectors", test_rational_vectors);
    g_test_add_func("/crime/rational/random-reference", test_math_reference);
    g_test_add_func("/crime/rational/zero-divisor", test_divide_zero);
    g_test_add_func("/crime/rational/reference-zero-divisor",
                    test_reference_divide_zero);
    g_test_add_func("/crime/rational/performance", test_rational_performance);
    return g_test_run();
}
