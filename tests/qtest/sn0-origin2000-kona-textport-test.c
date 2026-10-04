/* SPDX-License-Identifier: BSD-3-Clause */
/*
 * SGI KONA textport bitmap-placement qtests.
 *
 * sn0-origin2000-kona-test.c owns the textport's protocol, register and
 * reset/migration coverage. This file owns the bitmap command (0xa)'s own
 * placement arithmetic: sgi_kona_bitmap_blit() in hw/misc/sgi-kona.c places
 * a payload relative to the textport's raster position (command 4) minus
 * the bitmap's own origin, then advances the raster position by the
 * bitmap's own advance -- replacing an earlier model that placed header
 * words 2/3 as absolute coordinates and piled every one of a real boot's
 * 308 bitmap transfers into a single corner of the framebuffer (rows
 * 987-1023, columns 0-37 of a 1280x1024 surface).
 *
 * The header/payload data below is the real "Start System" menu label from
 * a captured PROM boot (its +0xc10000 write trace, replayed through the
 * textport's own framing by a one-off analysis script rather than typed
 * by hand), not synthetic data: it is real evidence that this placement
 * renders a real label legibly, not merely a self-consistency check of the
 * code against itself. The label is identified by its twelve glyphs, a
 * fixed left margin matching five other menu labels in the same trace,
 * and a glyph count and blank-glyph position matching "Start System"
 * letter-for-letter.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "qobject/qdict.h"
#include "qobject/qjson.h"
#include "sgi-test.h"

#define SMALL_WINDOW_SIZE UINT64_C(0x1000000)
#define SMALL_WINDOW(widget) \
    (SPACE_IO + (uint64_t)(widget) * SMALL_WINDOW_SIZE)

#define KONA_WIDGET 11
#define KONA_BASE   SMALL_WINDOW(KONA_WIDGET)
#define KONA_REG(offset) (KONA_BASE + (offset))
#define REG_TEXTPORT KONA_REG(0xc10000)

#define MACHINE_ARGS \
    "-M origin2000,nodes=1 -smp 1 -m 64M " \
    "-device sgi-kona,id=kona0,slot=io4"

#define KONA_QOM_PATH "/machine/module0/kona0"

/* The framebuffer's own bottom-left corner artifact the fix removes. */
#define KONA_FB_WIDTH  1280
#define KONA_FB_HEIGHT 1024
#define CORNER_ROW_FIRST 987
#define CORNER_COL_LAST  37

static QTestState *qts;

/*
 * sn0-origin2000-kona-test.c's own kona_qom_get_uint(): no shared header
 * carries it, and libqtest.h has no generic uint getter.
 */
static uint64_t kona_qom_get_uint(QTestState *s, const char *property)
{
    QDict *resp = qtest_qmp(s,
        "{'execute': 'qom-get', 'arguments': {'path': %s, "
        "'property': %s}}", KONA_QOM_PATH, property);
    uint64_t value;

    if (!qdict_haskey(resp, "return")) {
        g_autoptr(GString) str = qobject_to_json(QOBJECT(resp));
        g_error("qom-get %s failed: %s", property, str->str);
    }
    value = qdict_get_int(resp, "return");
    qobject_unref(resp);
    return value;
}

static uint32_t kona_test_pixel(QTestState *s, uint32_t x, uint32_t y)
{
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-x', 'value': %u}}", KONA_QOM_PATH, x);
    qtest_qmp_assert_success(s,
        "{'execute': 'qom-set', 'arguments': {'path': %s, "
        "'property': 'x-test-pixel-y', 'value': %u}}", KONA_QOM_PATH, y);
    return kona_qom_get_uint(s, "x-test-pixel");
}

/*
 * sn0-origin2000-kona-test.c's own kona_textport_push(): push one whole
 * command (sync word, selector, argc arguments) in a single call.
 */
static void kona_textport_push(QTestState *s, uint32_t selector,
                               const uint32_t *args, unsigned argc)
{
    unsigned i;

    qtest_writel(s, REG_TEXTPORT, 1);
    qtest_writel(s, REG_TEXTPORT, selector);
    for (i = 0; i < argc; i++) {
        qtest_writel(s, REG_TEXTPORT, args[i]);
    }
}

static void kona_reset(void)
{
    qtest_system_reset(qts);
}

/*
 * One glyph's own header + payload, exactly as captured from the real
 * trace: width, height, x origin, y origin, x advance, y advance, then
 * the packed 1-bpp payload (MSB first, row-major, no row padding).
 */
typedef struct KonaGlyph {
    uint32_t width, height;
    int32_t xorigin, yorigin;
    int32_t xadvance, yadvance;
    const uint32_t *payload;
} KonaGlyph;

static const uint32_t glyph_s_payload[] = {
    0x9fc3c1cc, 0x1b00780, 0x1e00f01f, 0xc3ff3ff9,
    0xffcffc3f, 0x80f00b80, 0x2e019806, 0x38783f20,
};
static const uint32_t glyph_t_payload[] = {
    0xf07e3c4, 0xf13c0f03, 0xc0f03c0f, 0x3c3fe3c, 0x700c030, 0x4000000,
};
static const uint32_t glyph_a_payload[] = {
    0x7e3be7ff, 0xf3c3cf0, 0xf1e3c0ef, 0x7c60f3, 0xc3cf1e1f, 0xe0000000,
};
static const uint32_t glyph_r_payload[] = {
    0xff03c03c, 0x3c03c03, 0xc03c03c0, 0x3c63ef3d, 0xffce0000,
};
static const uint32_t glyph_space_payload[] = { 0x0 };
static const uint32_t glyph_y_payload[] = {
    0x7800fe00, 0xf3006100, 0x1800180, 0x1c003c0,
    0x3c007e0, 0x7a00f30, 0xf101e18, 0x1e083c0c, 0xfe3f0000,
};
static const uint32_t glyph_s2_payload[] = {
    0xbf386c0e, 0x70fdfff, 0xfbf0e0b0, 0x6638fa00,
};
static const uint32_t glyph_e_payload[] = {
    0xfe0f0c7, 0x809e02f0, 0x3c00fff, 0xfc0f7039, 0xe1e38703, 0xf0000000,
};
static const uint32_t glyph_m_payload[] = {
    0xfe7e7f3c, 0x3c3c3c3c, 0x3c3c3c3c, 0x3c3c3c3c,
    0x3c3c3c3c, 0x3c3c3c3c, 0x3c3c3c3e, 0x7e7c3df9, 0xf8fcf0f0,
};

/*
 * "Start System": S-t-a-r-t- -S-y-s-t-e-m, twelve glyphs, event index 6833
 * of the parsed trace, raster position (210, 861). Every yadvance is 0:
 * the whole label sits on one line, only x advances between glyphs.
 */
static const KonaGlyph start_system_label[] = {
    { 14, 18, -1, 0, 16, 0, glyph_s_payload },     /* S */
    { 10, 17,  0, 0, 10, 0, glyph_t_payload },     /* t */
    { 14, 12,  0, 0, 15, 0, glyph_a_payload },     /* a */
    { 12, 12,  0, 0, 12, 0, glyph_r_payload },     /* r */
    { 10, 17,  0, 0, 10, 0, glyph_t_payload },     /* t */
    {  1,  1,  0, 0,  7, 0, glyph_space_payload }, /* space */
    { 14, 18, -1, 0, 16, 0, glyph_s_payload },     /* S */
    { 16, 17,  1, 5, 15, 0, glyph_y_payload },     /* y */
    { 10, 12,  0, 0, 11, 0, glyph_s2_payload },    /* s */
    { 10, 17,  0, 0, 10, 0, glyph_t_payload },     /* t */
    { 14, 12,  0, 0, 15, 0, glyph_e_payload },     /* e */
    { 24, 12,  0, 0, 25, 0, glyph_m_payload },     /* m */
};

#define START_SYSTEM_RASTER_X 210
#define START_SYSTEM_RASTER_Y 861
#define START_SYSTEM_COLOR 0x00112233

/*
 * Assert every pixel a glyph's own payload covers renders (or does not,
 * for a zero payload bit) exactly where sgi_kona_bitmap_blit()'s own
 * documented formula places it: pixel (c, r) of the payload lands at
 * (raster_x - xorigin + c, raster_y - yorigin + r).
 */
static void kona_assert_glyph_pixels(QTestState *s, const KonaGlyph *g,
                                     int32_t raster_x, int32_t raster_y,
                                     uint32_t color)
{
    uint32_t r, c;

    for (r = 0; r < g->height; r++) {
        for (c = 0; c < g->width; c++) {
            uint32_t bit = r * g->width + c;
            uint32_t word = g->payload[bit / 32];
            uint32_t set = (word >> (31 - (bit % 32))) & 1;
            int32_t x = raster_x - g->xorigin + (int32_t)c;
            int32_t y = raster_y - g->yorigin + (int32_t)r;

            g_assert_cmpint(x, >=, 0);
            g_assert_cmpint(y, >=, 0);
            g_assert_cmphex(kona_test_pixel(s, (uint32_t)x, (uint32_t)y), ==,
                            set ? color : 0);
        }
    }
}

/*
 * Replay "Start System" from real captured traffic: one raster-position
 * push, then the label's twelve glyphs back to back with no further
 * selector-4 or selector-2 push between them -- exactly the order the
 * real trace shows (colour is set once, before the first glyph, and
 * persists; only the first "S" and the space glyph get a fresh raster
 * position, the rest chain from advancing it). Each glyph's own bitmap
 * pixels must land exactly where the formula computes, proving the fix
 * against real firmware-shaped traffic rather than a hand-built fixture.
 */
static void test_textport_start_system_label_renders(void)
{
    int32_t raster_x = START_SYSTEM_RASTER_X;
    int32_t raster_y = START_SYSTEM_RASTER_Y;
    unsigned i;

    kona_reset();

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 0, 0x11 << 2, 0x22 << 2, 0x33 << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 0 }, 1);
    kona_textport_push(qts, 0x4,
                       (uint32_t[]){ (uint32_t)raster_x, (uint32_t)raster_y },
                       2);

    for (i = 0; i < G_N_ELEMENTS(start_system_label); i++) {
        const KonaGlyph *g = &start_system_label[i];
        unsigned words = (g->width * g->height + 31) / 32;
        unsigned w;

        kona_textport_push(qts, 0xa,
                           (uint32_t[]){ g->width, g->height,
                                         (uint32_t)g->xorigin,
                                         (uint32_t)g->yorigin,
                                         (uint32_t)g->xadvance,
                                         (uint32_t)g->yadvance }, 6);
        for (w = 0; w < words; w++) {
            qtest_writel(qts, REG_TEXTPORT, g->payload[w]);
        }

        kona_assert_glyph_pixels(qts, g, raster_x, raster_y,
                                 START_SYSTEM_COLOR);

        raster_x += g->xadvance;
        raster_y += g->yadvance;
    }
}

/*
 * The committed-before-this-fix model placed header words 2/3 as absolute
 * coordinates, so every one of a real boot's 308 bitmap transfers piled
 * into one corner: rows 987-1023, columns 0-37 of the 1280x1024 surface.
 * The label replayed above sits nowhere near there (x 210-372, y
 * 856-878), so confirm the corner itself stays untouched rather than only
 * checking the label's own pixels.
 */
static void test_textport_bitmap_leaves_no_corner_artifact(void)
{
    uint32_t x, y;
    unsigned i;

    kona_reset();

    kona_textport_push(qts, 0x3,
                       (uint32_t[]){ 0, 0x11 << 2, 0x22 << 2, 0x33 << 2 }, 4);
    kona_textport_push(qts, 0x2, (uint32_t[]){ 0 }, 1);
    kona_textport_push(qts, 0x4,
                       (uint32_t[]){ START_SYSTEM_RASTER_X,
                                     START_SYSTEM_RASTER_Y }, 2);

    for (i = 0; i < G_N_ELEMENTS(start_system_label); i++) {
        const KonaGlyph *g = &start_system_label[i];
        unsigned words = (g->width * g->height + 31) / 32;
        unsigned w;

        kona_textport_push(qts, 0xa,
                           (uint32_t[]){ g->width, g->height,
                                         (uint32_t)g->xorigin,
                                         (uint32_t)g->yorigin,
                                         (uint32_t)g->xadvance,
                                         (uint32_t)g->yadvance }, 6);
        for (w = 0; w < words; w++) {
            qtest_writel(qts, REG_TEXTPORT, g->payload[w]);
        }
    }

    for (y = CORNER_ROW_FIRST; y < KONA_FB_HEIGHT; y++) {
        for (x = 0; x <= CORNER_COL_LAST; x++) {
            g_assert_cmphex(kona_test_pixel(qts, x, y), ==, 0);
        }
    }
}

/*
 * Command 4 (raster position) stores and draws nothing of its own: a
 * push at a position no other case in this file touches must change no
 * pixel there, before or after.
 */
static void test_textport_raster_position_alone_renders_nothing(void)
{
    kona_reset();

    g_assert_cmphex(kona_test_pixel(qts, 999, 5), ==, 0);
    kona_textport_push(qts, 0x4, (uint32_t[]){ 999, 5 }, 2);
    g_assert_cmphex(kona_test_pixel(qts, 999, 5), ==, 0);
}

/*
 * sgi_kona_bitmap_blit()'s own placement and advance: selector 4 (raster
 * position) takes a full 32-bit guest argument with no known range (only
 * its argument shape is known, nothing about a valid range or a hardware
 * clamp), while this bitmap's own header fields are 16-bit range. Combining
 * a raster position near INT32_MAX with them in plain int32 arithmetic is
 * signed-integer-overflow undefined behaviour in C.
 *
 * This drives the raster position to INT32_MAX and blits a two-pixel-wide
 * bitmap whose second column alone overflows (xorigin 0, so column 1 lands
 * at INT32_MAX + 1); both payload bits are set. A regression back to plain
 * int32 arithmetic, or a fix that clamped and drew at a screen edge instead
 * of rejecting the overflow outright, would show up at the wrap/clamp
 * targets checked below -- nothing may land there, and the overflow must be
 * logged rather than left to undefined behaviour.
 *
 * qemu_log_mask(LOG_GUEST_ERROR, ...) only reaches stderr when guest-error
 * logging is enabled, so this runs its own private QTestState with
 * "-d guest_errors" (the convention other qtest files use for asserting a
 * LOG_GUEST_ERROR message, e.g. sn1-xbridge-test.c's transport_start())
 * rather than reusing the shared qts the other subtests in this file run
 * without that flag.
 */
static void test_textport_raster_overflow_draws_nothing_and_logs(void)
{
    if (g_test_subprocess()) {
        QTestState *s = qtest_init(MACHINE_ARGS " -d guest_errors");

        qtest_system_reset(s);

        kona_textport_push(s, 0x3,
                           (uint32_t[]){ 0, 0x11 << 2, 0x22 << 2, 0x33 << 2 },
                           4);
        kona_textport_push(s, 0x2, (uint32_t[]){ 0 }, 1);
        kona_textport_push(s, 0x4, (uint32_t[]){ (uint32_t)INT32_MAX, 0 }, 2);
        kona_textport_push(s, 0xa, (uint32_t[]){ 2, 1, 0, 0, 0, 0 }, 6);
        qtest_writel(s, REG_TEXTPORT, 0xc0000000);

        g_assert_cmphex(kona_test_pixel(s, 0, 0), ==, 0);
        g_assert_cmphex(kona_test_pixel(s, KONA_FB_WIDTH - 1, 0), ==, 0);

        qtest_quit(s);
        return;
    }
    g_test_trap_subprocess(NULL, 0, 0);
    g_test_trap_assert_passed();
    g_test_trap_assert_stderr(
        "*overflows signed 32-bit bitmap placement arithmetic*");
}

int main(int argc, char **argv)
{
    int ret;

    g_test_init(&argc, &argv, NULL);
    qts = qtest_init(MACHINE_ARGS);

    qtest_add_func("sn0/origin2000/kona/textport/"
                   "start-system-label-renders",
                   test_textport_start_system_label_renders);
    qtest_add_func("sn0/origin2000/kona/textport/"
                   "bitmap-leaves-no-corner-artifact",
                   test_textport_bitmap_leaves_no_corner_artifact);
    qtest_add_func("sn0/origin2000/kona/textport/"
                   "raster-position-alone-renders-nothing",
                   test_textport_raster_position_alone_renders_nothing);
    qtest_add_func("sn0/origin2000/kona/textport/"
                   "raster-overflow-draws-nothing-and-logs",
                   test_textport_raster_overflow_draws_nothing_and_logs);

    ret = g_test_run();
    qtest_quit(qts);
    return ret;
}
