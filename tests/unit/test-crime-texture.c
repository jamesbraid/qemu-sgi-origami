/* SPDX-License-Identifier: BSD-3-Clause */
/* Copyright (c) 2026 James Braid */
#include "qemu/osdep.h"
#include "qemu/host-utils.h"
#include "qemu/bswap.h"
#include "hw/mips/sgi/crime-rational.h"
#include "hw/mips/sgi/crime-texture.h"

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

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/crime/rational/small", test_rational_small);
    g_test_add_func("/crime/rational/limits", test_rational_limits);
    g_test_add_func("/crime/texture/constant-point", test_point_constant);
    return g_test_run();
}
