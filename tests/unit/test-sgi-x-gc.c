/* SPDX-License-Identifier: BSD-3-Clause */
#include "qemu/osdep.h"
#include "hw/display/sgi/x-gc.h"

static const uint32_t ci[] = {
    0x5a000, 0x1ffe, 0xa5000, 0x1ffe, 0xf00fff, 0,
    0x180c, 0x40001, 1, 6, 5, 1, 1,
};
static const uint32_t rgb[] = {
    0, 0, 0, 0, 0xffffff, 0xffffff,
    0x2804, 0x60001, 1, 6, 8, 0, 0,
};
static const uint32_t did[] = {
    0, 0, 0, 0, 0, 0xfc000, 8, 1, 0, 6, 0, 1, 1,
};

static void test_logic(void)
{
    static const unsigned truth[] = {
        0, 8, 4, 12, 2, 10, 6, 14, 1, 9, 5, 13, 3, 11, 7, 15,
    };
    const uint32_t source = 0x963ca569, dest = 0x5a81c30f;

    for (unsigned gx = 0; gx < 16; gx++) {
        uint32_t expected = 0;

        for (unsigned bit = 0; bit < 32; bit++) {
            unsigned index = (((source >> bit) & 1) << 1) |
                             ((dest >> bit) & 1);
            expected |= ((truth[gx] >> index) & 1U) << bit;
        }
        g_assert_cmphex(sgi_x_logic_op(gx, source, dest), ==, expected);
    }
    g_assert_cmphex(sgi_x_logic_op(16, source, dest), ==, source);
    g_assert_cmphex(sgi_x_logic_op(UINT_MAX, source, dest), ==, source);
}

static void test_ci(void)
{
    SGIXGC gc;
    uint32_t words[13];

    memcpy(words, ci, sizeof(words));
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_cmpint(gc.target, ==, SGI_X_GC_MAIN_CI8);
    g_assert_cmphex(gc.foreground, ==, 0x5a);
    g_assert_cmphex(gc.background, ==, 0xa5);
    g_assert_cmphex(gc.mask, ==, 0xff);
    g_assert_cmpmem(gc.raw, sizeof(gc.raw), words, sizeof(words));
    words[4] = 0xf0053f; /* Logical mask 0x53, alignment-fill bits 0xff. */
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_cmphex(gc.mask, ==, 0x53);
    g_assert_cmphex(sgi_x_gc_apply(&gc, 0x5a, 0xa5), ==, 0xf6);
    words[4] = 0xf0000f;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_true(sgi_x_gc_noop(&gc));
    words[4] = ci[4] ^ 1;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_MASK);
    words[4] = ci[4];
    words[0] |= 0x100;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_COLOR);
}

static void test_rgb(void)
{
    SGIXGC gc;
    uint32_t words[13];

    memcpy(words, rgb, sizeof(words));
    for (unsigned b = 0; b < 256; b++) {
        uint64_t component = (b << 4) | (b >> 4);
        uint64_t color = (component << 40) | (component << 27) |
                         (component << 14) | 0x1ffe;

        words[0] = color >> 32;
        words[1] = color;
        words[4] = component | (component << 12);
        words[5] = 0xfff000 | component;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
        g_assert_cmphex(gc.foreground, ==, b * 0x010101U);
        g_assert_cmphex(gc.mask, ==, b * 0x010101U);
    }
    memcpy(words, rgb, sizeof(words));
    for (unsigned m = 0; m < 4096; m++) {
        unsigned byte = m >> 4;
        bool canonical = m == ((byte << 4) | (byte >> 4));

        words[4] = 0xfff000 | m;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==,
                        canonical ? SGI_X_GC_OK : SGI_X_GC_BAD_MASK);
    }
    memcpy(words, rgb, sizeof(words));
    words[0] = 0xff000;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_COLOR);
    words[0] = 0;
    words[2] = 0xff000;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_COLOR);
}

static void test_did(void)
{
    SGIXGC gc;
    uint32_t words[13];

    for (unsigned v = 0; v <= 0xffff; v++) {
        memcpy(words, did, sizeof(words));
        words[0] = ((v & 15) << 3) | ((v & 0xfff0) << 4);
        words[4] = ((v >> 4) & 0xfff) | ((v & 15) << 20);
        words[5] = 0;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
        g_assert_cmphex(gc.foreground, ==, v);
        g_assert_cmphex(gc.mask, ==, v);
        words[0] = words[4] = 0;
        words[1] = ((v & 15) << 9) | ((v & 0xfff0) << 10);
        words[5] = ((v >> 12) & 15) | ((v & 0xfff) << 12);
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
        g_assert_cmphex(gc.foreground, ==, v);
        g_assert_cmphex(gc.mask, ==, v);
    }
    memcpy(words, did, sizeof(words));
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_cmphex(sgi_x_gc_apply(&gc, 0, 0xa5c3), ==, 0xa503);
    words[4] = 0x300a5c;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_MIXED_DID);
    words[5] = 0;
    words[0] = 0x80;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_COLOR);
    memcpy(words, did, sizeof(words));
    words[9] = 10;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_FIELDS);
}

static void test_classes(void)
{
    static const struct {
        uint32_t cfg;
        SGIXGCTarget target;
    } ancillary[] = {
        { 0x182c, SGI_X_GC_OVERLAY0 }, { 0x2c, SGI_X_GC_OVERLAY0 },
        { 0x102c, SGI_X_GC_OVERLAY0 }, { 0x1830, SGI_X_GC_OVERLAY1 },
        { 0x1820, SGI_X_GC_POPUP }, { 0x20, SGI_X_GC_POPUP },
        { 0x1020, SGI_X_GC_POPUP },
    };
    uint32_t words[13];
    SGIXGC gc;

    for (unsigned i = 0; i < ARRAY_SIZE(ancillary); i++) {
        memcpy(words, ci, sizeof(words));
        words[0] = 0x78;
        words[2] = 0x38;
        words[4] = ancillary[i].target == SGI_X_GC_POPUP ?
                   0x300000 : 0xf00000;
        words[6] = ancillary[i].cfg;
        words[7] = (words[6] & 0x3c) == 0x30 ? 2 : 1;
        words[8] = 0;
        if (words[6] < 0x1000) {
            words[10] = 0;
        }
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
        g_assert_cmpint(gc.target, ==, ancillary[i].target);
        g_assert_cmphex(gc.foreground, ==,
                        gc.target == SGI_X_GC_POPUP ? 3 : 15);
        g_assert_cmphex(gc.mask, ==,
                        gc.target == SGI_X_GC_POPUP ? 3 : 15);
    }
    memcpy(words, ci, sizeof(words));
    words[6] = 0xc;
    words[7] = 1;
    words[10] = 0;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_cmpint(gc.target, ==, SGI_X_GC_MAIN_CI8);
    words[0] = 0x93000;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_cmpint(gc.target, ==, SGI_X_GC_MAIN_CI8);
    words[4] |= 0x80000000U;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    g_assert_true(gc.opaque_or_tiled);
    memcpy(words, rgb, sizeof(words));
    words[7] = 0x7c001;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_FIELDS);
    words[6] = 0;
    words[7] = 0x64001;
    words[10] = 9;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_DEFERRED);
    memset(words, 0, sizeof(words));
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_FIELDS);
}

static void test_masks(void)
{
    SGIXGC gc;
    uint32_t words[13];

    for (unsigned gx = 0; gx < 16; gx++) {
        memcpy(words, ci, sizeof(words));
        words[9] = 2 * gx;
        words[4] = 0xf0053f;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
        g_assert_cmpuint(gc.gx, ==, gx);
        g_assert_cmphex(sgi_x_gc_apply(&gc, 0x195, 0x26a), ==,
                        (sgi_x_logic_op(gx, 0x95, 0x6a) & 0x53) |
                        (0x6a & 0xac));
        g_assert_cmpint(sgi_x_gc_noop(&gc), ==, gx == 5);
        g_assert_cmpint(sgi_x_gc_needs_destination(&gc), ==, gx != 5);
    }
    memcpy(words, ci, sizeof(words));
    for (unsigned gx = 0; gx < 16; gx++) {
        words[9] = 2 * gx;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
        g_assert_cmpint(sgi_x_gc_needs_destination(&gc), ==,
                        gx != 0 && gx != 3 && gx != 5 &&
                        gx != 12 && gx != 15);
    }
    for (unsigned bl = 1; bl < 32; bl += 2) {
        words[9] = bl;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_FIELDS);
    }
    words[9] = 32;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_FIELDS);
}

static void test_reserved(void)
{
    static const unsigned gaps[] = {
        0, 13, 26, 39, 52, 53, 54, 55, 56, 57, 58, 59, 60, 61, 62, 63,
    };
    uint32_t words[13];
    SGIXGC gc;

    memcpy(words, ci, sizeof(words));
    words[6] = 0x1820;
    words[7] = 1;
    words[8] = 0;
    words[4] = 0x300000;
    for (unsigned bit = 22; bit <= 23; bit++) {
        words[4] = 0x300000 | (1U << bit);
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_MASK);
        g_assert_cmpmem(gc.raw, sizeof(gc.raw), words, sizeof(words));
    }
    for (unsigned pair = 0; pair <= 2; pair += 2) {
        for (unsigned i = 0; i < ARRAY_SIZE(gaps); i++) {
            uint64_t value = 1ULL << gaps[i];

            memcpy(words, rgb, sizeof(words));
            words[pair] = value >> 32;
            words[pair + 1] = value;
            g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==,
                            SGI_X_GC_BAD_COLOR);
            g_assert_cmpmem(gc.raw, sizeof(gc.raw), words, sizeof(words));
        }
        memcpy(words, rgb, sizeof(words));
        words[pair + 1] = 2;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_BAD_COLOR);
        words[pair + 1] = 0x1ffe;
        g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_OK);
    }
    memcpy(words, ci, sizeof(words));
    words[6] = 0x30;
    words[7] = 2;
    words[8] = 0;
    words[10] = 0;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_DEFERRED);
    words[6] = 0x1030;
    words[10] = 5;
    g_assert_cmpint(sgi_x_gc_decode(words, &gc), ==, SGI_X_GC_DEFERRED);
}

int main(int argc, char **argv)
{
    g_test_init(&argc, &argv, NULL);
    g_test_add_func("/sgi/x-gc/logic", test_logic);
    g_test_add_func("/sgi/x-gc/ci", test_ci);
    g_test_add_func("/sgi/x-gc/rgb", test_rgb);
    g_test_add_func("/sgi/x-gc/did", test_did);
    g_test_add_func("/sgi/x-gc/classes", test_classes);
    g_test_add_func("/sgi/x-gc/masks", test_masks);
    g_test_add_func("/sgi/x-gc/reserved", test_reserved);
    return g_test_run();
}
